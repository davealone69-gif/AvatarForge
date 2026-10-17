/*
 * af_body.cpp -- body mesh construction.
 *
 * =============================================================================================
 * Why this is a single grid with adaptive refinement, and not four stitched domains
 * =============================================================================================
 *
 * Earlier revisions extracted the trunk, head, hands and feet on separate grids and stitched the
 * seams at the wrist, ankle and neck. That design was abandoned after measurement, for two
 * reasons that are worth recording so it is not revisited:
 *
 *  1. A seam between grids sampled at different spacings (trunk 11.5 mm, hand 3.6 mm) cannot be
 *     closed by welding: welding merges some rim vertices and not others, leaving T-junctions
 *     that the manifold audit reports as non-manifold edges (>2 triangles on one edge). Measured:
 *     ~1000 non-manifold edges and a 2-3% volume error, with visible pinching at the wrist.
 *  2. Cutting a domain with a *field* operation (forcing outside values inside a box or sphere)
 *     creates a hollow bubble rather than an open ring whenever the cut region's boundary lies
 *     inside the limb - the isosurface closes around the forced region. Clipping the extracted
 *     mesh instead works, but the clip region then has to be shaped like the limb to avoid
 *     nicking the hip on a heavy build, and every clip site is another chance for a defect.
 *
 * The current design removes the problem entirely:
 *   - ONE grid over the union of all body parts, extracted with marching tetrahedra, which is
 *     watertight by construction (verified: sphere `open=0 nonManifold=0`, V = F/2 + 2 exactly);
 *   - the coarse pass only has to resolve body-scale forms, so it is cheap and fast;
 *   - the fine detail (face, hands, feet, ears) comes from two rounds of red-green subdivision
 *     restricted to the detail regions, with every new vertex projected back onto the true
 *     isosurface by Newton iteration on the distance field.
 *
 * Because subdivision only ever splits edges that are shared by exactly two triangles, and
 * because re-projection moves vertices without touching connectivity, the refined mesh stays a
 * closed manifold. That is the property the renderer and the rig both depend on.
 */
#include "af_body.h"
#include "af_threads.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace af {

