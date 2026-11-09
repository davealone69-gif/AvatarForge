/*
 * glb_extension_test.cpp -- host regression test for GLB extension handling.
 *
 * Guards the two rules that were lost once when the reader was re-vendored:
 *   1. Required glTF extensions other than EXT_texture_webp are refused.
 *   2. EXT_texture_webp is accepted only with a valid image/webp source, and a required WebP that
 *      cannot be decoded fails the import instead of silently dropping the texture.
 *
 * Runs on the host (no Android SDK needed). Build and run from the repository root:
 *   g++ -std=c++17 -Iavatarforge-engine/src/main/cpp/src -Iavatarforge-engine/src/main/cpp/include \
 *       avatarforge-engine/src/test/cpp/glb_extension_test.cpp \
 *       avatarforge-engine/src/main/cpp/src/af_glb_read.cpp \
 *       avatarforge-engine/src/main/cpp/src/af_json.cpp \
 *       avatarforge-engine/src/main/cpp/src/af_png_decode.cpp -o glb_extension_test && ./glb_extension_test
 */
#include "af_glb_read.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

std::vector<uint8_t> makeGlb(std::string json, const std::vector<uint8_t>& bin) {
    while (json.size() % 4 != 0) json.push_back(' ');
    std::vector<uint8_t> binPadded = bin;
    while (binPadded.size() % 4 != 0) binPadded.push_back(0);
    const size_t total = 12 + 8 + json.size() + (binPadded.empty() ? 0 : 8 + binPadded.size());
    std::vector<uint8_t> out;
    put32(out, 0x46546C67u);  /* 'glTF' */
    put32(out, 2u);
    put32(out, static_cast<uint32_t>(total));
    put32(out, static_cast<uint32_t>(json.size()));
    put32(out, 0x4E4F534Au);  /* JSON */
    out.insert(out.end(), json.begin(), json.end());
    if (!binPadded.empty()) {
        put32(out, static_cast<uint32_t>(binPadded.size()));
        put32(out, 0x004E4942u);  /* BIN */
        out.insert(out.end(), binPadded.begin(), binPadded.end());
    }
    return out;
}

int failures = 0;

void expect(const char* name, const std::vector<uint8_t>& glb, int expectedRc,
            const char* expectedFragment) {
    af::GlbAsset asset;
    std::string error;
    const int rc = af::importGlb(glb.data(), glb.size(), &asset, &error);
    const bool rcOk = rc == expectedRc;
    const bool textOk = expectedFragment == nullptr || error.find(expectedFragment) != std::string::npos;
    if (rcOk && textOk) {
        std::printf("PASS  %s\n", name);
    } else {
        ++failures;
        std::printf("FAIL  %s: rc=%d (want %d) error=\"%s\"\n", name, rc, expectedRc, error.c_str());
    }
}

}  // namespace

int main() {
    /* 1. A required extension we do not implement must be refused. */
    expect("refuses required KHR_draco_mesh_compression",
           makeGlb(R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_draco_mesh_compression"],)"
                   R"("extensionsRequired":["KHR_draco_mesh_compression"]})", {}),
           AF_ERR_UNSUPPORTED, "KHR_draco_mesh_compression");

    /* 2. EXT_texture_webp declared as required but no texture uses it: refused as inconsistent. */
    expect("refuses required EXT_texture_webp with no texture using it",
           makeGlb(R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)"
                   R"("extensionsRequired":["EXT_texture_webp"]})", {}),
           AF_ERR_INVALID_ARG, "no texture uses the extension");

    /* 3. A WebP texture whose source is not declared image/webp is refused. */
    const std::vector<uint8_t> fakeWebp = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'E', 'B', 'P',
                                           'V', 'P', '8', 'L', 0, 0, 0, 0};
    expect("refuses EXT_texture_webp source without image/webp mimeType",
           makeGlb(R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)"
                   R"("extensionsRequired":["EXT_texture_webp"],)"
                   R"("images":[{"bufferView":0,"mimeType":"image/png"}],)"
                   R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)"
                   R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":20}],)"
                   R"("buffers":[{"byteLength":20}]})", fakeWebp),
           AF_ERR_INVALID_ARG, "image/webp");

    /* 4. Declared image/webp but the bytes are not RIFF/WEBP: refused. */
    expect("refuses EXT_texture_webp whose bytes are not RIFF/WEBP",
           makeGlb(R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)"
                   R"("extensionsRequired":["EXT_texture_webp"],)"
                   R"("images":[{"bufferView":0,"mimeType":"image/webp"}],)"
                   R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)"
                   R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":20}],)"
                   R"("buffers":[{"byteLength":20}]})",
                   std::vector<uint8_t>(20, 0x41)),
           AF_ERR_INVALID_ARG, "RIFF/WebP");

    /* 5. A valid-looking WebP envelope whose payload cannot be decoded: a required WebP must fail the
          import, not drop the texture. (On Android the platform decoder rejects the fake payload; on
          the host there is no decoder at all. Both end in AF_ERR_UNSUPPORTED.) */
    expect("a required WebP that cannot be decoded fails the import",
           makeGlb(R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)"
                   R"("extensionsRequired":["EXT_texture_webp"],)"
                   R"("images":[{"bufferView":0,"mimeType":"image/webp"}],)"
                   R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)"
                   R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":20}],)"
                   R"("buffers":[{"byteLength":20}]})", fakeWebp),
           AF_ERR_UNSUPPORTED, "EXT_texture_webp");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
