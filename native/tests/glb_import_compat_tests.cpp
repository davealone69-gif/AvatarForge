#include "af_glb_read.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void appendU32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value >> 16));
    out->push_back(static_cast<uint8_t>(value >> 24));
}

void appendF32(std::vector<uint8_t>* out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(out, bits);
}

std::vector<uint8_t> makeGlb(const char* colorType, int colorComponents) {
    std::vector<uint8_t> bin;
    for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        appendF32(&bin, value);
    }
    for (int vertex = 0; vertex < 3; ++vertex) {
        const float rgb[3][3] = {{0.25f, 0.5f, 0.75f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
        for (int component = 0; component < 3; ++component) {
            appendF32(&bin, rgb[vertex][component]);
        }
        if (colorComponents == 4) appendF32(&bin, vertex == 0 ? 0.4f : 1.0f);
    }

    const size_t positionBytes = 9 * sizeof(float);
    const std::string json = std::string("{\"asset\":{\"version\":\"2.0\"},") +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
        std::to_string(positionBytes) + "},{\"buffer\":0,\"byteOffset\":" +
        std::to_string(positionBytes) + ",\"byteLength\":" +
        std::to_string(bin.size() - positionBytes) + "}]," +
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"" + colorType + "\"}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1}}]}]," +
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

    std::vector<uint8_t> paddedJson(json.begin(), json.end());
    while (paddedJson.size() % 4 != 0) paddedJson.push_back(' ');

    std::vector<uint8_t> glb;
    const uint32_t totalLength = static_cast<uint32_t>(12 + 8 + paddedJson.size() + 8 + bin.size());
    appendU32(&glb, 0x46546C67u);
    appendU32(&glb, 2);
    appendU32(&glb, totalLength);
    appendU32(&glb, static_cast<uint32_t>(paddedJson.size()));
    appendU32(&glb, 0x4E4F534Au);
    glb.insert(glb.end(), paddedJson.begin(), paddedJson.end());
    appendU32(&glb, static_cast<uint32_t>(bin.size()));
    appendU32(&glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void checkColorAccessor(const char* type, int components, float expectedAlpha) {
    const std::vector<uint8_t> bytes = makeGlb(type, components);
    af::GlbAsset asset;
    std::string error;
    const int result = af::importGlb(bytes.data(), bytes.size(), &asset, &error);
    const std::string label = std::string("COLOR_0 ") + type;
    check(result == AF_OK, (label + " imports: " + error).c_str());
    if (result != AF_OK || asset.pools.empty() || asset.pools[0].size() != 3) return;

    const af_vertex& first = asset.pools[0][0];
    check(std::fabs(first.color[0] - 0.25f) < 1e-6f, (label + " preserves red").c_str());
    check(std::fabs(first.color[1] - 0.5f) < 1e-6f, (label + " preserves green").c_str());
    check(std::fabs(first.color[2] - 0.75f) < 1e-6f, (label + " preserves blue").c_str());
    check(std::fabs(first.color[3] - expectedAlpha) < 1e-6f, (label + " uses expected alpha").c_str());
}

} // namespace

int main() {
    checkColorAccessor("VEC3", 3, 1.0f);
    checkColorAccessor("VEC4", 4, 0.4f);
    if (failures != 0) {
        std::fprintf(stderr, "%d GLB importer compatibility check(s) failed\n", failures);
        return 1;
    }
    std::puts("All GLB importer compatibility checks passed");
    return 0;
}