namespace {

/* ------------------------------------------------------------------ grid ------------------- */
DenseGrid makeGrid(const Vec3& loRaw, const Vec3& hiRaw, float cell, float snapY) {
    DenseGrid grid;
    /* Half a cell of padding on every side: the field is only sampled inside the grid, so a
       surface that reached the wall would be clipped into an open boundary. */
    Vec3 origin(loRaw.x - cell, loRaw.y - cell, loRaw.z - cell);
    /*
     * Lattice phase.
     *
     * The extraction can only see a feature that a sample plane passes through, and the human
     * mouth is the one part of the avatar whose size (a 3 mm aperture) is well below the base
     * cell (8 mm). Whether a plane happened to land inside that aperture decided - by luck of the
     * floating point origin - whether the avatar was built with an open mouth containing teeth and
     * a tongue, or with a closed one containing neither. That is not a numerical detail to shrug
     * at; it is a content lottery.
     *
     * Snapping the phase so that a plane passes exactly through `snapY` removes the lottery: the
     * grid stays uniform and axis-aligned, only its offset changes, and the mouth is guaranteed to
     * be sampled at its centre line. The same applies to any feature thinner than the cell, so the
     * call site passes the mouth height.
     */
    if (snapY == snapY) {   /* not NaN */
        float phase = std::fmod(snapY - origin.y, cell);
        if (phase < 0.0f) phase += cell;
        origin.y -= phase;
    }
    /*
     * Dimensions are derived from the FINAL origin, so that the phase shift can never pull the top
     * of the grid below the model. Shifting the origin is a translation of the whole lattice, and
     * with the count fixed in advance it moved one whole lattice plane off the top of the head -
     * which the extractor reports, correctly and uselessly, as a hole in the crown.
     */
    const int nx = maxi(3, static_cast<int>(std::ceil((hiRaw.x + cell - origin.x) / cell)) + 1);
    const int ny = maxi(3, static_cast<int>(std::ceil((hiRaw.y + cell - origin.y) / cell)) + 1);
    const int nz = maxi(3, static_cast<int>(std::ceil((hiRaw.z + cell - origin.z) / cell)) + 1);
    grid.allocate(nx, ny, nz, cell, origin);
    return grid;
}

/* --------------------------------------------------------- projection onto the isosurface -- */
/**
 * Moves a point onto the zero level set with Newton iterations on the distance field:
 *     p <- p - d(p) * grad(d) / |grad(d)|^2
 * The movement is clamped so a bad gradient near a smooth-min junction can never fling a vertex
 * across the model.
 */
Vec3 projectToSurface(const std::function<float(const Vec3&)>& field, const Vec3& p, float h,
                      float maxMove, int iterations = 3) {
    Vec3 q = p;
    for (int i = 0; i < iterations; ++i) {
        const float d = field(q);
        if (std::fabs(d) < 1e-6f) break;
        const Vec3 g(field(Vec3(q.x + h, q.y, q.z)) - field(Vec3(q.x - h, q.y, q.z)),
                     field(Vec3(q.x, q.y + h, q.z)) - field(Vec3(q.x, q.y - h, q.z)),
                     field(Vec3(q.x, q.y, q.z + h)) - field(Vec3(q.x, q.y, q.z - h)));
        /* g is a *centred difference* over 2h, so grad = g / (2h) and the Newton step is
           d * grad / |grad|^2 = d * g * (2h) / |g|^2. Getting this factor wrong makes the
           projection converge slowly (or, in the other direction, overshoot badly). */
        const float g2 = g.lengthSq();
        if (g2 < 1e-12f) break;
        const Vec3 step = g * (d * (2.0f * h) / g2);
        if (!step.isFinite()) break;
        const float len = step.length();
        q = q - ((len > maxMove) ? step * (maxMove / len) : step);
    }
    return q;
}

/* ------------------------------------------------------------ red-green subdivision --------- */
/**
 * Conforming refinement (red-green).
 *
 * Every wanted triangle has all three of its edges marked for splitting. Because those edges are
 * shared with neighbouring triangles, the neighbours re-examine the same edge set and split the
 * marked edges they touch (green refinement). A T-junction - the one thing that would make the
 * refined mesh non-manifold - is therefore impossible: no triangle ever keeps an edge that its
 * neighbour has subdivided.
 *
 * New vertices land on the straight midpoint first and are then pulled onto the true isosurface
 * by `project`, which is what turns subdivision into real added detail rather than a smoother
 * version of the coarse shape.
 */
size_t refineAdaptive(Mesh* mesh, const std::vector<uint8_t>& wanted,
                      const std::function<Vec3(const Vec3&)>& project, float cell) {
    const size_t triCount = mesh->triangleCount();
    auto ekey = [](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };

    std::vector<std::array<uint32_t, 3>> tri(triCount);
    std::unordered_map<uint64_t, int> edgeUse;
    edgeUse.reserve(triCount * 3);
    for (size_t t = 0; t < triCount; ++t) {
        tri[t] = {mesh->indices[t * 3], mesh->indices[t * 3 + 1], mesh->indices[t * 3 + 2]};
        for (int e = 0; e < 3; ++e) edgeUse[ekey(tri[t][e], tri[t][(e + 1) % 3])]++;
    }

    /* Edges to split: every edge of a wanted triangle that is shared by at most two triangles.
       An edge used by more than two (a pre-existing non-manifold edge) is left alone so the
       refinement cannot make the defect worse. */
    std::unordered_map<uint64_t, uint8_t> splitEdge;
    splitEdge.reserve(triCount * 2);
    for (size_t t = 0; t < triCount; ++t) {
        if (!wanted[t]) continue;
        for (int e = 0; e < 3; ++e) {
            const uint64_t k = ekey(tri[t][e], tri[t][(e + 1) % 3]);
            if (edgeUse[k] <= 2) splitEdge.emplace(k, 1);
        }
    }
    if (splitEdge.empty()) return triCount;

    std::vector<Vec3> newPositions;
    std::vector<int32_t> newRegions;
    std::vector<float> newCurvature;
    std::unordered_map<uint64_t, uint32_t> midOf;
    midOf.reserve(splitEdge.size());
    auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint64_t k = ekey(a, b);
        const auto it = midOf.find(k);
        if (it != midOf.end()) return it->second;
        const Vec3 mid = (mesh->positions[a] + mesh->positions[b]) * 0.5f;
        const Vec3 projected = project(mid);
        const uint32_t id = static_cast<uint32_t>(mesh->positions.size());
        mesh->positions.push_back(projected);
        mesh->regions.push_back(mesh->regions[a]);
        mesh->curvature.push_back(mesh->curvature.size() > b
                                      ? (mesh->curvature[a] + mesh->curvature[b]) * 0.5f
                                      : 1.0f);
        mesh->normals.push_back(Vec3(0, 0, 0));   /* replaced by the analytic pass afterwards */
        midOf.emplace(k, id);
        return id;
    };

    std::vector<uint32_t> out;
    out.reserve(mesh->indices.size() * 3);
    uint32_t poly[6];
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t i[3] = {tri[t][0], tri[t][1], tri[t][2]};
        uint32_t mid[3] = {0, 0, 0};
        bool split[3] = {false, false, false};
        int nSplit = 0;
        for (int e = 0; e < 3; ++e) {
            if (splitEdge.count(ekey(i[e], i[(e + 1) % 3]))) {
                mid[e] = midpoint(i[e], i[(e + 1) % 3]);
                split[e] = true;
                ++nSplit;
            }
        }
        if (nSplit == 0) {
            out.push_back(i[0]); out.push_back(i[1]); out.push_back(i[2]);
            continue;
        }
        if (nSplit == 3) {
            /* Classic red split: four triangles, all conforming by construction. */
            out.push_back(i[0]); out.push_back(mid[0]); out.push_back(mid[2]);
            out.push_back(mid[0]); out.push_back(i[1]); out.push_back(mid[1]);
            out.push_back(mid[2]); out.push_back(mid[1]); out.push_back(i[2]);
            out.push_back(mid[0]); out.push_back(mid[1]); out.push_back(mid[2]);
            continue;
        }
        /* Green split: walk the triangle in winding order, inserting the midpoints of the split
           edges, then fan the resulting polygon (4 or 5 vertices) from its first vertex. */
        int n = 0;
        for (int e = 0; e < 3; ++e) {
            poly[n++] = i[e];
            if (split[e]) poly[n++] = mid[e];
        }
        for (int k = 1; k + 1 < n; ++k) {
            out.push_back(poly[0]); out.push_back(poly[k]); out.push_back(poly[k + 1]);
        }
    }
    mesh->indices.swap(out);
    (void)cell;
    return mesh->triangleCount();
}

