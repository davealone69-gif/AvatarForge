/*
 * af_glb_read.cpp -- glTF 2.0 GLB importer.
 *
 * Layout of the file this reads (glTF 2.0 spec section 4):
 *
 *      u32 magic 'glTF' | u32 version | u32 totalLength
 *      [ u32 chunkLength | u32 chunkType | payload ... ] * 1..n
 *      chunk type 0x4E4F534A = JSON, 0x004E4942 = BIN
 *
 * Everything below is written defensively in the same style: validate the container, parse the
 * JSON, then resolve every index the document contains *before* dereferencing anything. The
 * accessor reader is the security-critical part - a glTF accessor carries a byteOffset, a count, a
 * stride and a component type, and every combination of those can point anywhere - so it is one
 * function that does all of the range checking in 64-bit arithmetic in one place, and every other
 * part of the importer goes through it.
 */
#include "af_glb_read.h"

#include "af_json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace af {
namespace {

constexpr uint32_t GLB_MAGIC = 0x46546C67u;   /* 'glTF' little-endian */
constexpr uint32_t CHUNK_JSON = 0x4E4F534Au;
constexpr uint32_t CHUNK_BIN = 0x004E4942u;
constexpr int MAX_NODE_DEPTH = 256;
constexpr size_t MAX_ACCESSOR_FLOATS = 8u * 1024u * 1024u;
constexpr size_t MAX_ACCESSOR_INDICES = 8u * 1024u * 1024u;

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t readUnsignedLE(const uint8_t* p, int size) {
    if (size == 1) return p[0];
    if (size == 2) return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
    return rd32(p);
}

int componentSize(int componentType) {
    switch (componentType) {
        case 5120: return 1;  /* BYTE */
        case 5121: return 1;  /* UNSIGNED_BYTE */
        case 5122: return 2;  /* SHORT */
        case 5123: return 2;  /* UNSIGNED_SHORT */
        case 5125: return 4;  /* UNSIGNED_INT */
        case 5126: return 4;  /* FLOAT */
        default: return 0;
    }
}

bool checkedAdd(size_t a, size_t b, size_t* out) {
    if (!out || b > std::numeric_limits<size_t>::max() - a) return false;
    *out = a + b;
    return true;
}

bool checkedMul(size_t a, size_t b, size_t* out) {
    if (!out || (a != 0 && b > std::numeric_limits<size_t>::max() / a)) return false;
    *out = a * b;
    return true;
}

bool checkedRange(size_t offset, size_t length, size_t limit) {
    return offset <= limit && length <= limit - offset;
}

int typeComponents(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

/* Decodes one component to float, applying the glTF normalisation rules. */
float readComponent(const uint8_t* p, int componentType, bool normalized) {
    switch (componentType) {
        case 5120: {
            const int8_t v = static_cast<int8_t>(*p);
            return normalized ? static_cast<float>(v) / 127.0f : static_cast<float>(v);
        }
        case 5121: {
            const uint8_t v = *p;
            return normalized ? static_cast<float>(v) / 255.0f : static_cast<float>(v);
        }
        case 5122: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<float>(v) / 32767.0f : static_cast<float>(v);
        }
        case 5123: {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
        }
        case 5125: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v);
        }
        case 5126: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        default:
            return 0.0f;
    }
}

/* ------------------------------------------------------------------ the accessor reader ---- */
/*
 * A resolved accessor: element count, components per element, byte stride, and a pointer to the
 * first element. All range checks happen in resolveAccessor(); readElement() below can then do no
 * arithmetic that is not local to one element.
 */
struct AccessorView {
    const uint8_t* base = nullptr;
    size_t count = 0;
    int components = 0;
    int componentType = 0;
    size_t stride = 0;
    bool normalized = false;
    bool valid = false;
};

int resolveAccessor(const uint8_t* bin, size_t binSize, const json::Json& doc, int accessorIndex,
                    AccessorView* view, std::string* error) {
    if (!view || accessorIndex < 0) {
        if (error) *error = "glTF: accessor index out of range";
        return AF_ERR_INVALID_ARG;
    }
    const json::Json& acc = doc["accessors"].atOrNull(static_cast<size_t>(accessorIndex));
    if (acc.isNull()) {
        if (error) *error = "glTF: accessor index out of range";
        return AF_ERR_INVALID_ARG;
    }
    size_t count = 0;
    if (!acc["count"].asIndex(&count) || count == 0) {
        if (error) *error = "glTF: accessor count must be a positive integer";
        return AF_ERR_INVALID_ARG;
    }
    const int componentType = acc["componentType"].asInt(0);
    const int csize = componentSize(componentType);
    const int components = typeComponents(acc["type"].stringOr(""));
    if (csize == 0 || components == 0) {
        if (error) *error = "glTF: accessor with an unknown component type or element type";
        return AF_ERR_INVALID_ARG;
    }
    size_t elementBytes = 0;
    if (!checkedMul(static_cast<size_t>(csize), static_cast<size_t>(components), &elementBytes)) {
        if (error) *error = "glTF: accessor element size overflows";
        return AF_ERR_INVALID_ARG;
    }

    view->count = count;
    view->components = components;
    view->componentType = componentType;
    view->normalized = acc["normalized"].asBool(false);
    view->stride = elementBytes;
    view->valid = true;

    const json::Json& bvRef = acc["bufferView"];
    if (!acc.find("bufferView")) {
        if (!acc["sparse"].isObject()) {
            if (error) *error = "glTF: accessor without a bufferView must have sparse data";
            return AF_ERR_INVALID_ARG;
        }
        view->base = nullptr;
        return AF_OK;
    }
    if (!bvRef.isNumber()) {
        if (error) *error = "glTF: accessor bufferView reference is not an integer";
        return AF_ERR_INVALID_ARG;
    }
    const int bvIndex = bvRef.asInt(-1);
    if (bvIndex < 0) {
        if (error) *error = "glTF: accessor bufferView index is invalid";
        return AF_ERR_INVALID_ARG;
    }
    const json::Json& bvJson = doc["bufferViews"].atOrNull(static_cast<size_t>(bvIndex));
    if (bvJson.isNull()) {
        if (error) *error = "glTF: accessor bufferView index out of range";
        return AF_ERR_INVALID_ARG;
    }

    size_t bvOffset = 0, bvLength = 0;
    if (!bvJson["byteOffset"].isNull() && !bvJson["byteOffset"].asIndex(&bvOffset)) {
        if (error) *error = "glTF: bufferView has an invalid byteOffset";
        return AF_ERR_INVALID_ARG;
    }
    if (!bvJson["byteLength"].asIndex(&bvLength) || !checkedRange(bvOffset, bvLength, binSize)) {
        if (error) *error = "glTF: bufferView range exceeds the binary buffer";
        return AF_ERR_INVALID_ARG;
    }
    size_t bufferIndex = 0;
    if (!bvJson["buffer"].asIndex(&bufferIndex) || bufferIndex != 0) {
        if (error) *error = "glTF: bufferView references an unavailable buffer";
        return AF_ERR_UNSUPPORTED;
    }

    size_t byteOffset = 0;
    if (!acc["byteOffset"].isNull() && !acc["byteOffset"].asIndex(&byteOffset)) {
        if (error) *error = "glTF: accessor has an invalid byteOffset";
        return AF_ERR_INVALID_ARG;
    }
    size_t stride = elementBytes;
    if (bvJson.find("byteStride")) {
        if (!bvJson["byteStride"].asIndex(&stride) || stride < 4 || stride > 252 || (stride % 4) != 0) {
            if (error) *error = "glTF: byteStride must be a multiple of 4 in the range 4..252";
            return AF_ERR_INVALID_ARG;
        }
        if (stride < elementBytes) {
            if (error) *error = "glTF: byteStride smaller than the element size";
            return AF_ERR_INVALID_ARG;
        }
    }

    size_t lastOffset = 0, span = 0, absoluteStart = 0;
    if (byteOffset > bvLength ||
        !checkedMul(count - 1, stride, &lastOffset) ||
        !checkedAdd(lastOffset, elementBytes, &span) ||
        !checkedRange(byteOffset, span, bvLength) ||
        !checkedAdd(bvOffset, byteOffset, &absoluteStart) ||
        (absoluteStart % static_cast<size_t>(csize)) != 0 ||
        !checkedRange(absoluteStart, span, binSize)) {
        if (error) *error = "glTF: accessor offset/span overflows or exceeds its bufferView";
        return AF_ERR_INVALID_ARG;
    }
    if (!bin) {
        if (error) *error = "glTF: accessor requires a missing binary chunk";
        return AF_ERR_INVALID_ARG;
    }
    view->base = bin + absoluteStart;
    view->stride = stride;
    return AF_OK;
}

