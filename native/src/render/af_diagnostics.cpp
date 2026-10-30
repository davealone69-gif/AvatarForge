#include "af_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace af {
namespace {
constexpr DiagnosticRungInfo kRungs[] = {
    {DiagnosticRung::CLEAR, "clear", "surface clears and presents with no geometry"},
    {DiagnosticRung::TRIANGLE, "triangle", "one non-indexed triangle is visible"},
    {DiagnosticRung::INDEXED_TRIANGLE, "indexed triangle", "one indexed triangle is visible"},
    {DiagnosticRung::TEXTURED_QUAD, "textured quad", "a sampled textured quad is visible"},
    {DiagnosticRung::LIT_SPHERE, "lit sphere", "a sphere responds to the lighting path"},
    {DiagnosticRung::SKINNED_MESH, "skinned mesh", "joint deformation changes the mesh"},
    {DiagnosticRung::BLENDSHAPE_MESH, "blendshape mesh", "a blendshape deformation is visible"},
    {DiagnosticRung::PRODUCTION_AVATAR, "production avatar", "the production avatar renders end-to-end"},
};

constexpr float kPi = 3.14159265358979323846f;

af_vertex vertex(float x, float y, float z, float nx, float ny, float nz,
                 float u, float v, float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    af_vertex out{};
    out.pos[0] = x; out.pos[1] = y; out.pos[2] = z;
    out.normal[0] = nx; out.normal[1] = ny; out.normal[2] = nz;
    out.tangent[0] = 1.0f; out.tangent[1] = 0.0f; out.tangent[2] = 0.0f; out.tangent[3] = 1.0f;
    out.uv[0] = u; out.uv[1] = v;
    out.color[0] = r; out.color[1] = g; out.color[2] = b; out.color[3] = 1.0f;
    out.weights[0] = 1.0f;
    return out;
}

void appendQuad(DiagnosticMesh* mesh, const af_vertex& bl, const af_vertex& br,
                const af_vertex& tr, const af_vertex& tl) {
    const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
    mesh->vertices.insert(mesh->vertices.end(), {bl, br, tr, tl});
    mesh->indices.insert(mesh->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void appendSphere(DiagnosticMesh* mesh, float radius, float centreY, float centreZ,
                  float timeSeconds, bool smileMorph) {
    constexpr int sectors = 24;
    constexpr int stacks = 16;
    const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
    for (int j = 0; j <= stacks; ++j) {
        const float v = float(j) / float(stacks);
        const float phi = v * kPi;
        const float sp = std::sin(phi), cp = std::cos(phi);
        for (int i = 0; i <= sectors; ++i) {
            const float u = float(i) / float(sectors);
            const float theta = u * 2.0f * kPi;
            const float nx = sp * std::cos(theta);
            const float ny = cp;
            const float nz = sp * std::sin(theta);
            float y = centreY + radius * ny;
            float z = centreZ + radius * nz;
            /* A small front-facing cheek/lip-region deformation makes the base mesh visibly change
               with the test weight while keeping the fixture deterministic and independent of an
               attached production engine. */
            if (smileMorph && nz > 0.45f && std::fabs(ny) < 0.42f) {
                const float smile = 0.5f + 0.5f * std::sin(timeSeconds * 1.35f);
                const float corner = std::max(0.0f, std::fabs(nx) - 0.08f) / 0.92f;
                y += radius * 0.30f * smile * corner * corner;
            }
            mesh->vertices.push_back(vertex(radius * nx, y, z,
                                            nx, ny, nz, u, v,
                                            0.88f, 0.62f, 0.46f));
        }
    }
    for (int j = 0; j < stacks; ++j) {
        for (int i = 0; i < sectors; ++i) {
            const std::uint32_t a = base + std::uint32_t(j * (sectors + 1) + i);
            const std::uint32_t b = a + std::uint32_t(sectors + 1);
            const std::uint32_t c = b + 1;
            const std::uint32_t d = a + 1;
            mesh->indices.insert(mesh->indices.end(), {a, c, b, a, d, c});
        }
    }
}

void appendSkinnedSegment(DiagnosticMesh* mesh, float x0, float y0, float x1, float y1,
                          float halfWidth, float angle, bool rotateAtTop, float colour) {
    const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
    const float hingeY = y0;
    auto point = [&](float x, float y) {
        if (rotateAtTop) {
            const float dx = x;
            const float dy = y - hingeY;
            const float c = std::cos(angle), s = std::sin(angle);
            x = dx * c - dy * s;
            y = hingeY + dx * s + dy * c;
        }
        return std::pair<float, float>{x, y};
    };
    const float corners[4][2] = {{x0 - halfWidth, y0}, {x0 + halfWidth, y0},
                                  {x1 + halfWidth, y1}, {x1 - halfWidth, y1}};
    for (int i = 0; i < 4; ++i) {
        const auto p = point(corners[i][0], corners[i][1]);
        mesh->vertices.push_back(vertex(p.first, p.second, 0.05f, 0.0f, 0.0f, 1.0f,
                                        float(i == 1 || i == 2), float(i >= 2),
                                        colour, 0.72f, 1.0f - colour * 0.35f));
    }
    mesh->indices.insert(mesh->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void appendMorphMouth(DiagnosticMesh* mesh, float timeSeconds) {
    constexpr int segments = 12;
    const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
    const float smile = 0.5f + 0.5f * std::sin(timeSeconds * 1.35f);
    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i <= segments; ++i) {
            const float x = -0.25f + 0.5f * float(i) / float(segments);
            const float t = x / 0.25f;
            const float curve = smile * 0.10f * t * t;
            const float y = 0.88f + curve + (row ? 0.035f : 0.0f);
            const float z = 0.495f - 0.06f * t * t;
            mesh->vertices.push_back(vertex(x, y, z, 0.0f, 0.0f, 1.0f,
                                            float(i) / float(segments), float(row),
                                            0.18f, 0.035f, 0.045f));
        }
    }
    for (int i = 0; i < segments; ++i) {
        const std::uint32_t a = base + std::uint32_t(i);
        const std::uint32_t b = base + std::uint32_t(i + 1);
        const std::uint32_t c = base + std::uint32_t(segments + 2 + i);
        const std::uint32_t d = base + std::uint32_t(segments + 1 + i);
        mesh->indices.insert(mesh->indices.end(), {a, b, c, a, c, d});
    }
}
}

const DiagnosticRungInfo& diagnosticRungInfo(DiagnosticRung rung) {
    const int index = static_cast<int>(rung);
    static constexpr DiagnosticRungInfo invalid{
        DiagnosticRung::CLEAR, "invalid", "no diagnostic rung"};
    return index >= 0 && index < kDiagnosticRungCount ? kRungs[index] : invalid;
}

bool diagnosticRungValid(int rung) { return rung >= 0 && rung < kDiagnosticRungCount; }

bool buildDiagnosticMesh(DiagnosticRung rung, float timeSeconds, DiagnosticMesh* out) {
    if (!out || !diagnosticRungValid(static_cast<int>(rung))) return false;
    out->vertices.clear();
    out->indices.clear();
    out->indexed = false;
    out->textured = false;
    switch (rung) {
    case DiagnosticRung::CLEAR:
    case DiagnosticRung::PRODUCTION_AVATAR:
        return true;
    case DiagnosticRung::TRIANGLE:
    case DiagnosticRung::INDEXED_TRIANGLE: {
        out->vertices = {
            vertex(-0.58f, 0.54f, 0.0f, 0, 0, 1, 0, 0, 1.0f, 0.25f, 0.18f),
            vertex( 0.58f, 0.54f, 0.0f, 0, 0, 1, 1, 0, 0.20f, 0.85f, 0.42f),
            vertex( 0.00f, 1.55f, 0.0f, 0, 0, 1, 0.5f, 1, 0.22f, 0.48f, 1.0f),
        };
        out->indexed = rung == DiagnosticRung::INDEXED_TRIANGLE;
        if (out->indexed) out->indices = {0, 1, 2};
        return true;
    }
    case DiagnosticRung::TEXTURED_QUAD:
        appendQuad(out,
                   vertex(-0.72f, 0.42f, 0.0f, 0, 0, 1, 0, 0),
                   vertex( 0.72f, 0.42f, 0.0f, 0, 0, 1, 1, 0),
                   vertex( 0.72f, 1.58f, 0.0f, 0, 0, 1, 1, 1),
                   vertex(-0.72f, 1.58f, 0.0f, 0, 0, 1, 0, 1));
        out->indexed = true;
        out->textured = true;
        return true;
    case DiagnosticRung::LIT_SPHERE:
        appendSphere(out, 0.58f, 1.0f, 0.0f, timeSeconds, false);
        out->indexed = true;
        return true;
    case DiagnosticRung::SKINNED_MESH: {
        const float angle = 0.72f * std::sin(timeSeconds * 1.7f);
        appendSkinnedSegment(out, -0.02f, 0.28f, -0.02f, 0.96f, 0.26f, 0.0f, false, 0.20f);
        appendSkinnedSegment(out, -0.02f, 0.88f, -0.02f, 1.68f, 0.22f, angle, true, 0.92f);
        out->indexed = true;
        return true;
    }
    case DiagnosticRung::BLENDSHAPE_MESH:
        appendSphere(out, 0.55f, 1.0f, 0.0f, timeSeconds, true);
        appendMorphMouth(out, timeSeconds);
        out->indexed = true;
        return true;
    case DiagnosticRung::COUNT:
        return false;
    }
    return false;
}

bool gpuTimestampDeltaToMilliseconds(std::uint64_t startTicks, std::uint64_t endTicks,
                                     std::uint32_t validBits, double periodNs,
                                     float* outMilliseconds) {
    if (outMilliseconds) *outMilliseconds = -1.0f;
    if (!outMilliseconds || validBits == 0 || validBits > 64 ||
        !std::isfinite(periodNs) || periodNs <= 0.0) return false;

    std::uint64_t ticks = endTicks - startTicks;
    if (validBits < 64) ticks &= (std::uint64_t{1} << validBits) - 1;
    const double milliseconds = static_cast<double>(ticks) * periodNs / 1000000.0;
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        milliseconds > static_cast<double>(std::numeric_limits<float>::max())) return false;
    *outMilliseconds = static_cast<float>(milliseconds);
    return std::isfinite(*outMilliseconds);
}
} // namespace af