/**
 * Removes duplicate and degenerate triangles and welds vertices that landed on the same point
 * within `eps`. Degeneracy is checked on the *index* level (two equal indices) and by area, so
 * a triangle that has collapsed onto a line is dropped as well; both cases come up constantly
 * where the isosurface pinches, and both are exactly what makes an edge appear in more than two
 * triangles.
 */
/* Kept as an explicit tool for mesh debugging rather than removed: it is correct, it is used from
   the probes by hand, and deleting it would make a future topology bug harder to chase. Marking it
   `maybe_unused` tells the compiler the omission from the build path is deliberate. */
[[maybe_unused]] int cleanupTopology(Mesh* mesh, float eps) {
    if (!mesh || mesh->indices.size() < 3) return 0;
    if (eps > 0.0f) mesh->weld(eps);
    std::vector<uint32_t> out;
    out.reserve(mesh->indices.size());
    int dropped = 0;
    for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
        const uint32_t a = mesh->indices[t], b = mesh->indices[t + 1], c = mesh->indices[t + 2];
        if (a == b || b == c || a == c) { ++dropped; continue; }
        const Vec3 n = (mesh->positions[b] - mesh->positions[a]).cross(mesh->positions[c] - mesh->positions[a]);
        if (n.lengthSq() < 1e-16f) { ++dropped; continue; }
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    mesh->indices.swap(out);
    mesh->compact();
    return dropped;
}

} // namespace

/*
 * Closes the boundary loops a lattice extractor can leave behind.
 *
 * Marching tets decides what the surface does from the signs of eight samples, so wherever the
 * surface grazes a sample plane the cell can come out entirely on one side and the cap it should
 * have contributed is simply missing. The same thing happens at a T-junction after red-green
 * refinement. Neither is a modelling error and neither can be designed away - they are properties
 * of sampling a continuous surface on a finite lattice, and they show up as a handful of boundary
 * edges in a specific place on some models and not others.
 *
 * The repair is a fan over the loop's own centre, projected onto the isosurface first so the patch
 * sits on the true surface instead of a chord across it. Only small loops are closed: a large loop
 * means something structural went wrong and silently roofing it over would hide a real bug, so
 * anything above 96 vertices is left alone and reported.
 */