/* Reads element `i` as up to 16 floats. Bounds were proven by resolveAccessor(). */
bool readElement(const AccessorView& view, size_t i, float* out, int outComponents) {
    if (!out || i >= view.count || outComponents < 0 || outComponents > 16) return false;
    for (int c = 0; c < outComponents; ++c) out[c] = 0.0f;
    if (!view.base) return true; /* sparse-only: zeros are the glTF default before applying the patch */
    size_t elementOffset = 0;
    if (!checkedMul(i, view.stride, &elementOffset)) return false;
    const int n = view.components < outComponents ? view.components : outComponents;
    const size_t csize = static_cast<size_t>(componentSize(view.componentType));
    for (int c = 0; c < n; ++c) {
        size_t componentOffset = 0, readOffset = 0;
        if (!checkedMul(static_cast<size_t>(c), csize, &componentOffset) ||
            !checkedAdd(elementOffset, componentOffset, &readOffset)) return false;
        out[c] = readComponent(view.base + readOffset, view.componentType, view.normalized);
    }
    return true;
}

/* Applies an accessor's sparse patch over a dense float buffer of `count * components`. */
int applySparse(const uint8_t* bin, size_t binSize, const json::Json& doc, const json::Json& acc,
                size_t count, int components, float* buffer, std::string* error) {
    const json::Json& sparse = acc["sparse"];
    if (sparse.isNull()) return AF_OK;
    size_t sparseCount = 0;
    if (!sparse["count"].asIndex(&sparseCount) || sparseCount == 0 || sparseCount > count) {
        if (error) *error = "glTF: sparse patch count is invalid";
        return AF_ERR_INVALID_ARG;
    }
    if (!bin || !buffer || components <= 0) {
        if (error) *error = "glTF: sparse patch has no valid data buffer";
        return AF_ERR_INVALID_ARG;
    }

    auto resolveSparseView = [&](const json::Json& ref, size_t* offset, size_t* length) -> bool {
        size_t index = 0;
        if (!ref.asIndex(&index)) return false;
        const json::Json& bv = doc["bufferViews"].atOrNull(index);
        if (bv.isNull()) return false;
        size_t bufferIndex = 0;
        *offset = 0;
        if (!bv["buffer"].asIndex(&bufferIndex) || bufferIndex != 0) return false;
        if (!bv["byteOffset"].isNull() && !bv["byteOffset"].asIndex(offset)) return false;
        if (!bv["byteLength"].asIndex(length) || !checkedRange(*offset, *length, binSize)) return false;
        /* glTF sparse index/value bufferViews are tightly packed and may not declare a stride. */
        if (bv.find("byteStride")) return false;
        return true;
    };

    const int idxComponent = sparse["indices"]["componentType"].asInt(0);
    const int idxSize = componentSize(idxComponent);
    if (idxComponent != 5121 && idxComponent != 5123 && idxComponent != 5125) {
        if (error) *error = "glTF: sparse indices must use UNSIGNED_BYTE, UNSIGNED_SHORT, or UNSIGNED_INT";
        return AF_ERR_INVALID_ARG;
    }
    size_t idxBvOffset = 0, idxBvLength = 0;
    if (!resolveSparseView(sparse["indices"]["bufferView"], &idxBvOffset, &idxBvLength)) {
        if (error) *error = "glTF: sparse indices reference an invalid bufferView";
        return AF_ERR_INVALID_ARG;
    }
    size_t idxByteOffset = 0, idxStart = 0, idxSpan = 0;
    if ((!sparse["indices"]["byteOffset"].isNull() &&
         !sparse["indices"]["byteOffset"].asIndex(&idxByteOffset)) ||
        !checkedMul(sparseCount, static_cast<size_t>(idxSize), &idxSpan) ||
        !checkedRange(idxByteOffset, idxSpan, idxBvLength) ||
        !checkedAdd(idxBvOffset, idxByteOffset, &idxStart) ||
        !checkedRange(idxStart, idxSpan, binSize)) {
        if (error) *error = "glTF: sparse index offset/span overflows or exceeds its bufferView";
        return AF_ERR_INVALID_ARG;
    }
    const uint8_t* idxBase = bin + idxStart;

    size_t valBvOffset = 0, valBvLength = 0;
    if (!resolveSparseView(sparse["values"]["bufferView"], &valBvOffset, &valBvLength)) {
        if (error) *error = "glTF: sparse values reference an invalid bufferView";
        return AF_ERR_INVALID_ARG;
    }
    size_t valByteOffset = 0, valStart = 0, valSpan = 0, valueComponents = 0;
    if ((!sparse["values"]["byteOffset"].isNull() &&
         !sparse["values"]["byteOffset"].asIndex(&valByteOffset)) ||
        !checkedMul(sparseCount, static_cast<size_t>(components), &valueComponents) ||
        !checkedMul(valueComponents, static_cast<size_t>(componentSize(static_cast<int>(acc["componentType"].asInt(0)))), &valSpan) ||
        !checkedRange(valByteOffset, valSpan, valBvLength) ||
        !checkedAdd(valBvOffset, valByteOffset, &valStart) ||
        !checkedRange(valStart, valSpan, binSize)) {
        if (error) *error = "glTF: sparse value offset/span overflows or exceeds its bufferView";
        return AF_ERR_INVALID_ARG;
    }
    const int componentType = static_cast<int>(acc["componentType"].asInt(0));
    const int csize = componentSize(componentType);
    if (csize == 0) {
        if (error) *error = "glTF: sparse values have an unsupported component type";
        return AF_ERR_INVALID_ARG;
    }
    const uint8_t* valBase = bin + valStart;
    const bool normalized = acc["normalized"].asBool(false);
    for (size_t i = 0; i < sparseCount; ++i) {
        size_t target = 0, indexOffset = 0;
        if (!checkedMul(i, static_cast<size_t>(idxSize), &indexOffset) ||
            !checkedRange(indexOffset, static_cast<size_t>(idxSize), idxSpan)) {
            if (error) *error = "glTF: sparse index read offset overflows";
            return AF_ERR_INVALID_ARG;
        }
        if (idxSize == 1) target = idxBase[indexOffset];
        else if (idxSize == 2) {
            uint16_t v;
            std::memcpy(&v, idxBase + indexOffset, sizeof(v));
            target = v;
        } else {
            uint32_t v;
            std::memcpy(&v, idxBase + indexOffset, sizeof(v));
            target = v;
        }
        if (target >= count) {
            if (error) *error = "glTF: sparse index beyond the accessor";
            return AF_ERR_INVALID_ARG;
        }
        size_t targetBase = 0;
        if (!checkedMul(target, static_cast<size_t>(components), &targetBase)) {
            if (error) *error = "glTF: sparse destination index overflows";
            return AF_ERR_INVALID_ARG;
        }
        size_t sourceBase = 0;
        if (!checkedMul(i, static_cast<size_t>(components), &sourceBase) ||
            !checkedMul(sourceBase, static_cast<size_t>(csize), &sourceBase)) {
            if (error) *error = "glTF: sparse source index overflows";
            return AF_ERR_INVALID_ARG;
        }
        for (int c = 0; c < components; ++c) {
            size_t sourceComponent = 0, targetComponent = 0, sourceOffset = 0;
            if (!checkedMul(static_cast<size_t>(c), static_cast<size_t>(csize), &sourceComponent) ||
                !checkedAdd(sourceBase, sourceComponent, &sourceOffset) ||
                !checkedAdd(targetBase, static_cast<size_t>(c), &targetComponent)) {
                if (error) *error = "glTF: sparse component offset overflows";
                return AF_ERR_INVALID_ARG;
            }
            buffer[targetComponent] = readComponent(valBase + sourceOffset, componentType, normalized);
        }
    }
    return AF_OK;
}

