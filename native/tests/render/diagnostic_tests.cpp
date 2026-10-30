#include "../../src/render/af_diagnostics.h"
#include <cassert>
#include <cstring>
#include <limits>

int main() {
    for (int i = 0; i < af::kDiagnosticRungCount; ++i) {
        assert(af::diagnosticRungValid(i));
        const auto& info = af::diagnosticRungInfo(static_cast<af::DiagnosticRung>(i));
        assert(info.name && std::strlen(info.name) > 0);
        assert(info.passCriterion && std::strlen(info.passCriterion) > 0);
    }
    assert(!af::diagnosticRungValid(-1));
    assert(!af::diagnosticRungValid(af::kDiagnosticRungCount));
    assert(std::strcmp(af::diagnosticRungInfo(static_cast<af::DiagnosticRung>(99)).name,
                       "invalid") == 0);
    af::DiagnosticMesh mesh;
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::CLEAR, 0.0f, &mesh));
    assert(mesh.vertices.empty() && mesh.indices.empty());
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::TRIANGLE, 0.0f, &mesh));
    assert(mesh.vertices.size() == 3 && mesh.indices.empty() && !mesh.indexed);
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::INDEXED_TRIANGLE, 0.0f, &mesh));
    assert(mesh.vertices.size() == 3 && mesh.indices.size() == 3 && mesh.indexed);
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::TEXTURED_QUAD, 0.0f, &mesh));
    assert(mesh.vertices.size() == 4 && mesh.indices.size() == 6 && mesh.textured);
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::LIT_SPHERE, 0.0f, &mesh));
    assert(mesh.vertices.size() > 100 && mesh.indices.size() > 300 && mesh.indexed);
    af::DiagnosticMesh skinA, skinB;
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::SKINNED_MESH, 0.0f, &skinA));
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::SKINNED_MESH, 0.8f, &skinB));
    assert(skinA.vertices.size() == skinB.vertices.size() && skinA.indices.size() == skinB.indices.size());
    bool jointChanged = false;
    for (size_t i = 0; i < skinA.vertices.size(); ++i)
        jointChanged = jointChanged || skinA.vertices[i].pos[0] != skinB.vertices[i].pos[0] ||
                       skinA.vertices[i].pos[1] != skinB.vertices[i].pos[1];
    assert(jointChanged);
    af::DiagnosticMesh faceA, faceB;
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::BLENDSHAPE_MESH, 0.0f, &faceA));
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::BLENDSHAPE_MESH, 1.1f, &faceB));
    assert(faceA.vertices.size() == faceB.vertices.size() && faceA.indices.size() == faceB.indices.size());
    bool morphChanged = false;
    for (size_t i = 0; i < faceA.vertices.size(); ++i)
        morphChanged = morphChanged || faceA.vertices[i].pos[1] != faceB.vertices[i].pos[1];
    assert(morphChanged);
    assert(af::buildDiagnosticMesh(af::DiagnosticRung::PRODUCTION_AVATAR, 0.0f, &mesh));
    assert(mesh.vertices.empty() && mesh.indices.empty());
    assert(!af::buildDiagnosticMesh(static_cast<af::DiagnosticRung>(99), 0.0f, &mesh));
    const std::uint8_t background[3] = {14, 19, 28};
    const std::uint8_t image[] = {
        14, 19, 28, 255,  /* exact clear colour */
        16, 18, 30, 0,    /* within tolerance; alpha is irrelevant */
        18, 19, 28, 255,  /* outside tolerance in red */
        14, 19, 34, 255,  /* outside tolerance in blue */
    };
    std::uint64_t nonBackground = 99;
    assert(af::countNonBackgroundPixels(image, 4, background, 3, &nonBackground));
    assert(nonBackground == 2);
    assert(!af::countNonBackgroundPixels(nullptr, 4, background, 3, &nonBackground));
    assert(nonBackground == 0);
    assert(!af::countNonBackgroundPixels(image, 4, nullptr, 3, &nonBackground));
    assert(!af::countNonBackgroundPixels(image, 4, background, 3, nullptr));
    const af::GpuTimingResult unknown{};
    assert(!unknown.capability.supported && unknown.frameMs == -1.0f);
    float milliseconds = 99.0f;
    assert(af::gpuTimestampDeltaToMilliseconds(0xfffffffeu, 1u, 32, 1000000.0, &milliseconds));
    assert(milliseconds == 3.0f);
    assert(af::gpuTimestampDeltaToMilliseconds(100u, 125u, 64, 1000.0, &milliseconds));
    assert(milliseconds == 0.025f);
    assert(!af::gpuTimestampDeltaToMilliseconds(0u, 1u, 0, 1000000.0, &milliseconds));
    assert(milliseconds == -1.0f);
    assert(!af::gpuTimestampDeltaToMilliseconds(0u, 1u, 64,
                                                std::numeric_limits<double>::quiet_NaN(),
                                                &milliseconds));
    assert(milliseconds == -1.0f);
    return 0;
}