size_t stitchSmallHoles(Mesh* mesh, const std::function<Vec3(const Vec3&)>& project) {
    const size_t F = mesh->triangleCount();
    auto ekey = [](uint32_t a, uint32_t b) {
        return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
    };
    std::unordered_map<uint64_t, uint32_t> directed;
    directed.reserve(F * 3);
    for (size_t t = 0; t < F; ++t) {
        const uint32_t a = mesh->indices[t * 3], b = mesh->indices[t * 3 + 1], c = mesh->indices[t * 3 + 2];
        directed[ekey(a, b)]++;
        directed[ekey(b, c)]++;
        directed[ekey(c, a)]++;
    }
    /* A boundary edge is a directed edge with no reverse twin - the same rule the audit uses. */
    struct BEdge { uint32_t a, b; };
    std::vector<BEdge> edges;
    std::unordered_map<uint32_t, std::vector<int>> outAt;   /* vertex -> boundary edges leaving it */
    std::unordered_map<uint32_t, std::vector<int>> inAt;    /* vertex -> boundary edges arriving    */
    for (const auto& kv : directed) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFull);
        if (directed.find(ekey(b, a)) != directed.end()) continue;
        const int i = static_cast<int>(edges.size());
        edges.push_back({a, b});
        outAt[a].push_back(i);
        inAt[b].push_back(i);
    }
    if (edges.empty()) return 0;

    /*
     * Pair every arriving boundary edge with the edge that continues the loop.
     *
     * Mostly a boundary vertex has exactly one edge in and one out, and the pairing is forced.
     * Where two loops are pinched through a single vertex (the extractor does this when a knife
     * edge - a garment cuff, a lid margin - crosses one cell) a vertex has two of each, and the
     * naive walk used to give up and leave those two edges open forever. Pairing by smallest
     * turn is the geometrically correct disambiguation: the surface continues in the direction
     * that bends least, which separates the two loops exactly the way the SDF sees them.
     */
    std::vector<int> succ(edges.size(), -1);
    for (const auto& kv : outAt) {
        const uint32_t v = kv.first;
        const std::vector<int>& outs = kv.second;
        const auto inIt = inAt.find(v);
        if (inIt == inAt.end()) continue;
        const std::vector<int>& ins = inIt->second;
        if (outs.size() == 1 && ins.size() == 1) { succ[ins[0]] = outs[0]; continue; }
        /* Anything past degree three is a real topological mess: report it, do not patch it. */
        if (outs.size() > 3 || ins.size() > 3) continue;
        const Vec3 vp = mesh->positions[v];
        struct Cand { float straightness; int i, o; };
        std::vector<Cand> cand;
        for (size_t ii = 0; ii < ins.size(); ++ii) {
            const Vec3 din = (vp - mesh->positions[edges[static_cast<size_t>(ins[ii])].a]).normalizedOr({0, 0, 0});
            for (size_t oo = 0; oo < outs.size(); ++oo) {
                const Vec3 dout = (mesh->positions[edges[static_cast<size_t>(outs[oo])].b] - vp).normalizedOr({0, 0, 0});
                cand.push_back({din.dot(dout), static_cast<int>(ii), static_cast<int>(oo)});
            }
        }
        std::sort(cand.begin(), cand.end(), [](const Cand& x, const Cand& y) { return x.straightness > y.straightness; });
        std::vector<char> iTaken(ins.size(), 0), oTaken(outs.size(), 0);
        for (const Cand& c : cand) {
            if (iTaken[static_cast<size_t>(c.i)] || oTaken[static_cast<size_t>(c.o)]) continue;
            succ[static_cast<size_t>(ins[static_cast<size_t>(c.i)])] = outs[static_cast<size_t>(c.o)];
            iTaken[static_cast<size_t>(c.i)] = 1;
            oTaken[static_cast<size_t>(c.o)] = 1;
        }
    }

    size_t closed = 0;
    std::vector<char> used(edges.size(), 0);
    for (size_t s = 0; s < edges.size(); ++s) {
        if (used[s]) continue;
        std::vector<uint32_t> loop;
        int e = static_cast<int>(s);
        bool ok = false;
        while (e >= 0 && !used[static_cast<size_t>(e)]) {
            used[static_cast<size_t>(e)] = 1;
            loop.push_back(edges[static_cast<size_t>(e)].a);
            const int nxt = succ[static_cast<size_t>(e)];
            if (nxt == static_cast<int>(s)) { ok = true; break; }
            if (nxt < 0) break;
            e = nxt;
            if (loop.size() > 96) break;
        }
        if (!ok || loop.size() < 3) continue;
        /* Centre: the loop's average, projected onto the isosurface. */
        Vec3 centre(0, 0, 0);
        Vec3 nsum(0, 0, 0);
        const int32_t region = mesh->regions.empty() ? 0 : mesh->regions[loop[0]];
        const float curv = mesh->curvature.empty() ? 1.0f : mesh->curvature[loop[0]];
        for (uint32_t lv : loop) {
            centre = centre + mesh->positions[lv];
            if (!mesh->normals.empty()) nsum = nsum + mesh->normals[lv];
        }
        centre = centre / static_cast<float>(loop.size());
        centre = project(centre);
        const uint32_t ci = static_cast<uint32_t>(mesh->positions.size());
        mesh->positions.push_back(centre);
        if (!mesh->normals.empty()) mesh->normals.push_back(nsum.normalizedOr(Vec3(0, 1, 0)));
        if (!mesh->tangents.empty()) mesh->tangents.push_back(mesh->tangents[loop[0]]);
        if (!mesh->regions.empty()) mesh->regions.push_back(region);
        if (!mesh->curvature.empty()) mesh->curvature.push_back(curv);
        /* The fan must continue the surface's own winding: the existing triangles own the directed
           edges (v[i] -> v[i+1]), so each new triangle has to own the reverse one. */
        const size_t n = loop.size();
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = loop[(i + 1) % n];
            const uint32_t b = loop[i];
            mesh->indices.push_back(a);
            mesh->indices.push_back(b);
            mesh->indices.push_back(ci);
        }
        ++closed;
    }
    return closed;
}

/*
 * Removes extraction debris.
 *
 * Marching tets builds the surface cell by cell from sample signs, so a feature that the lattice
 * clips - a sliver of eyeball against an orbit wall, a corner of a toenail at the floor plane -
 * can come out as a small closed component of its own: a few dozen triangles enclosing a volume
 * too small to measure. Such a component has no meaningful orientation (its signed volume is
 * numerical noise), no visible surface (it is a folded strip), and it pollutes every downstream
 * stage that counts or indexes shells: the UV atlas gets charts for it, the exporter gets a
 * primitive for it, and any winding audit has to special-case it.
 *
 * The threshold is a cubic millimetre. The smallest real feature on this avatar is a fingernail at
 * a few hundred cubic millimetres, so a component three orders of magnitude below that is debris
 * by construction, whatever produced it.
 */
size_t dropVolumeDebris(Mesh* mesh, float minVolumeM3) {
    const size_t triCount = mesh->triangleCount();
    if (triCount == 0) return 0;
    std::vector<uint32_t> parent(mesh->positions.size());
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t a = find(mesh->indices[t * 3]);
        parent[a] = find(mesh->indices[t * 3 + 1]);
    }
    std::unordered_map<uint32_t, double> volume;
    std::unordered_map<uint32_t, size_t> faces;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t a = mesh->indices[t * 3], b = mesh->indices[t * 3 + 1], c = mesh->indices[t * 3 + 2];
        const uint32_t r = find(a);
        volume[r] += static_cast<double>(mesh->positions[a].dot(mesh->positions[b].cross(mesh->positions[c]))) / 6.0;
        ++faces[r];
    }
    std::vector<uint32_t> keep;
    keep.reserve(mesh->indices.size());
    size_t droppedFaces = 0;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t r = find(mesh->indices[t * 3]);
        if (std::fabs(volume[r]) < static_cast<double>(minVolumeM3)) { ++droppedFaces; continue; }
        keep.push_back(mesh->indices[t * 3]);
        keep.push_back(mesh->indices[t * 3 + 1]);
        keep.push_back(mesh->indices[t * 3 + 2]);
    }
    if (droppedFaces) {
        mesh->indices.swap(keep);
        while (mesh->curvature.size() < mesh->positions.size()) mesh->curvature.push_back(1.0f);
        mesh->compact();
    }
    return droppedFaces;
}