/* Reads an accessor into a float buffer of count*components, applying sparse patches. */
int readAccessorFloats(const uint8_t* bin, size_t binSize, const json::Json& doc, int accessorIndex,
                       int components, std::vector<float>* out, size_t* outCount, std::string* error) {
    const json::Json& acc = doc["accessors"].atOrNull(static_cast<size_t>(accessorIndex < 0 ? 0 : accessorIndex));
    if (accessorIndex < 0 || acc.isNull()) {
        if (error) *error = "glTF: accessor index out of range";
        return AF_ERR_INVALID_ARG;
    }
    AccessorView view;
    const int rc = resolveAccessor(bin, binSize, doc, accessorIndex, &view, error);
    if (rc != AF_OK) return rc;
    if (view.components != components) {
        if (error) *error = "glTF: accessor component count does not match the requested attribute type";
        return AF_ERR_INVALID_ARG;
    }
    size_t floatCount = 0;
    if (!checkedMul(view.count, static_cast<size_t>(components), &floatCount) ||
        floatCount > out->max_size() || floatCount > MAX_ACCESSOR_FLOATS) {
        if (error) *error = "glTF: accessor output allocation size overflows or exceeds the importer limit";
        return AF_ERR_INVALID_ARG;
    }
    out->assign(floatCount, 0.0f);
    for (size_t i = 0; i < view.count; ++i) {
        float v[16];
        if (!readElement(view, i, v, components)) {
            if (error) *error = "glTF: accessor element offset overflows";
            return AF_ERR_INVALID_ARG;
        }
        size_t destBase = 0;
        if (!checkedMul(i, static_cast<size_t>(components), &destBase)) {
            if (error) *error = "glTF: accessor destination offset overflows";
            return AF_ERR_INVALID_ARG;
        }
        for (int c = 0; c < components; ++c) {
            size_t dest = 0;
            if (!checkedAdd(destBase, static_cast<size_t>(c), &dest) || dest >= out->size()) {
                if (error) *error = "glTF: accessor destination span is invalid";
                return AF_ERR_INVALID_ARG;
            }
            (*out)[dest] = v[c];
        }
    }
    if (applySparse(bin, binSize, doc, acc, view.count, components, out->data(), error) != AF_OK) return AF_ERR_INVALID_ARG;
    for (float value : *out) {
        if (!std::isfinite(value)) {
            if (error) *error = "glTF: accessor contains a non-finite value";
            return AF_ERR_INVALID_ARG;
        }
    }
    *outCount = view.count;
    return AF_OK;
}

/* Decode index accessors as integer data end-to-end. Going through float loses UINT32 low bits
   and can invoke undefined float-to-integer conversion for 0xffffffff. */
int readIndices(const uint8_t* bin, size_t binSize, const json::Json& doc, int accessorIndex,
                std::vector<uint32_t>* out, std::string* error) {
    if (!out || accessorIndex < 0) return AF_ERR_INVALID_ARG;
    const json::Json& accessor = doc["accessors"].atOrNull(static_cast<size_t>(accessorIndex));
    if (accessor.isNull() || accessor["type"].stringOr("") != "SCALAR") {
        if (error) *error = "glTF: index accessor must be an unsigned integer SCALAR";
        return AF_ERR_INVALID_ARG;
    }
    AccessorView view;
    const int rc = resolveAccessor(bin, binSize, doc, accessorIndex, &view, error);
    if (rc != AF_OK) return rc;
    const int valueType = view.componentType;
    const int valueSize = componentSize(valueType);
    if ((valueType != 5121 && valueType != 5123 && valueType != 5125) || view.normalized ||
        view.components != 1 || view.stride != static_cast<size_t>(valueSize)) {
        if (error) *error = "glTF: indices must be tightly packed unsigned scalar values";
        return AF_ERR_INVALID_ARG;
    }
    if (view.count > MAX_ACCESSOR_INDICES) {
        if (error) *error = "glTF: index accessor exceeds the bounded importer limit";
        return AF_ERR_UNSUPPORTED;
    }
    std::vector<uint32_t> decoded(view.count, 0);
    if (view.base) {
        for (size_t i = 0; i < view.count; ++i) {
            size_t offset = 0, elementEnd = 0;
            if (!checkedMul(i, view.stride, &offset) ||
                !checkedAdd(offset, static_cast<size_t>(valueSize), &elementEnd)) {
                if (error) *error = "glTF: index element offset overflows";
                return AF_ERR_INVALID_ARG;
            }
            (void)elementEnd; /* resolveAccessor already proved every element is inside its bufferView. */
            decoded[i] = readUnsignedLE(view.base + offset, valueSize);
        }
    }

    const json::Json& sparse = accessor["sparse"];
    if (!sparse.isNull()) {
        if (!bin) {
            if (error) *error = "glTF: sparse index accessor requires a binary chunk";
            return AF_ERR_INVALID_ARG;
        }
        size_t sparseCount = 0;
        if (!sparse["count"].asIndex(&sparseCount) || sparseCount == 0 || sparseCount > view.count) {
            if (error) *error = "glTF: invalid sparse index accessor count";
            return AF_ERR_INVALID_ARG;
        }
        const int sparseIndexType = sparse["indices"]["componentType"].asInt(0);
        const int sparseIndexSize = componentSize(sparseIndexType);
        if (sparseIndexType != 5121 && sparseIndexType != 5123 && sparseIndexType != 5125) {
            if (error) *error = "glTF: sparse index positions use an unsupported component type";
            return AF_ERR_INVALID_ARG;
        }
        size_t sparseIndexView = 0, sparseValueView = 0;
        if (!sparse["indices"]["bufferView"].asIndex(&sparseIndexView) ||
            !sparse["values"]["bufferView"].asIndex(&sparseValueView)) {
            if (error) *error = "glTF: sparse index accessor has an invalid bufferView reference";
            return AF_ERR_INVALID_ARG;
        }
        const json::Json& idxBv = doc["bufferViews"].atOrNull(sparseIndexView);
        const json::Json& valBv = doc["bufferViews"].atOrNull(sparseValueView);
        if (idxBv.isNull() || valBv.isNull() || idxBv.find("byteStride") || valBv.find("byteStride")) {
            if (error) *error = "glTF: sparse index bufferViews are invalid";
            return AF_ERR_INVALID_ARG;
        }
        size_t idxViewOffset = 0, idxViewLength = 0, valViewOffset = 0, valViewLength = 0;
        size_t idxBuffer = 0, valBuffer = 0;
        if ((!idxBv["byteOffset"].isNull() && !idxBv["byteOffset"].asIndex(&idxViewOffset)) ||
            !idxBv["byteLength"].asIndex(&idxViewLength) ||
            !idxBv["buffer"].asIndex(&idxBuffer) || idxBuffer != 0 ||
            !checkedRange(idxViewOffset, idxViewLength, binSize) ||
            (!valBv["byteOffset"].isNull() && !valBv["byteOffset"].asIndex(&valViewOffset)) ||
            !valBv["byteLength"].asIndex(&valViewLength) ||
            !valBv["buffer"].asIndex(&valBuffer) || valBuffer != 0 ||
            !checkedRange(valViewOffset, valViewLength, binSize)) {
            if (error) *error = "glTF: sparse index bufferView range is invalid";
            return AF_ERR_INVALID_ARG;
        }
        size_t idxOffset = 0, valOffset = 0, idxSpan = 0, valSpan = 0;
        size_t idxStart = 0, valStart = 0;
        if ((!sparse["indices"]["byteOffset"].isNull() && !sparse["indices"]["byteOffset"].asIndex(&idxOffset)) ||
            (!sparse["values"]["byteOffset"].isNull() && !sparse["values"]["byteOffset"].asIndex(&valOffset)) ||
            !checkedMul(sparseCount, static_cast<size_t>(sparseIndexSize), &idxSpan) ||
            !checkedMul(sparseCount, static_cast<size_t>(valueSize), &valSpan) ||
            !checkedRange(idxOffset, idxSpan, idxViewLength) || !checkedRange(valOffset, valSpan, valViewLength) ||
            !checkedAdd(idxViewOffset, idxOffset, &idxStart) || !checkedAdd(valViewOffset, valOffset, &valStart) ||
            !checkedRange(idxStart, idxSpan, binSize) || !checkedRange(valStart, valSpan, binSize)) {
            if (error) *error = "glTF: sparse index data offset/span overflows or exceeds its bufferView";
            return AF_ERR_INVALID_ARG;
        }
        const uint8_t* idxBase = bin + idxStart;
        const uint8_t* valBase = bin + valStart;
        size_t previous = 0;
        for (size_t i = 0; i < sparseCount; ++i) {
            const size_t target = static_cast<size_t>(readUnsignedLE(idxBase + i * static_cast<size_t>(sparseIndexSize), sparseIndexSize));
            if (target >= view.count || (i > 0 && target <= previous)) {
                if (error) *error = "glTF: sparse index positions must be increasing and within the accessor";
                return AF_ERR_INVALID_ARG;
            }
            previous = target;
            decoded[target] = readUnsignedLE(valBase + i * static_cast<size_t>(valueSize), valueSize);
        }
    }
    out->swap(decoded);
    return AF_OK;
}

