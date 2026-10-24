#pragma once
#include "../src/af_png.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static constexpr int kRenderTestWidth = 256;
static constexpr int kRenderTestHeight = 384;

static void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

// A deliberately tiny, opaque red triangle facing the default camera. This fixture is not shipped
// in the app and cannot be mistaken for generated avatar geometry. The untextured variant omits
// UVs deliberately: zero derivatives plus anisotropic 1x1 fallback sampling regressed alpha.
static std::vector<uint8_t> triangleGlb(int textureSize = 0) {
    const float data[] = {-0.55f, 0.30f, 0, 0, 1.60f, 0, 0.55f, 0.30f, 0,
                          0, 0, -1, 0, 0, -1, 0, 0, -1};
    const uint32_t indices[] = {0, 1, 2};
    std::vector<uint8_t> bin(sizeof(data) + sizeof(indices));
    std::memcpy(bin.data(), data, sizeof(data));
    std::memcpy(bin.data() + sizeof(data), indices, sizeof(indices));
    std::string attributes = "\"POSITION\":0,\"NORMAL\":1";
    std::string materialTexture, extraViews, extraAccessors, images;
    if (textureSize > 0) {
        const float uv[] = {0, 0, 0.5f, 1, 1, 0};
        const auto* uvBytes = reinterpret_cast<const uint8_t*>(uv);
        bin.insert(bin.end(), uvBytes, uvBytes + sizeof(uv));
        std::vector<uint8_t> white(size_t(textureSize) * textureSize * 4, 255), png;
        if (!af::encodePng(white.data(), textureSize, textureSize, 4, &png)) return {};
        const size_t imageOffset = bin.size();
        bin.insert(bin.end(), png.begin(), png.end());
        attributes += ",\"TEXCOORD_0\":3";
        materialTexture = ",\"baseColorTexture\":{\"index\":0}";
        extraViews = ",{\"buffer\":0,\"byteOffset\":84,\"byteLength\":24},"
                     "{\"buffer\":0,\"byteOffset\":" + std::to_string(imageOffset) +
                     ",\"byteLength\":" + std::to_string(png.size()) + "}";
        extraAccessors = ",{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}";
        images = ",\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}]";
    }
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)" + attributes + R"(},
      "indices":2,"material":0}]}],"materials":[{"pbrMetallicRoughness":{
      "baseColorFactor":[1,0.015,0.01,1],"metallicFactor":0,"roughnessFactor":0.9)" + materialTexture + R"(}}],
      "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
      {"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":12})" + extraViews + R"(],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
      "min":[-0.55,0.3,0],"max":[0.55,1.6,0]},
      {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
      {"bufferView":2,"componentType":5125,"count":3,"type":"SCALAR"})" + extraAccessors + "]" + images + "}";
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin.push_back(0);
    std::vector<uint8_t> result;
    appendU32(result, 0x46546c67); appendU32(result, 2);
    appendU32(result, uint32_t(12 + 8 + json.size() + 8 + bin.size()));
    appendU32(result, uint32_t(json.size())); appendU32(result, 0x4e4f534a);
    result.insert(result.end(), json.begin(), json.end());
    appendU32(result, uint32_t(bin.size())); appendU32(result, 0x004e4942);
    result.insert(result.end(), bin.begin(), bin.end());
    return result;
}