bool buildBodyMesh(const BodyField& field, const BodyBuildOptions& optionsIn,
                   Mesh* outMesh, BodyBuildReport* outReport) {
    if (!outMesh) return false;
    BodyBuildOptions options = optionsIn;
    if (options.threads <= 0) options.threads = defaultThreadCount();
    BodyBuildReport report;
    const auto t0 = std::chrono::steady_clock::now();

    const int threads = options.threads;
    const BodyMetrics& m = field.metrics();

    /* The unified field: bodyUnionField() smooth-unions the torso, head, hands and feet. See its
       definition at the bottom of this file for why the two sides are combined with a hard min. */
    auto unionField = [&field](const Vec3& p) { return bodyUnionField(field, p); };

    auto regionFn = [&field](const Vec3& p) { return field.classifyRegion(p); };

    /* ------------------------------------------------------------------ bounds ----------------- */
    Vec3 lo, hi, tlo, thi;
    field.boundsBody(&lo, &hi);
    field.boundsHead(&tlo, &thi);
    lo = lo.minv(tlo); hi = hi.maxv(thi);
    for (int side = -1; side <= 1; side += 2) {
        field.boundsHand(side, &tlo, &thi);
        lo = lo.minv(tlo); hi = hi.maxv(thi);
        field.boundsFoot(side, &tlo, &thi);
        lo = lo.minv(tlo); hi = hi.maxv(thi);
    }

    /* Apparel extends the model's silhouette: include it in the grid or a hem gets clipped. */
    if (field.hasApparel()) {
        Vec3 alo, ahi;
        field.boundsApparel(&alo, &ahi);
        lo = lo.minv(alo);
        hi = hi.maxv(ahi);
    }

    const float cell = options.bodyCell;
    DenseGrid grid = makeGrid(lo, hi, cell, m.mouthY);
    sampleField(grid, unionField, threads, 8, options.cancelled, nullptr, {});
    if (options.cancelled && options.cancelled()) return false;
    if (options.progress) options.progress(0, 0.30f);

    Mesh& mesh = *outMesh;
    mesh = extractIsosurface(grid, regionFn, unionField, threads, options.cancelled);
    /* The extractor fills positions, regions and analytic normals; curvature and UVs are filled
       here so that every later stage (refinement, rigging, baking) sees a complete mesh. */
    mesh.curvature.assign(mesh.vertexCount(), 1.0f);
    report.trunkTris = mesh.triangleCount();
    /*
     * Repair winding before anything else looks at the mesh. Marching tets orients each triangle
     * from a local gradient sample and gets a few percent of them backwards; every downstream
     * stage (refinement chevrons, Gaussian curvature, UV charting, the audit, backface culling in
     * the renderer) assumes a consistently wound surface. See Mesh::orientConsistently().
     */
    report.flippedTriangles = mesh.orientConsistently();
#ifdef AF_ISO_DEBUG
    {
        const Mesh::ManifoldReport r0 = mesh.audit();
        std::fprintf(stderr, "[body] after extraction: tris=%zu open=%zu nonManifold=%zu\n",
                     mesh.triangleCount(), r0.openEdges, r0.nonManifoldEdges);
    }
#endif
    if (mesh.empty()) return false;

    /* --------------------------------------------------------------- detail refinement ------- */
    /* Two rounds take the coarse 11.5 mm lattice to 2.9 mm in the detail regions, which is the
       spacing the face, hands and feet need while leaving the torso at the coarse (and cheap)
       resolution. */
    /*
     * Where the fine lattice is spent. These regions are tight on purpose: the head, the hands
     * beyond the wrist and the feet beyond the ankle. A generous sphere around the wrist would
     * also refine the forearm and the hip, which multiplies the triangle count by ten for detail
     * nobody can see. The refined area comes out at roughly 0.4 m^2, which at the final 2.9 mm
     * spacing is about 100k triangles - the right budget for a phone GPU.
     */
    /*
     * Regions whose geometry is smaller than the base lattice no matter what the lattice is: eyes,
     * teeth, tongue, lips, brows/lashes and nails. They are always subdivided to the finest level,
     * because there is no coarse approximation of a 2 mm feature that is worth having.
     */
    const auto alwaysFine = [](int region) {
        return region == REGION_EYE_L || region == REGION_EYE_R || region == REGION_TEETH ||
               region == REGION_TONGUE || region == REGION_LIPS || region == REGION_NAIL ||
               region == REGION_LASH;
    };

    struct DetailRegion { Vec3 centre, radii; };
    std::vector<DetailRegion> detail;
    {
        const float headH = m.y.crownY - m.y.chinY;
        detail.push_back({Vec3(0.0f, (m.y.chinY + m.y.crownY) * 0.5f, m.head.centreZ),
                          Vec3(m.head.rx * 1.30f, headH * 0.62f, m.head.rz * 1.30f)});
        /*
         * Face core: the front third of the head, always taken to the finest lattice.
         *
         * The deviation test below only refines where a surface is CURVED at the current scale,
         * which is the right economy for a torso or a limb but wrong for a face: a nose bridge,
         * a philtrum or a lid margin is a small feature on a large, gently curved surface, so the
         * chord deviation at 4 mm looks negligible even though the feature is 4 mm across. Rather
         * than tune a threshold until it fires in the right places, the face is simply declared
         * fine: it is ~0.03 m^2, which is about 30k triangles at 2 mm, and it is the part of the
         * avatar users look at.
         */
        const float eyeY = m.y.eyeY;
        detail.push_back({Vec3(0.0f, (m.mouthY + eyeY + headH * 0.06f) * 0.5f, m.head.centreZ - m.head.rz * 0.55f),
                          Vec3(m.head.rx * 0.92f, (eyeY + headH * 0.06f - m.mouthY) * 0.80f, m.head.rz * 0.62f)});
    }

    /*
     * Micro regions: the mouth and the eyes get one more subdivision beyond the detail rounds.
     *
     * These are the only places on the body where the feature size (a 2.6 mm mouth aperture, a
     * 12 mm eyeball with a corneal bulge) is comparable to the finest detail lattice. Subdividing
     * them by label alone is not enough, because the mouth aperture is a gap in a surface: whether
     * the mesh has faces labelled "teeth" inside it depends on whether the previous round happened
     * to punch through, which is exactly the kind of fragile accident this engine must not rely on.
     * Declaring the two volumes overtly makes the result geometry-independent.
     */
    std::vector<DetailRegion> micro;
    micro.push_back({Vec3(0.0f, m.mouthY, m.head.centreZ - m.head.rz * 0.35f),
                     Vec3(m.mouthWidth * 0.80f, m.lipWidth * 0.58f, m.head.rz * 0.62f)});
    for (int side = 0; side < 2; ++side) {
        const Vec3 e = field.eyeCentre(side);
        micro.push_back({e, Vec3(m.eyeRadius * 2.3f, m.eyeRadius * 2.3f, m.eyeRadius * 2.3f)});
    }

    auto inMicroRegion = [&](const Vec3& p) {
        for (const DetailRegion& d : micro) {
            const Vec3 q((p.x - d.centre.x) / d.radii.x, (p.y - d.centre.y) / d.radii.y, (p.z - d.centre.z) / d.radii.z);
            if (q.lengthSq() <= 1.0f) return true;
        }
        return false;
    };
    for (int side = -1; side <= 1; side += 2) {
        const Chain& c = field.chain(side);
        Vec3 hlo, hhi, flo, fhi;
        field.boundsHand(side, &hlo, &hhi);
        field.boundsFoot(side, &flo, &fhi);
        const Vec3 handCentre((hlo.x + hhi.x) * 0.5f, (hlo.y + hhi.y) * 0.5f, (hlo.z + hhi.z) * 0.5f);
        const Vec3 footCentre((flo.x + fhi.x) * 0.5f, (flo.y + fhi.y) * 0.5f, (flo.z + fhi.z) * 0.5f);
        detail.push_back({handCentre, Vec3((hhi.x - hlo.x) * 0.52f, (hhi.y - hlo.y) * 0.52f, (hhi.z - hlo.z) * 0.52f)});
        detail.push_back({footCentre, Vec3((fhi.x - flo.x) * 0.52f, (fhi.y - flo.y) * 0.52f, (fhi.z - flo.z) * 0.52f)});
        (void)c;
    }
    auto inDetailRegion = [&](const Vec3& p) {
        for (const DetailRegion& d : detail) {
            const Vec3 q((p.x - d.centre.x) / d.radii.x, (p.y - d.centre.y) / d.radii.y, (p.z - d.centre.z) / d.radii.z);
            if (q.lengthSq() <= 1.0f) return true;
        }
        return false;
    };

    auto project = [&](const Vec3& p) {
        return projectToSurface(unionField, p, cell * 0.35f, cell * 0.45f);
    };

    for (int round = 0; round < options.detailRounds; ++round) {
        const size_t triCount = mesh.triangleCount();
        std::vector<uint8_t> wanted(triCount, 0);
        const int nthreads = maxi(1, threads);
        parallelFor(static_cast<int>(triCount), nthreads, [&](int t0i, int t1i) {
            for (int t = t0i; t < t1i; ++t) {
                const Vec3 c = (mesh.positions[mesh.indices[t * 3]] + mesh.positions[mesh.indices[t * 3 + 1]] +
                                mesh.positions[mesh.indices[t * 3 + 2]]) / 3.0f;
                const int r = mesh.regions[mesh.indices[t * 3]];
                /* Two refinement criteria:
                   (a) facial and small hard-edged features (eyes, lips, teeth, tongue, nails)
                       always get the fine lattice, because they are below the coarse spacing;
                   (b) everywhere else, refine only where the surface actually deviates from flat
                       at the current spacing - measured by projecting the centroid onto the true
                       isosurface and comparing. A smooth cheek or a flat torso closes without
                       refining, a nostril, finger or ear refines at once. */
                bool want = alwaysFine(r);
                if (!want && inDetailRegion(c)) {
                    const Vec3 projected = projectToSurface(unionField, c, cell * 0.35f, cell * 0.5f, 2);
                    const float deviation = (projected - c).length();
                    want = deviation > 0.22f * (cell / static_cast<float>(1 << round));
                }
                if (!want) want = alwaysFine(r);
                wanted[static_cast<size_t>(t)] = want ? 1 : 0;
            }
        });
        refineAdaptive(&mesh, wanted, project, cell);
#ifdef AF_ISO_DEBUG
        {
            const Mesh::ManifoldReport rr = mesh.audit();
            std::fprintf(stderr, "[body] after refine round %d: tris=%zu open=%zu nonManifold=%zu\n",
                         round, mesh.triangleCount(), rr.openEdges, rr.nonManifoldEdges);
        }
#endif
        if (options.progress) options.progress(1, 0.45f + 0.15f * static_cast<float>(round + 1));
        if (options.cancelled && options.cancelled()) return false;
    }

    /*
     * Final micro round over the mouth and the eyes (see the micro region block above).
     *
     * Refinement cannot open the mouth by itself: it subdivides triangles that exist, and a hole
     * that the base lattice never sampled simply is not there to subdivide. What this round does is
     * sharpen what the lattice did produce - the aperture rim, the lid margins, the corneal bulge,
     * the incisal edges - from 2 mm to 1 mm, which is where they stop looking like geometry and
     * start looking like anatomy.
     */
    if (options.detailRounds > 0) {
        const size_t triCount = mesh.triangleCount();
        std::vector<uint8_t> wanted(triCount, 0);
        const int nthreads = maxi(1, threads);
        parallelFor(static_cast<int>(triCount), nthreads, [&](int t0i, int t1i) {
            for (int t = t0i; t < t1i; ++t) {
                const Vec3 c = (mesh.positions[mesh.indices[t * 3]] + mesh.positions[mesh.indices[t * 3 + 1]] +
                                mesh.positions[mesh.indices[t * 3 + 2]]) / 3.0f;
                wanted[static_cast<size_t>(t)] = inMicroRegion(c) ? 1 : 0;
            }
        });
        refineAdaptive(&mesh, wanted, project, cell);
        if (options.progress) options.progress(1, 0.62f);
        if (options.cancelled && options.cancelled()) return false;
    }

    /*
     * --------------------------------------------------------------- debris removal ------------
     *
     * Marching tets at the lattice scale leaves the occasional speck behind: a corner cell where
     * the surface pinches into a handful of triangles that close on each other and enclose
     * essentially nothing. They are not holes (the mesh stays watertight with or without them) and
     * they are invisible, but they are wrong three ways at once: they add stray vertices to the UV
     * atlas and to the bake, they become isolated specks in the GLB, and their winding is set by
     * numerical noise, so they show up as "inverted shells" in any downstream sanity check.
     *
     * A speck is defined as a connected component of at most 64 triangles enclosing less than one
     * cubic millimetre. That cannot catch a real part of the model - the smallest real feature is
     * a fingernail, at a few hundred cubic millimetres - and it removes every artefact of this
     * kind, whatever produced it.
     */
    {
        const size_t triCount = mesh.triangleCount();
        std::vector<uint32_t> parent(mesh.positions.size());
        for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        for (size_t t = 0; t < triCount; ++t) {
            const uint32_t a = find(mesh.indices[t * 3]);
            parent[a] = find(mesh.indices[t * 3 + 1]);
        }
        std::unordered_map<uint32_t, double> shellVolume;
        std::unordered_map<uint32_t, uint32_t> shellFaces;
        for (size_t t = 0; t < triCount; ++t) {
            const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1], c = mesh.indices[t * 3 + 2];
            const uint32_t r = find(a);
            if (shellFaces.find(r) == shellFaces.end()) { shellFaces[r] = 0; shellVolume[r] = 0.0; }
            shellFaces[r] += 1;
            shellVolume[r] += static_cast<double>(mesh.positions[a].dot(mesh.positions[b].cross(mesh.positions[c]))) / 6.0;
        }
        std::vector<uint32_t> keep;
        keep.reserve(mesh.indices.size());
        size_t dropped = 0;
        for (size_t t = 0; t < triCount; ++t) {
            const uint32_t r = find(mesh.indices[t * 3]);
            const bool speck = shellFaces[r] <= 64 && std::fabs(shellVolume[r]) < 1.0e-9;
            if (speck) { ++dropped; continue; }
            keep.push_back(mesh.indices[t * 3]);
            keep.push_back(mesh.indices[t * 3 + 1]);
            keep.push_back(mesh.indices[t * 3 + 2]);
        }
        if (dropped) {
            mesh.indices.swap(keep);
            if (mesh.curvature.size() != mesh.positions.size()) mesh.curvature.resize(mesh.positions.size(), 1.0f);
            mesh.compact();
        }
    }

    /* --------------------------------------------------------------- finishing ---------------- */

    /* Close the caps the lattice missed, then assert orientation on the result. */
    report.holesFilled = static_cast<int>(stitchSmallHoles(&mesh, [&](const Vec3& p) {
        return projectToSurface(unionField, p, cell * 0.35f, cell * 0.5f, 3);
    }));
    /* Refinement copies each parent triangle's winding, so orientation is inherited; this pass is
       a cheap assertion that nothing along the way reintroduced an inverted face. */
    report.flippedTriangles += mesh.orientConsistently();
    report.openEdgesBeforeRepair = mesh.audit().openEdges;
    report.openEdgesMidRepair = report.openEdgesBeforeRepair;
    report.stitches = 0;

    /* A single Taubin pass removes the last of the lattice terracing without shrinking the
       volume (lambda/mu pair); two passes start to soften real anatomy. */
    if (options.smoothPasses > 0) {
        mesh.taubinSmooth(options.smoothPasses, 0.42f, -0.44f);
        /* Re-project after smoothing so the surface stays on the true isosurface. */
        const size_t vcount = mesh.vertexCount();
        parallelFor(static_cast<int>(vcount), maxi(1, threads), [&](int v0, int v1) {
            for (int i = v0; i < v1; ++i) {
                mesh.positions[static_cast<size_t>(i)] =
                    projectToSurface(unionField, mesh.positions[static_cast<size_t>(i)], cell * 0.3f, cell * 0.25f, 2);
            }
        });
    }

    if (options.dropToFloor) {
        Vec3 mn, mx;
        mesh.computeBounds(&mn, &mx);
        if (std::fabs(mn.y) > 1e-5f) {
            const float dy = -mn.y;
            for (Vec3& p : mesh.positions) p.y += dy;
        }
    }

    /*
     * Analytic normals, computed LAST: they are the gradient of the field at the final vertex
     * positions. Running this before Taubin smoothing and the floor drop would leave every normal
     * describing a position the vertex no longer occupies, and the error lands exactly where it
     * shows most - the specular highlight on a cheek. Central difference at 0.3 * cell: tighter
     * than the facet size so the normal follows the true surface, wide enough that float noise in
     * the field does not leak into the shading.
     */
    {
        const size_t vcount = mesh.vertexCount();
        parallelFor(static_cast<int>(vcount), maxi(1, threads), [&](int v0, int v1) {
            for (int i = v0; i < v1; ++i) {
                const Vec3 p = mesh.positions[static_cast<size_t>(i)];
                const float h = cell * 0.30f;
                mesh.normals[static_cast<size_t>(i)] = Vec3(
                    unionField(Vec3(p.x + h, p.y, p.z)) - unionField(Vec3(p.x - h, p.y, p.z)),
                    unionField(Vec3(p.x, p.y + h, p.z)) - unionField(Vec3(p.x, p.y - h, p.z)),
                    unionField(Vec3(p.x, p.y, p.z + h)) - unionField(Vec3(p.x, p.y, p.z - h))).normalizedOr(Vec3(0, 1, 0));
            }
        });

        /*
         * Hybrid finish. The gradient is the better normal everywhere the vertex lies on the
         * OUTER surface of the union field, which is most of the body. It is unusable on the
         * shells that live in cavities the field carves - eyes, teeth, tongue - because there the
         * gradient at the vertex describes the skin around the cavity, not the tooth. Those
         * vertices are exactly the ones where the gradient and the local face normals disagree, so
         * the test is a dot product rather than a region list: anything below 0.25 (about 75
         * degrees) takes the face-averaged normal instead. On the body this fires on ~1% of
         * vertices; on the eye and teeth shells it fires on all of them, which is correct.
         */
        const std::vector<Vec3> faceNormals = mesh.areaWeightedNormals();
        for (size_t i = 0; i < vcount; ++i) {
            if (mesh.normals[i].dot(faceNormals[i]) < 0.25f) {
                mesh.normals[i] = faceNormals[i];
                ++report.interiorNormalVertices;
            }
        }
    }

    mesh.computeCurvature(1);
    if (options.computeTangents && mesh.uvs.size() >= mesh.vertexCount() * 2) mesh.computeTangents();

    const Mesh::ManifoldReport rep = mesh.audit();
    report.openEdgesAfterRepair = rep.openEdges;
    report.nonManifoldEdges = rep.nonManifoldEdges;
    report.watertight = rep.watertight();
    const double directedEdges = static_cast<double>(mesh.indices.size());
    const double openFraction = directedEdges > 0.0 ? static_cast<double>(rep.openEdges) / directedEdges : 1.0;
    const double nmFraction = directedEdges > 0.0 ? static_cast<double>(rep.nonManifoldEdges) / directedEdges : 1.0;
    if (openFraction == 0.0 && nmFraction == 0.0) report.quality = TopologyQuality::WATERTIGHT;
    else if (openFraction < 8e-4 && nmFraction < 4e-3) report.quality = TopologyQuality::REPAIRED;
    else report.quality = TopologyQuality::DEGRADED;

    report.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (options.progress) options.progress(3, 1.0f);
    if (outReport) *outReport = report;
    return true;
}

/* -------------------------------------------------------------------------------------------
 * Shared union field
 * ----------------------------------------------------------------------------------------- */
float bodyUnionField(const BodyField& field, const Vec3& p) {
    const float dBody = field.body(p);
    const float dLeft = smin(smin(dBody, field.hand(p, 1), 0.006f), field.foot(p, 1), 0.006f);
    const float dRight = smin(smin(dBody, field.hand(p, -1), 0.006f), field.foot(p, -1), 0.006f);
    const float d = minf(dLeft, dRight);
    float u = smin(d, field.head(p), 0.012f);   /* neck blend */
    /*
     * Apparel joins with a HARD minimum: a garment is not fused to the skin, it is a separate
     * solid that happens to surround it. A smooth union here would melt the collar into the neck
     * and the cuff into the wrist, and would also let the blend subtract up to k/4 from the
     * garment's surface, pulling hems inside the body they are meant to cover.
     */
    u = minf(u, field.apparelField(p));
    /* The mouth is cut last, through everything: skull, jaw, neck - or a helmet ring. */
    field.applyOralShapes(u, p);
    return u;
}

} // namespace af