/* Optional attribute: present -> true and filled; absent -> false and untouched (not an error). */
bool attribute(const json::Json& prim, const char* name, int* outIndex) {
    const json::Json* a = prim["attributes"].find(name);
    if (!a || !a->isNumber()) return false;
    *outIndex = a->asInt(-1);
    return *outIndex >= 0;
}

/* A primitive's attribute identity: the accessor index of every attribute it reads. Two primitives
   with the same key can share one vertex pool, because they read exactly the same vertices. */
struct AttributeKey {
    int values[7] = {-1, -1, -1, -1, -1, -1, -1};
    bool operator==(const AttributeKey& o) const {
        for (int i = 0; i < 7; ++i) {
            if (values[i] != o.values[i]) return false;
        }
        return true;
    }
};

AttributeKey attributeKey(const json::Json& prim) {
    static const char* const NAMES[7] = {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0",
                                         "COLOR_0", "JOINTS_0", "WEIGHTS_0"};
    AttributeKey key;
    for (int i = 0; i < 7; ++i) {
        int idx = -1;
        if (attribute(prim, NAMES[i], &idx)) key.values[i] = idx;
    }
    return key;
}

int buildPrimitive(const uint8_t* bin, size_t binSize, const json::Json& doc, const json::Json& prim,
                   int meshIndex, const std::string& meshName, std::vector<af_vertex>* pool,
                   bool indicesOnly, ImportedPrimitive* out, std::string* error) {
    int posIndex = -1;
    if (!attribute(prim, "POSITION", &posIndex)) {
        /* A primitive without positions has nothing to draw; the spec allows it (a points-cloud
           variant), the renderer does not. Skipping with a warning is handled by the caller. */
        out->meshName = meshName;
        return AF_ERR_UNSUPPORTED;
    }
    std::vector<float> positions;
    size_t vertexCount = 0;
    if (indicesOnly) {
        /* The pool already holds these vertices; only the count is needed, to validate indices. */
        vertexCount = doc["accessors"].atOrNull(static_cast<size_t>(posIndex))["count"].asInt(0) < 0
                          ? 0
                          : static_cast<size_t>(doc["accessors"].atOrNull(static_cast<size_t>(posIndex))["count"].asInt(0));
        out->vertexOffset = 0;
        out->vertexCount = vertexCount;
        out->meshName = meshName;
        out->mesh = meshIndex;
        out->material = prim.find("material") && prim["material"].isNumber() ? prim["material"].asInt(-1) : -1;
        if (prim.find("indices") && prim["indices"].isNumber()) {
            std::vector<uint32_t> idx;
            const int irc2 = readIndices(bin, binSize, doc, prim["indices"].asInt(-1), &out->indices, error);
            if (irc2 != AF_OK) return irc2;
            for (uint32_t v : out->indices) {
                if (v >= vertexCount) {
                    if (error) *error = "glTF: index beyond the primitive's vertex count";
                    return AF_ERR_INVALID_ARG;
                }
            }
        } else {
            out->indices.resize(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) out->indices[i] = static_cast<uint32_t>(i);
        }
        return AF_OK;
    }
    int rc = readAccessorFloats(bin, binSize, doc, posIndex, 3, &positions, &vertexCount, error);
    if (rc != AF_OK) return rc;
    if (vertexCount > (1u << 24)) {
        if (error) *error = "glTF: primitive with more than 16 M vertices";
        return AF_ERR_UNSUPPORTED;
    }

    pool->assign(vertexCount, af_vertex{});
    out->vertexOffset = 0;
    out->vertexCount = vertexCount;
    out->meshName = meshName;
    out->mesh = meshIndex;
    for (size_t i = 0; i < vertexCount; ++i) {
        af_vertex& v = (*pool)[i];
        v.pos[0] = positions[i * 3 + 0];
        v.pos[1] = positions[i * 3 + 1];
        v.pos[2] = positions[i * 3 + 2];
        v.normal[1] = 1.0f;   /* default until NORMAL is read */
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
        v.tangent[3] = 1.0f;
    }

    auto readVec = [&](const char* name, int components, std::vector<float>* dst) -> int {
        int idx = -1;
        if (!attribute(prim, name, &idx)) return 1; /* absent */
        size_t n = 0;
        const int r = readAccessorFloats(bin, binSize, doc, idx, components, dst, &n, error);
        if (r != AF_OK) return r;
        if (n != vertexCount) {
            if (error) *error = std::string("glTF: ") + name + " count differs from POSITION count";
            return AF_ERR_INVALID_ARG;
        }
        return 0;
    };

    std::vector<float> scratch;
    int present = readVec("NORMAL", 3, &scratch);
    if (present < 0) return present;
    if (present == 0) {
        for (size_t i = 0; i < vertexCount; ++i) {
            (*pool)[i].normal[0] = scratch[i * 3 + 0];
            (*pool)[i].normal[1] = scratch[i * 3 + 1];
            (*pool)[i].normal[2] = scratch[i * 3 + 2];
        }
    }
    present = readVec("TANGENT", 4, &scratch);
    if (present < 0) return present;
    if (present == 0) {
        for (size_t i = 0; i < vertexCount; ++i) {
            (*pool)[i].tangent[0] = scratch[i * 4 + 0];
            (*pool)[i].tangent[1] = scratch[i * 4 + 1];
            (*pool)[i].tangent[2] = scratch[i * 4 + 2];
            (*pool)[i].tangent[3] = scratch[i * 4 + 3];
        }
    }
    present = readVec("TEXCOORD_0", 2, &scratch);
    if (present < 0) return present;
    if (present == 0) {
        for (size_t i = 0; i < vertexCount; ++i) {
            (*pool)[i].uv[0] = scratch[i * 2 + 0];
            (*pool)[i].uv[1] = scratch[i * 2 + 1];
        }
    }
    {
        int colorIndex = -1;
        if (attribute(prim, "COLOR_0", &colorIndex)) {
            const json::Json& colorAccessor = doc["accessors"].atOrNull(static_cast<size_t>(colorIndex));
            const std::string colorType = colorAccessor["type"].stringOr("");
            const int colorComponents = colorType == "VEC3" ? 3 : colorType == "VEC4" ? 4 : 0;
            if (colorComponents == 0) {
                if (error) *error = "glTF: COLOR_0 accessor must be VEC3 or VEC4";
                return AF_ERR_INVALID_ARG;
            }
            size_t n = 0;
            const int colorRc = readAccessorFloats(bin, binSize, doc, colorIndex, colorComponents, &scratch, &n, error);
            if (colorRc != AF_OK) return colorRc;
            if (n != vertexCount) {
                if (error) *error = "glTF: COLOR_0 count differs from POSITION count";
                return AF_ERR_INVALID_ARG;
            }
            for (size_t i = 0; i < vertexCount; ++i) {
                for (int c = 0; c < colorComponents; ++c)
                    (*pool)[i].color[c] = scratch[i * static_cast<size_t>(colorComponents) + static_cast<size_t>(c)];
                if (colorComponents == 3) (*pool)[i].color[3] = 1.0f;
            }
        }
    }
    present = readVec("JOINTS_0", 4, &scratch);
    if (present < 0) return present;
    if (present == 0) {
        for (size_t i = 0; i < vertexCount; ++i) {
            for (int c = 0; c < 4; ++c) {
                const float j = scratch[i * 4 + static_cast<size_t>(c)];
                (*pool)[i].joints[c] = static_cast<uint16_t>(j < 0.0f ? 0.0f : (j > 65535.0f ? 65535.0f : j));
            }
        }
    }
    present = readVec("WEIGHTS_0", 4, &scratch);
    if (present < 0) return present;
    if (present == 0) {
        for (size_t i = 0; i < vertexCount; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const float w = scratch[i * 4 + static_cast<size_t>(c)];
                (*pool)[i].weights[c] = w;
                sum += w;
            }
            /* Renormalise: a writer may have quantised the weights, and the shader relies on a
               partition of unity. All-zero weights stay all-zero (rigid, unskinned). */
            if (sum > 1e-6f) {
                const float inv = 1.0f / sum;
                for (int c = 0; c < 4; ++c) (*pool)[i].weights[c] *= inv;
            }
        }
    }

    if (prim.find("indices") && prim["indices"].isNumber()) {
        rc = readIndices(bin, binSize, doc, prim["indices"].asInt(-1), &out->indices, error);
        if (rc != AF_OK) return rc;
        for (uint32_t idx : out->indices) {
            if (idx >= vertexCount) {
                if (error) *error = "glTF: index beyond the primitive's vertex count";
                return AF_ERR_INVALID_ARG;
            }
        }
    } else {
        /* Non-indexed primitive: synthesise the trivial index buffer, since the renderer draws
           indexed everything. */
        out->indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) out->indices[i] = static_cast<uint32_t>(i);
    }
    out->material = prim.find("material") && prim["material"].isNumber() ? prim["material"].asInt(-1) : -1;
    return AF_OK;
}

} /* namespace */

void GlbAsset::bounds(Vec3* outMin, Vec3* outMax) const {
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    bool any = false;
    for (const auto& pool : pools) {
        for (const auto& v : pool) {
            any = true;
            lo.x = v.pos[0] < lo.x ? v.pos[0] : lo.x;
            lo.y = v.pos[1] < lo.y ? v.pos[1] : lo.y;
            lo.z = v.pos[2] < lo.z ? v.pos[2] : lo.z;
            hi.x = v.pos[0] > hi.x ? v.pos[0] : hi.x;
            hi.y = v.pos[1] > hi.y ? v.pos[1] : hi.y;
            hi.z = v.pos[2] > hi.z ? v.pos[2] : hi.z;
        }
    }
    if (!any) {
        lo = hi = Vec3(0.0f, 0.0f, 0.0f);
    }
    if (outMin) *outMin = lo;
    if (outMax) *outMax = hi;
}

int importGlb(const uint8_t* data, size_t size, GlbAsset* out, std::string* error) {
    if (error) error->clear();
    if (!out) {
        if (error) *error = "importGlb: null output";
        return AF_ERR_INVALID_ARG;
    }
    *out = GlbAsset();
    if (!data || size < 20) {
        if (error) *error = "GLB: file shorter than a header";
        return AF_ERR_INVALID_ARG;
    }
    if (rd32(data) != GLB_MAGIC) {
        if (error) *error = "GLB: bad magic (not a binary glTF)";
        return AF_ERR_INVALID_ARG;
    }
    const uint32_t version = rd32(data + 4);
    if (version != 2) {
        if (error) *error = "GLB: only version 2 is supported";
        return AF_ERR_UNSUPPORTED;
    }
    const uint32_t total = rd32(data + 8);
    if (total > size) {
        if (error) *error = "GLB: declared length exceeds the file";
        return AF_ERR_INVALID_ARG;
    }
    const size_t fileSize = total;   /* trust the header only within the actual buffer */

    const uint8_t* jsonChunk = nullptr;
    size_t jsonSize = 0;
    const uint8_t* binChunk = nullptr;
    size_t binSize = 0;
    size_t pos = 12;
    while (pos <= fileSize && fileSize - pos >= 8) {
        const uint32_t chunkLen = rd32(data + pos);
        const uint32_t chunkType = rd32(data + pos + 4);
        size_t payloadStart = 0, nextPos = 0;
        if (!checkedAdd(pos, 8, &payloadStart) ||
            !checkedRange(payloadStart, static_cast<size_t>(chunkLen), fileSize) ||
            !checkedAdd(payloadStart, static_cast<size_t>(chunkLen), &nextPos)) {
            if (error) *error = "GLB: chunk length overflows or exceeds the file";
            return AF_ERR_INVALID_ARG;
        }
        const uint8_t* payload = data + payloadStart;
        if (chunkType == CHUNK_JSON && !jsonChunk) {
            jsonChunk = payload;
            jsonSize = chunkLen;
        } else if (chunkType == CHUNK_BIN && !binChunk) {
            binChunk = payload;
            binSize = chunkLen;
        }
        /* Unknown chunk types must be skipped, per spec. */
        pos = nextPos;
    }
    if (pos != fileSize) {
        if (error) *error = "GLB: trailing bytes do not form a complete chunk header";
        return AF_ERR_INVALID_ARG;
    }
    if (!jsonChunk) {
        if (error) *error = "GLB: no JSON chunk";
        return AF_ERR_INVALID_ARG;
    }

    json::Json doc;
    if (!json::Json::parse(std::string(reinterpret_cast<const char*>(jsonChunk), jsonSize), &doc, error)) return AF_ERR_INVALID_ARG;
    if (!doc.isObject()) {
        if (error) *error = "GLB: JSON chunk is not an object";
        return AF_ERR_INVALID_ARG;
    }
    const std::string assetVersion = doc["asset"]["version"].stringOr("");
    if (assetVersion.rfind("2.", 0) != 0) {
        if (error) *error = "GLB: asset.version is not glTF 2.x";
        return AF_ERR_UNSUPPORTED;
    }
    out->version = assetVersion;
    out->generator = doc["asset"]["generator"].stringOr("");

    /* This importer currently supports only the single embedded GLB BIN buffer. Validate every
       bufferView up front, including views used only by images or sparse data. */
    const json::Json& buffersJson = doc["buffers"];
    const json::Json& viewsJson = doc["bufferViews"];
    size_t declaredBufferLength = 0;
    if (viewsJson.size() > 0) {
        if (buffersJson.size() != 1 || !binChunk) {
            if (error) *error = "GLB: bufferViews require one embedded BIN buffer";
            return AF_ERR_UNSUPPORTED;
        }
        const json::Json& bufferJson = buffersJson.at(0);
        if (bufferJson.find("uri") || !bufferJson["byteLength"].asIndex(&declaredBufferLength) ||
            declaredBufferLength > binSize || binSize - declaredBufferLength > 3) {
            if (error) *error = "GLB: embedded buffer declaration is invalid or exceeds the BIN chunk";
            return AF_ERR_INVALID_ARG;
        }
        for (size_t i = 0; i < viewsJson.size(); ++i) {
            const json::Json& bv = viewsJson.at(i);
            size_t bufferIndex = 0, offset = 0, length = 0;
            if (!bv["buffer"].asIndex(&bufferIndex) || bufferIndex != 0 ||
                (!bv["byteOffset"].isNull() && !bv["byteOffset"].asIndex(&offset)) ||
                !bv["byteLength"].asIndex(&length) ||
                !checkedRange(offset, length, declaredBufferLength)) {
                if (error) *error = "GLB: bufferView has an invalid buffer index or out-of-range byte span";
                return AF_ERR_INVALID_ARG;
            }
            if (bv.find("byteStride")) {
                size_t stride = 0;
                if (!bv["byteStride"].asIndex(&stride) || stride < 4 || stride > 252 || (stride % 4) != 0) {
                    if (error) *error = "GLB: byteStride must be a multiple of 4 in the range 4..252";
                    return AF_ERR_INVALID_ARG;
                }
            }
        }
    }

    /* Extensions: record them all. Any *required* extension is fatal, because the document's
       meaning may depend on it and this reader implements none of them. */
    for (const auto& e : doc["extensionsUsed"].items()) {
        if (e.isString()) out->extensionsUsed.push_back(e.asString());
    }
    for (const auto& e : doc["extensionsRequired"].items()) {
        if (e.isString()) {
            if (error) *error = "GLB: requires unsupported extension '" + e.asString() + "'";
            return AF_ERR_UNSUPPORTED;
        }
    }

    /* ---- nodes and the scene ---- */
    const json::Json& nodesJson = doc["nodes"];
    out->nodes.resize(nodesJson.size());
    for (size_t i = 0; i < nodesJson.size(); ++i) {
        const json::Json& n = nodesJson.at(i);
        ImportedNode& node = out->nodes[i];
        node.name = n["name"].stringOr("");
        node.mesh = n.find("mesh") && n["mesh"].isNumber() ? n["mesh"].asInt(-1) : -1;
        node.skin = n.find("skin") && n["skin"].isNumber() ? n["skin"].asInt(-1) : -1;
        for (const auto& c : n["children"].items()) {
            size_t child = 0;
            if (!c.asIndex(&child) || child >= out->nodes.size()) {
                if (error) *error = "GLB: node child index out of range";
                return AF_ERR_INVALID_ARG;
            }
            node.children.push_back(static_cast<int>(child));
            /* Record the parent here rather than trusting the file to be consistent. If a node is
               claimed by two parents, the first wins and the second link is dropped - a node with
               two parents has no defined world transform. */
            if (out->nodes[child].parent < 0) {
                out->nodes[child].parent = static_cast<int>(i);
            } else {
                out->warnings.push_back({"node has multiple parents", "link dropped"});
                node.children.pop_back();
            }
        }
        if (n.find("matrix")) {
            const json::Json& m = n["matrix"];
            if (m.size() == 16) {
                for (size_t k = 0; k < 16; ++k) node.local.m[k] = static_cast<float>(m.at(k).asNumber(k == 0 || k == 5 || k == 10 || k == 15 ? 1.0 : 0.0));
            } else {
                out->warnings.push_back({"node matrix has the wrong length", "identity used"});
            }
        } else {
            const json::Json& t = n["translation"];
            const json::Json& r = n["rotation"];
            const json::Json& s = n["scale"];
            const Vec3 translation(t.size() == 3 ? static_cast<float>(t.at(0).asNumber()) : 0.0f,
                                   t.size() == 3 ? static_cast<float>(t.at(1).asNumber()) : 0.0f,
                                   t.size() == 3 ? static_cast<float>(t.at(2).asNumber()) : 0.0f);
            Quat rotation(0, 0, 0, 1);
            if (r.size() == 4) {
                rotation = Quat(static_cast<float>(r.at(0).asNumber()), static_cast<float>(r.at(1).asNumber()),
                                static_cast<float>(r.at(2).asNumber()), static_cast<float>(r.at(3).asNumber()));
            }
            const Vec3 scale(s.size() == 3 ? static_cast<float>(s.at(0).asNumber()) : 1.0f,
                             s.size() == 3 ? static_cast<float>(s.at(1).asNumber()) : 1.0f,
                             s.size() == 3 ? static_cast<float>(s.at(2).asNumber()) : 1.0f);
            node.local = Mat4::trs(translation, rotation, scale);
        }
    }

    /* Root set: the default scene's nodes, or every parentless node when no scene is declared. */
    const json::Json& scenes = doc["scenes"];
    size_t sceneIndex = 0;
    if (doc.find("scene") && !doc["scene"].asIndex(&sceneIndex)) sceneIndex = 0;
    const json::Json& scene = scenes.atOrNull(sceneIndex);
    for (const auto& r : scene["nodes"].items()) {
        size_t idx = 0;
        if (r.asIndex(&idx) && idx < out->nodes.size()) out->sceneRoots.push_back(static_cast<int>(idx));
    }
    if (out->sceneRoots.empty()) {
        for (size_t i = 0; i < out->nodes.size(); ++i) {
            if (out->nodes[i].parent < 0) out->sceneRoots.push_back(static_cast<int>(i));
        }
    }

    /* ---- materials ---- */
    for (const auto& m : doc["materials"].items()) {
        ImportedMaterial mat;
        mat.name = m["name"].stringOr("");
        const json::Json& pbr = m["pbrMetallicRoughness"];
        const json::Json& bc = pbr["baseColorFactor"];
        if (bc.size() == 4) {
            for (int c = 0; c < 4; ++c) mat.baseColor[c] = static_cast<float>(bc.at(static_cast<size_t>(c)).asNumber(c == 3 ? 1.0 : 1.0));
        }
        mat.metallic = static_cast<float>(pbr["metallicFactor"].asNumber(1.0));
        mat.roughness = static_cast<float>(pbr["roughnessFactor"].asNumber(1.0));
        const json::Json& em = m["emissiveFactor"];
        if (em.size() == 3) {
            for (int c = 0; c < 3; ++c) mat.emissive[c] = static_cast<float>(em.at(static_cast<size_t>(c)).asNumber(0.0));
        }
        mat.doubleSided = m["doubleSided"].asBool(false);
        auto texIndex = [&](const json::Json& texRef) -> int {
            const json::Json& tex = doc["textures"].atOrNull(static_cast<size_t>(texRef["index"].asInt(-1)));
            if (tex.isNull()) return -1;
            const json::Json& src = doc["images"].atOrNull(static_cast<size_t>(tex["source"].asInt(-1)));
            if (src.isNull()) return -1;
            return tex["source"].asInt(-1);
        };
        mat.baseColorTexture = texIndex(pbr["baseColorTexture"]);
        mat.metallicRoughnessTexture = texIndex(pbr["metallicRoughnessTexture"]);
        mat.normalTexture = texIndex(m["normalTexture"]);
        mat.emissiveTexture = texIndex(m["emissiveTexture"]);
        out->materials.push_back(std::move(mat));
    }

    /* ---- textures (images decoded to RGBA8; the texture list is parallel to images) ---- */
    const json::Json& images = doc["images"];
    for (size_t i = 0; i < images.size(); ++i) {
        const json::Json& img = images.at(i);
        ImportedTexture tex;
        tex.name = img["name"].stringOr("");
        if (img.find("uri")) {
            /* External or data URI: the app is offline and its exports are single-file. */
            out->warnings.push_back({"image uses a URI", "not loaded (offline, single-file assets only)"});
            out->textures.push_back(std::move(tex));
            continue;
        }
        const int bvIndex = img["bufferView"].asInt(-1);
        const json::Json& bv = doc["bufferViews"].atOrNull(static_cast<size_t>(bvIndex < 0 ? 0 : bvIndex));
        if (bvIndex < 0 || bv.isNull()) {
            out->warnings.push_back({"image without a bufferView", "skipped"});
            out->textures.push_back(std::move(tex));
            continue;
        }
        size_t bvOffset = 0, bvLength = 0;
        bv["byteOffset"].asIndex(&bvOffset);
        bv["byteLength"].asIndex(&bvLength);
        if (!checkedRange(bvOffset, bvLength, binSize)) {
            out->warnings.push_back({"image bufferView exceeds the binary chunk", "skipped"});
            out->textures.push_back(std::move(tex));
            continue;
        }
        const uint8_t* bytes = binChunk ? binChunk + bvOffset : nullptr;
        std::string reason;
        DecodedImage decoded;
        if (!bytes) {
            reason = "no binary chunk";
        } else if (looksLikePng(bytes, bvLength)) {
            if (!decodePng(bytes, bvLength, &decoded, &reason)) {
                out->warnings.push_back({"PNG decode failed", reason});
            }
        } else if (bvLength >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
            reason = "JPEG is not supported by this decoder";
            out->warnings.push_back({"image is JPEG", reason});
        } else {
            reason = "unrecognised image bytes";
            out->warnings.push_back({"image format", reason});
        }
        if (!decoded.rgba.empty()) {
            tex.width = decoded.width;
            tex.height = decoded.height;
            tex.rgba = std::move(decoded.rgba);
        }
        out->textures.push_back(std::move(tex));
    }

    /* ---- skins ---- */
    for (const auto& s : doc["skins"].items()) {
        ImportedSkin skin;
        skin.name = s["name"].stringOr("");
        for (const auto& j : s["joints"].items()) {
            size_t idx = 0;
            if (!j.asIndex(&idx) || idx >= out->nodes.size()) {
                if (error) *error = "GLB: skin joint index out of range";
                return AF_ERR_INVALID_ARG;
            }
            skin.joints.push_back(static_cast<int>(idx));
        }
        skin.skeletonRoot = s.find("skeleton") ? s["skeleton"].asInt(-1) : -1;
        skin.inverseBindMatrices.resize(skin.joints.size(), Mat4::identity());
        if (s.find("inverseBindMatrices")) {
            std::vector<float> mats;
            size_t count = 0;
            if (readAccessorFloats(binChunk, binSize, doc, s["inverseBindMatrices"].asInt(-1), 16, &mats, &count, error) == AF_OK) {
                if (count != skin.joints.size()) {
                    out->warnings.push_back({"inverse bind matrix count differs from joint count", "extra entries ignored"});
                }
                const size_t n = count < skin.joints.size() ? count : skin.joints.size();
                for (size_t k = 0; k < n; ++k) {
                    for (size_t e = 0; e < 16; ++e) skin.inverseBindMatrices[k].m[e] = mats[k * 16 + e];
                }
            }
        }
        out->skins.push_back(std::move(skin));
    }

    /* ---- meshes -> primitives, sharing vertex pools where the attributes are identical ---- */
    const json::Json& meshes = doc["meshes"];
    std::vector<AttributeKey> poolKeys;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const json::Json& mesh = meshes.at(mi);
        const std::string meshName = mesh["name"].stringOr("");
        for (const auto& prim : mesh["primitives"].items()) {
            const AttributeKey key = attributeKey(prim);
            int poolIndex = -1;
            for (size_t k = 0; k < poolKeys.size(); ++k) {
                if (poolKeys[k] == key) {
                    poolIndex = static_cast<int>(k);
                    break;
                }
            }
            ImportedPrimitive ip;
            if (poolIndex >= 0) {
                /* Reuse: this primitive reads the same vertices as an earlier one, so it only
                   contributes its index range. Its attribute reads are skipped entirely, which is
                   also why a shared pool costs one decode instead of ten. */
                std::vector<af_vertex> scratch;
                const int rc = buildPrimitive(binChunk, binSize, doc, prim, static_cast<int>(mi), meshName,
                                              &scratch, true, &ip, error);
                if (rc == AF_ERR_UNSUPPORTED) {
                    out->warnings.push_back({"primitive skipped", ip.meshName.empty() ? "no POSITION" : ip.meshName});
                    continue;
                }
                if (rc != AF_OK) return rc;
                ip.pool = poolIndex;
                out->primitives.push_back(std::move(ip));
                continue;
            }
            out->pools.emplace_back();
            const int rc = buildPrimitive(binChunk, binSize, doc, prim, static_cast<int>(mi), meshName,
                                          &out->pools.back(), false, &ip, error);
            if (rc == AF_ERR_UNSUPPORTED) {
                out->pools.pop_back();
                out->warnings.push_back({"primitive skipped", ip.meshName.empty() ? "no POSITION" : ip.meshName});
                continue;
            }
            if (rc != AF_OK) return rc;
            ip.pool = static_cast<int>(out->pools.size()) - 1;
            poolKeys.push_back(key);
            out->primitives.push_back(std::move(ip));
        }
    }
    /* Attach each primitive to the first node that references its mesh, so world transforms can
       be applied to it. A mesh instanced by two nodes is recorded once: the renderer draws the
       geometry from the node it knows about, and instancing is a Phase 2 concern. */
    for (auto& p : out->primitives) {
        for (size_t ni = 0; ni < out->nodes.size(); ++ni) {
            if (out->nodes[ni].mesh == p.mesh) {
                p.node = static_cast<int>(ni);
                break;
            }
        }
    }

    /* ---- animations ---- */
    for (const auto& a : doc["animations"].items()) {
        ImportedAnimation anim;
        anim.name = a["name"].stringOr("");
        for (const auto& ch : a["channels"].items()) {
            ImportedAnimationChannel chan;
            size_t nodeIndex = 0;
            if (!ch["target"]["node"].asIndex(&nodeIndex) || nodeIndex >= out->nodes.size()) {
                if (error) *error = "glTF: animation channel target node is missing or out of range";
                return AF_ERR_INVALID_ARG;
            }
            chan.node = static_cast<int>(nodeIndex);
            const std::string path = ch["target"]["path"].stringOr("");
            if (path == "translation") chan.path = 0;
            else if (path == "rotation") chan.path = 1;
            else if (path == "scale") chan.path = 2;
            else {
                if (path == "weights") continue; /* morph weights are outside this channel format */
                if (error) *error = "glTF: unsupported animation target path";
                return AF_ERR_UNSUPPORTED;
            }
            size_t samplerIndex = 0;
            if (!ch["sampler"].asIndex(&samplerIndex)) {
                if (error) *error = "glTF: animation channel has an invalid sampler index";
                return AF_ERR_INVALID_ARG;
            }
            const json::Json& sampler = a["samplers"].atOrNull(samplerIndex);
            if (sampler.isNull()) {
                if (error) *error = "glTF: animation channel sampler is out of range";
                return AF_ERR_INVALID_ARG;
            }
            const std::string interp = sampler["interpolation"].stringOr("LINEAR");
            if (interp != "LINEAR" && interp != "STEP" && interp != "CUBICSPLINE") {
                if (error) *error = "glTF: unsupported animation interpolation";
                return AF_ERR_UNSUPPORTED;
            }
            chan.interpolation = (interp == "STEP") ? 1 : (interp == "CUBICSPLINE") ? 2 : 0;

            std::vector<float> times;
            size_t timeCount = 0;
            const int inputRc = readAccessorFloats(binChunk, binSize, doc, sampler["input"].asInt(-1), 1, &times, &timeCount, error);
            if (inputRc != AF_OK) return inputRc;
            std::vector<float> values;
            size_t valueCount = 0;
            const int comps = chan.path == 1 ? 4 : 3; /* rotation is VEC4; translation/scale are VEC3 */
            const int outputRc = readAccessorFloats(binChunk, binSize, doc, sampler["output"].asInt(-1), comps, &values, &valueCount, error);
            if (outputRc != AF_OK) return outputRc;
            if (timeCount == 0 || valueCount != timeCount *
                    static_cast<size_t>(chan.interpolation == 2 ? 3 : 1)) {
                if (error) *error = "glTF: animation sampler input/output key counts do not match target path";
                return AF_ERR_INVALID_ARG;
            }
            for (size_t i = 0; i < times.size(); ++i) {
                if (!std::isfinite(times[i]) || times[i] < 0.0f || (i > 0 && times[i] <= times[i - 1])) {
                    if (error) *error = "glTF: animation input times must be finite, non-negative and strictly increasing";
                    return AF_ERR_INVALID_ARG;
                }
            }

            chan.times.assign(times.begin(), times.end());
            if (chan.interpolation == 2) {
                /* CUBICSPLINE stores in-tangent, value, out-tangent for each key. */
                const size_t keys = timeCount;
                chan.values.resize(keys);
                chan.inTangents.resize(keys);
                chan.outTangents.resize(keys);
                for (size_t k = 0; k < keys; ++k) {
                    auto readValue = [&](size_t index) {
                        const size_t base = index * static_cast<size_t>(comps);
                        return Vec4(values[base], values[base + 1], values[base + 2], comps == 4 ? values[base + 3] : 0.0f);
                    };
                    chan.inTangents[k] = readValue(k * 3 + 0);
                    chan.values[k] = readValue(k * 3 + 1);
                    chan.outTangents[k] = readValue(k * 3 + 2);
                }
            } else {
                chan.values.resize(timeCount);
                for (size_t k = 0; k < timeCount; ++k) {
                    const size_t base = k * static_cast<size_t>(comps);
                    chan.values[k] = Vec4(values[base], values[base + 1], values[base + 2],
                                           comps == 4 ? values[base + 3] : 0.0f);
                }
            }
            if (!chan.times.empty()) anim.duration = chan.times.back() > anim.duration ? chan.times.back() : anim.duration;
            anim.channels.push_back(std::move(chan));
        }
        out->animations.push_back(std::move(anim));
    }

    /* ---- world transforms, with a cycle check ---- */
    if (!resolveWorldTransforms(out, error)) return AF_ERR_INVALID_ARG;

    /* This engine currently flattens imported geometry into vertex pools and does not apply
       per-node matrices at draw time. Refuse transformed mesh nodes rather than silently rendering
       geometry in the wrong place. Identity transforms remain supported. */
    for (const auto& node : out->nodes) {
        if (node.mesh < 0) continue;
        const Mat4& world = node.world;
        for (size_t i = 0; i < 16; ++i) {
            const float expected = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;
            if (!std::isfinite(world.m[i]) || std::fabs(world.m[i] - expected) > 1e-5f) {
                if (error) *error = "glTF: transformed mesh nodes are unsupported by the flattened engine importer";
                return AF_ERR_UNSUPPORTED;
            }
        }
    }
    return AF_OK;
}

bool resolveWorldTransforms(GlbAsset* asset, std::string* error) {
    const size_t n = asset->nodes.size();
    /* Iterative, parent-first placement with a hop counter. A cycle shows up as a node whose
       parent chain never reaches a root; the counter catches it in O(n) worst case per node and
       the function then fails rather than looping. */
    std::vector<int> state(n, 0);   /* 0 unvisited, 1 in progress, 2 placed */
    for (size_t start = 0; start < n; ++start) {
        if (state[start] == 2) continue;
        std::vector<int> chain;
        int cur = static_cast<int>(start);
        while (cur >= 0 && state[static_cast<size_t>(cur)] == 0) {
            state[static_cast<size_t>(cur)] = 1;
            chain.push_back(cur);
            cur = asset->nodes[static_cast<size_t>(cur)].parent;
            if (chain.size() > n + 1) break;
        }
        if (cur >= 0 && state[static_cast<size_t>(cur)] == 1) {
            if (error) *error = "GLB: node hierarchy contains a cycle";
            return false;
        }
        /* Place the chain root-first. */
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            ImportedNode& node = asset->nodes[static_cast<size_t>(*it)];
            if (node.parent >= 0) {
                node.world = asset->nodes[static_cast<size_t>(node.parent)].world * node.local;
            } else {
                node.world = node.local;
            }
            state[static_cast<size_t>(*it)] = 2;
        }
    }
    return true;
}

int importGlbFile(const char* path, GlbAsset* out, std::string* error) {
    if (!path) {
        if (error) *error = "importGlbFile: null path";
        return AF_ERR_INVALID_ARG;
    }
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        if (error) *error = std::string("cannot open ") + path;
        return AF_ERR_IO;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        if (error) *error = "empty file";
        return AF_ERR_IO;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) {
        if (error) *error = "short read";
        return AF_ERR_IO;
    }
    return importGlb(bytes.data(), bytes.size(), out, error);
}

} /* namespace af */
