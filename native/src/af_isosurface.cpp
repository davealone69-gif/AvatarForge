/*
 * af_isosurface.cpp -- grid sampling, marching tetrahedra and seam stitching.
 *
 * Marching tetrahedra (not marching cubes) is used deliberately: each cube is decomposed into
 * six tetrahedra, and a tetrahedron has a unique triangulation for every sign configuration.
 * That removes the ambiguous-face cracks that force marching-cubes implementations to fall back
 * on asymptotic deciders, so the extracted shell is manifold by construction.
 *
 * Vertex identity: a vertex is keyed by the *ordered pair of grid sample indices* of the edge it
 * lies on. Two cubes (and two tetrahedra inside a cube) that touch the same grid edge therefore
 * produce exactly ONE vertex, which is what makes the result welded and its edges shared by
 * exactly two triangles.
 *
 * Orientation: inside is negative, so the surface normal must point along +grad(field). Every
 * triangle is checked against the grid gradient at its centroid and flipped when needed. Doing
 * this analytically instead of trusting a hand-written case table is what removed the last of
 * the inward-facing triangle islands.
 */
#include "af_isosurface.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace af {

/* ================================================================== DenseGrid ================ */
float DenseGrid::sample(const Vec3& p) const {
    const float fx = (p.x - origin.x) / cell;
    const float fy = (p.y - origin.y) / cell;
    const float fz = (p.z - origin.z) / cell;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int z0 = static_cast<int>(std::floor(fz));
    const float tx = fx - x0, ty = fy - y0, tz = fz - z0;
    const int x1 = mini(x0 + 1, nx - 1), y1 = mini(y0 + 1, ny - 1), z1 = mini(z0 + 1, nz - 1);
    const int xc = clampi(x0, 0, nx - 1), yc = clampi(y0, 0, ny - 1), zc = clampi(z0, 0, nz - 1);
    const float c000 = at(xc, yc, zc),  c100 = at(x1, yc, zc);
    const float c010 = at(xc, y1, zc),  c110 = at(x1, y1, zc);
    const float c001 = at(xc, yc, z1),  c101 = at(x1, yc, z1);
    const float c011 = at(xc, y1, z1),  c111 = at(x1, y1, z1);
    const float c00 = c000 + (c100 - c000) * tx;
    const float c01 = c001 + (c101 - c001) * tx;
    const float c10 = c010 + (c110 - c010) * tx;
    const float c11 = c011 + (c111 - c011) * tx;
    const float c0 = c00 + (c10 - c00) * ty;
    const float c1 = c01 + (c11 - c01) * ty;
    return c0 + (c1 - c0) * tz;
}

Vec3 DenseGrid::gradient(const Vec3& p, float h) const {
    return Vec3(sample(Vec3(p.x + h, p.y, p.z)) - sample(Vec3(p.x - h, p.y, p.z)),
                sample(Vec3(p.x, p.y + h, p.z)) - sample(Vec3(p.x, p.y - h, p.z)),
                sample(Vec3(p.x, p.y, p.z + h)) - sample(Vec3(p.x, p.y, p.z - h))) / (2.0f * h);
}

/* ================================================================== sampling ================ */
namespace {
/** Applies the local cut rules to a sampled value. */
inline float applyCuts(const std::vector<LocalCut>& cuts, const Vec3& p, float v, float cell) {
    for (const LocalCut& c : cuts) {
        if (!c.contains(p)) continue;
        const float coord = (c.axis == 0) ? p.x : ((c.axis == 1) ? p.y : p.z);
        /* Samples exactly on the plane keep their true value: the extracted ring then lands on
           the plane and can be stitched to the neighbouring domain's ring. */
        if (std::fabs(coord - c.planeValue) < 0.25f * cell) continue;
        const bool beyond = c.keepBelow ? (coord > c.planeValue) : (coord < c.planeValue);
        if (beyond) return 1.0f;
    }
    return v;
}
} // namespace

void sampleField(DenseGrid& grid, const std::function<float(const Vec3&)>& field, int threads,
                 int blockSize, const std::function<bool()>& cancel,
                 const std::function<void(float)>& progress, const std::vector<LocalCut>& cuts) {
    const int bs = maxi(1, blockSize);
    const int bx = (grid.nx + bs - 1) / bs;
    const int by = (grid.ny + bs - 1) / bs;
    const int bz = (grid.nz + bs - 1) / bs;
    const int totalBlocks = bx * by * bz;

    std::vector<uint8_t> done(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, 0);
    std::atomic<int> finished{0};

    /* Evaluates one grid sample (with cut rules) and stores it. Returns its value. */
    auto evaluateSample = [&](int x, int y, int z) -> float {
        const size_t idx = grid.index(x, y, z);
        if (done[idx]) return grid.values[idx];
        const Vec3 p = grid.position(x, y, z);
        const float v = applyCuts(cuts, p, field(p), grid.cell);
        grid.values[idx] = v;
        done[idx] = 1;
        return v;
    };

    parallelFor(totalBlocks, threads, [&](int i0, int i1) {
        for (int block = i0; block < i1; ++block) {
            if (cancel && cancel()) return;
            const int bzi = block / (bx * by);
            const int rem = block % (bx * by);
            const int byi = rem / bx;
            const int bxi = rem % bx;
            const int x0 = bxi * bs, y0 = byi * bs, z0 = bzi * bs;
            const int x1 = mini(grid.nx - 1, x0 + bs);
            const int y1 = mini(grid.ny - 1, y0 + bs);
            const int z1 = mini(grid.nz - 1, z0 + bs);

            if (bs > 1 && x1 > x0 && y1 > y0 && z1 > z0) {
                /* Block skip: evaluate the eight corner samples (they are shared with the
                   neighbouring blocks and must be stored regardless), then decide whether the
                   interior can possibly contain the isosurface. The threshold is the block
                   diagonal, so a feature thinner than a block can never be skipped. */
                float minCorner = 1e30f;
                int corners[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                                     {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
                for (int k = 0; k < 8; ++k) {
                    minCorner = minf(minCorner, evaluateSample(corners[k][0], corners[k][1], corners[k][2]));
                }
                const float diag = static_cast<float>(bs) * grid.cell * 1.7320508f;
                if (minCorner > diag * 0.95f) {
                    const int f = ++finished;
                    if (progress && (f & 31) == 0) progress(static_cast<float>(f) / static_cast<float>(maxi(1, totalBlocks)));
                    continue;
                }
            }

            for (int z = z0; z <= z1; ++z) {
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) evaluateSample(x, y, z);
                }
            }
            const int f = ++finished;
            if (progress && (f & 31) == 0) progress(static_cast<float>(f) / static_cast<float>(maxi(1, totalBlocks)));
        }
    });
    if (progress) progress(1.0f);
}

/* ================================================================== extraction ============== */
namespace {

/* Cube corner offsets: bit0 = +x, bit1 = +y, bit2 = +z. */
constexpr int kCornerDx[8] = {0, 1, 0, 1, 0, 1, 0, 1};
constexpr int kCornerDy[8] = {0, 0, 1, 1, 0, 0, 1, 1};
constexpr int kCornerDz[8] = {0, 0, 0, 0, 1, 1, 1, 1};

/* The six tetrahedra of the decomposition, on the cube's main diagonal 0-7. */
constexpr int kTets[6][4] = {
    {0, 1, 3, 7}, {0, 5, 1, 7}, {0, 3, 2, 7},
    {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 6, 4, 7},
};

/* The tetrahedron's six edges, as (local vertex a, local vertex b). */
constexpr int kTetEdges[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};

inline uint64_t edgeKeyOf(size_t a, size_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b & 0xFFFFFFFFull);
}

} // namespace

Mesh extractIsosurface(const DenseGrid& grid, const std::function<int(const Vec3&)>& regionFn,
                       const std::function<float(const Vec3&)>& field, int threads,
                       const std::function<bool()>& cancel) {
    Mesh mesh;
    const int nxc = grid.nx - 1, nyc = grid.ny - 1, nzc = grid.nz - 1;
    if (nxc <= 0 || nyc <= 0 || nzc <= 0) return mesh;

    /* ---- pass 1: mark the cells that straddle the surface -------------------------------- */
    /* The marking is pure sign testing over the sampled grid and is parallelised over z slabs.
       Sorting the result makes the emission order identical no matter how many threads ran. */
    /*
     * Cell coordinates are packed into ONE integer and unpacked in pass 2. Each axis gets 21 bits
     * (2 million cells), and the packing is 64-bit: an earlier revision used 8 bits per axis into a
     * 32-bit word, which silently wrapped the x coordinate of every cell past the 255th into the y
     * field - turning a tall or wide grid into a scatter of cells evaluated at the wrong place, and
     * so into holes that appear and disappear with the model's proportions. The cost of being
     * generous here is eight bytes per surface cell.
     */
    std::vector<uint64_t> cells;
    cells.reserve(1 << 14);
    {
        const int slabCount = nzc;
        std::vector<std::vector<uint64_t>> perSlab(static_cast<size_t>(slabCount));
        parallelFor(slabCount, threads, [&](int z0, int z1) {
            for (int z = z0; z < z1; ++z) {
                std::vector<uint64_t>& out = perSlab[static_cast<size_t>(z)];
                for (int y = 0; y < nyc; ++y) {
                    for (int x = 0; x < nxc; ++x) {
                        int neg = 0;
                        for (int c = 0; c < 8; ++c) {
                            if (grid.at(x + kCornerDx[c], y + kCornerDy[c], z + kCornerDz[c]) < 0.0f) ++neg;
                        }
                        if (neg == 0 || neg == 8) continue;
                        out.push_back(static_cast<uint64_t>(x) |
                                      (static_cast<uint64_t>(y) << 21) |
                                      (static_cast<uint64_t>(z) << 42));
                    }
                }
            }
        });
        for (int z = 0; z < slabCount; ++z) {
            cells.insert(cells.end(), perSlab[static_cast<size_t>(z)].begin(), perSlab[static_cast<size_t>(z)].end());
        }
    }

#ifdef AF_ISO_DEBUG
    std::fprintf(stderr, "[iso] surface cells: %zu\n", cells.size());
#endif
    /* ---- pass 2: emit ---------------------------------------------------------------- */
    std::unordered_map<uint64_t, uint32_t> vertexTable;
    vertexTable.reserve(cells.size() * 3);

    auto vertexOnEdge = [&](size_t ia, size_t ib, const Vec3& pa, const Vec3& pb, float va, float vb) -> uint32_t {
        const uint64_t key = edgeKeyOf(ia, ib);
        const auto it = vertexTable.find(key);
        if (it != vertexTable.end()) return it->second;
        const float denom = vb - va;
        /* The parameter is kept strictly inside the edge. Without this, a grid corner whose
           sample is exactly zero produces crossing points that coincide with the corner, and
           every tetrahedron around that corner then emits a zero-area triangle - which is
           dropped, leaving a hole. Clamping by 0.02% of a cell moves the vertex by a fraction
           of a millimetre and removes the entire failure mode. */
        const float t = (std::fabs(denom) < 1e-12f) ? 0.5f : clampf(-va / denom, 2.0e-4f, 1.0f - 2.0e-4f);
        const Vec3 pos = pa + (pb - pa) * t;
        const uint32_t id = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(pos);
        mesh.regions.push_back(regionFn ? regionFn(pos) : 0);
        /* Analytic normal from the grid gradient: no faceting at all, and no dependence on the
           triangle soup's averaging order. */
        mesh.normals.push_back(grid.gradient(pos, grid.cell * 0.5f).normalizedOr({0, 1, 0}));
        vertexTable.emplace(key, id);
        return id;
    };

#ifdef AF_ISO_DEBUG
    static int s_calls = 0, s_degen = 0, s_dup = 0;
#endif
    auto pushTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
#ifdef AF_ISO_DEBUG
        ++s_calls;
#endif
        if (a == b || b == c || a == c) {
#ifdef AF_ISO_DEBUG
            ++s_dup;
#endif
            return;
        }
        const Vec3& pa = mesh.positions[a];
        const Vec3& pb = mesh.positions[b];
        const Vec3& pc = mesh.positions[c];
        const Vec3 n = (pb - pa).cross(pc - pa);
        /* A zero-area triangle is KEPT. It contributes nothing to the rendered surface, but it
           still closes the shell topologically, and dropping it would leave its three edges with
           a single incident triangle each - i.e. it would punch a 3-edge hole in an otherwise
           watertight mesh. Only the truly impossible case (two equal indices) is rejected. */
        const float area2 = n.length();
        (void)area2;
        const Vec3 centroid = (pa + pb + pc) / 3.0f;

        const Vec3 g2 = grid.gradient(centroid, grid.cell * 0.5f);
        if (n.lengthSq() > 1e-20f && n.dot(g2) < 0.0f) {
            mesh.indices.push_back(a); mesh.indices.push_back(c); mesh.indices.push_back(b);
        } else {
            mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
        }
    };

    const size_t cellCount = cells.size();
    for (size_t ci = 0; ci < cellCount; ++ci) {
        if (cancel && (ci & 8191u) == 0u && cancel()) break;
        const uint64_t packed = cells[ci];
        const int x = static_cast<int>(packed & 0x1FFFFFull);
        const int y = static_cast<int>((packed >> 21) & 0x1FFFFFull);
        const int z = static_cast<int>((packed >> 42) & 0x1FFFFFull);

        Vec3 p[8];
        size_t gidx[8];
        float v[8];
        for (int c = 0; c < 8; ++c) {
            const int cx = x + kCornerDx[c], cy = y + kCornerDy[c], cz = z + kCornerDz[c];
            gidx[c] = grid.index(cx, cy, cz);
            p[c] = grid.position(cx, cy, cz);
            v[c] = grid.values[gidx[c]];
        }

        for (int t = 0; t < 6; ++t) {
            const int* tet = kTets[t];
            int negMask = 0, negCount = 0;
            for (int k = 0; k < 4; ++k) {
                if (v[tet[k]] < 0.0f) { negMask |= (1 << k); ++negCount; }
            }
            if (negCount == 0 || negCount == 4) continue;

            /* Crossing vertices on the tetrahedron's six edges, created on demand. */
            uint32_t vid[6] = {0, 0, 0, 0, 0, 0};
            bool has[6] = {false, false, false, false, false, false};
            for (int e = 0; e < 6; ++e) {
                const int a = tet[kTetEdges[e][0]], b = tet[kTetEdges[e][1]];
                if ((v[a] < 0.0f) == (v[b] < 0.0f)) continue;
                has[e] = true;
                vid[e] = vertexOnEdge(gidx[a], gidx[b], p[a], p[b], v[a], v[b]);
            }

            /* Edge index of the edge joining two LOCAL tet vertices (0..3). The mapping through
               `tet` is essential: tet[] holds cube-corner numbers, not local positions. */
            auto edgeBetween = [&](int localA, int localB) {
                const int ca = tet[localA], cb = tet[localB];
                for (int e = 0; e < 6; ++e) {
                    const int ea = tet[kTetEdges[e][0]], eb = tet[kTetEdges[e][1]];
                    if ((ea == ca && eb == cb) || (ea == cb && eb == ca)) return e;
                }
                return -1;
            };

            if (negCount == 1 || negCount == 3) {
                /* One corner on its own: a single triangle on that corner's three edges. */
                const int lone = (negCount == 1) ? (negMask == 1 ? 0 : (negMask == 2 ? 1 : (negMask == 4 ? 2 : 3)))
                                                 : (negMask == 0xE ? 0 : (negMask == 0xD ? 1 : (negMask == 0xB ? 2 : 3)));
                int e[3];
                int n = 0;
                for (int k = 1; k <= 3; ++k) {
                    const int idx = edgeBetween(lone, (lone + k) % 4);
                    if (idx >= 0 && has[idx]) e[n++] = idx;
                }
                if (n == 3) pushTriangle(vid[e[0]], vid[e[1]], vid[e[2]]);
            } else {
                /* Two corners each: the four crossings form a cyclic quad. Emitting the two
                   triangles across the n0-p1 diagonal is the only choice that yields a manifold
                   surface; the other diagonal creates a bow-tie (two triangles meeting at a
                   single vertex), which is precisely the non-manifold defect this renderer's
                   normal generation cannot tolerate. */
                int ins[2], outs[2], ni = 0, no = 0;
                for (int k = 0; k < 4; ++k) {
                    if (v[tet[k]] < 0.0f) ins[ni++] = k; else outs[no++] = k;
                }
                if (ni != 2 || no != 2) continue;
                const int e00 = edgeBetween(ins[0], outs[0]);
                const int e10 = edgeBetween(ins[1], outs[0]);
                const int e11 = edgeBetween(ins[1], outs[1]);
                const int e01 = edgeBetween(ins[0], outs[1]);
                if (e00 < 0 || e10 < 0 || e11 < 0 || e01 < 0) continue;
                if (!has[e00] || !has[e10] || !has[e11] || !has[e01]) continue;
                /* Cyclic order: (n0,p0) -> (n1,p0) -> (n1,p1) -> (n0,p1). */
                const uint32_t q0 = vid[e00], q1 = vid[e10], q2 = vid[e11], q3 = vid[e01];
                pushTriangle(q0, q1, q2);
                pushTriangle(q0, q2, q3);
            }
        }
    }

#ifdef AF_ISO_DEBUG
    std::fprintf(stderr, "[iso] emitted verts=%zu tris=%zu | pushTriangle calls=%d dup=%d degen=%d\n",
                 mesh.positions.size(), mesh.indices.size()/3, s_calls, s_dup, s_degen);
    s_calls = s_degen = s_dup = 0;
#endif
    /* ---- pass 3: drop unused vertices (a vertex can be created and then lost when a triangle
       is rejected as degenerate) ------------------------------------------------------- */
    if (!mesh.positions.empty()) {
        std::vector<int32_t> remap(mesh.positions.size(), -1);
        std::vector<Vec3> np; std::vector<Vec3> nn; std::vector<int32_t> nr;
        for (uint32_t& idx : mesh.indices) {
            if (remap[idx] < 0) {
                remap[idx] = static_cast<int32_t>(np.size());
                np.push_back(mesh.positions[idx]);
                nn.push_back(mesh.normals[idx]);
                nr.push_back(mesh.regions[idx]);
            }
            idx = static_cast<uint32_t>(remap[idx]);
        }
        mesh.positions.swap(np);
        mesh.normals.swap(nn);
        mesh.regions.swap(nr);
    }
    return mesh;
}

/* ================================================================== boundary loops ========== */
std::vector<std::vector<uint32_t>> findBoundaryLoops(const Mesh& mesh) {
    std::unordered_map<uint64_t, int> dir;
    dir.reserve(mesh.indices.size());
    auto key = [](uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; };
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t i[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
        for (int e = 0; e < 3; ++e) dir[key(i[e], i[(e + 1) % 3])]++;
    }
    /* Boundary edges are the directed edges without an opposite twin. A vertex can be the tail
       of several boundary edges when the surface pinches, so the walk below consumes edges
       rather than vertices and therefore cannot loop forever or drop ring segments. */
    std::unordered_multimap<uint32_t, uint32_t> outgoing;
    for (const auto& kv : dir) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
        if (kv.second != 1) continue;
        if (dir.find(key(b, a)) != dir.end()) continue;   /* has a twin: not a boundary edge */
        outgoing.emplace(a, b);
    }

    std::unordered_set<uint64_t> used;
    std::vector<std::vector<uint32_t>> loops;
    for (const auto& start : outgoing) {
        const uint64_t startKey = key(start.first, start.second);
        if (used.count(startKey)) continue;
        std::vector<uint32_t> loop;
        uint32_t cur = start.first;
        uint32_t nextV = start.second;
        for (size_t guard = 0; guard < outgoing.size() + 4; ++guard) {
            const uint64_t k = key(cur, nextV);
            if (used.count(k)) break;
            used.insert(k);
            loop.push_back(cur);
            const uint32_t from = nextV;
            uint32_t chosen = 0xFFFFFFFFu;
            const auto range = outgoing.equal_range(from);
            for (auto it = range.first; it != range.second; ++it) {
                if (!used.count(key(from, it->second))) { chosen = it->second; break; }
            }
            if (chosen == 0xFFFFFFFFu) { loop.push_back(from); break; }
            cur = from;
            nextV = chosen;
            if (cur == start.first && nextV == start.second) break;
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    std::sort(loops.begin(), loops.end(), [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
        return a.size() > b.size();
    });
    return loops;
}

void describeBoundaryLoops(const Mesh& mesh, const std::function<void(int, const Vec3&)>& fn) {
    const std::vector<std::vector<uint32_t>> loops = findBoundaryLoops(mesh);
    for (const std::vector<uint32_t>& loop : loops) {
        Vec3 c(0, 0, 0);
        for (uint32_t v : loop) c += mesh.positions[v];
        c /= static_cast<float>(loop.size());
        fn(static_cast<int>(loop.size()), c);
    }
}

/* ================================================================== stitching =============== */
namespace {

void sortRingByAngle(const Mesh& mesh, std::vector<uint32_t>* ring, int axis, const Vec3& axisPoint) {
    const int a0 = (axis + 1) % 3, a1 = (axis + 2) % 3;
    auto coord = [&](const Vec3& p, int i) { return (i == 0) ? p.x : ((i == 1) ? p.y : p.z); };
    std::sort(ring->begin(), ring->end(), [&](uint32_t u, uint32_t w) {
        const Vec3 pu = mesh.positions[u] - axisPoint;
        const Vec3 pw = mesh.positions[w] - axisPoint;
        return std::atan2(coord(pu, a1), coord(pu, a0)) < std::atan2(coord(pw, a1), coord(pw, a0));
    });
}

float ringRadius(const Mesh& mesh, const std::vector<uint32_t>& ring, int axis, const Vec3& c) {
    float sum = 0.0f;
    for (uint32_t v : ring) {
        const Vec3 d = mesh.positions[v] - c;
        sum += (axis == 0) ? std::sqrt(d.y * d.y + d.z * d.z)
                           : ((axis == 1) ? std::sqrt(d.x * d.x + d.z * d.z) : std::sqrt(d.x * d.x + d.y * d.y));
    }
    return ring.empty() ? 0.0f : sum / static_cast<float>(ring.size());
}

} // namespace

int stitchCutPlanes(Mesh& mesh, const std::vector<CutPlane>& planes, float tolerance) {
    int stitches = 0;
    for (const CutPlane& plane : planes) {
        const std::vector<std::vector<uint32_t>> loops = findBoundaryLoops(mesh);
        std::vector<std::vector<uint32_t>> onPlane;
        std::vector<Vec3> centres;
        for (const std::vector<uint32_t>& loop : loops) {
            Vec3 c(0, 0, 0);
            for (uint32_t v : loop) c += mesh.positions[v];
            c /= static_cast<float>(loop.size());
            const float coord = (plane.axis == 0) ? c.x : ((plane.axis == 1) ? c.y : c.z);
            if (std::fabs(coord - plane.value) > tolerance) continue;
            bool flat = true;
            for (uint32_t v : loop) {
                const Vec3& p = mesh.positions[v];
                const float pc = (plane.axis == 0) ? p.x : ((plane.axis == 1) ? p.y : p.z);
                if (std::fabs(pc - plane.value) > tolerance) { flat = false; break; }
            }
            if (!flat) continue;
            onPlane.push_back(loop);
            centres.push_back(c);
        }
#ifdef AF_ISO_DEBUG
        std::fprintf(stderr, "[stitch] axis=%d value=%.4f loops=%zu onPlane=%zu\n",
                     plane.axis, plane.value, loops.size(), onPlane.size());
        for (size_t q = 0; q < loops.size(); ++q) {
            Vec3 cc(0, 0, 0);
            for (uint32_t v : loops[q]) cc += mesh.positions[v];
            cc /= static_cast<float>(loops[q].size());
            float maxDev = 0.0f;
            for (uint32_t v : loops[q]) {
                const Vec3& p = mesh.positions[v];
                const float pc = (plane.axis == 0) ? p.x : ((plane.axis == 1) ? p.y : p.z);
                maxDev = maxf(maxDev, std::fabs(pc - plane.value));
            }
            std::fprintf(stderr, "    loop %zu size=%zu centre=(%.4f %.4f %.4f) maxDev=%.4f radius=%.4f\n",
                         q, loops[q].size(), cc.x, cc.y, cc.z, maxDev,
                         ringRadius(mesh, loops[q], plane.axis, cc));
        }
#endif
        if (onPlane.size() < 2) continue;

        /* Choose the seam pair. Criteria, in order:
             1. the two rims must have comparable radii (0.4x .. 2.5x): a torso rim and a wrist
                rim are never the same seam;
             2. their centres must nearly coincide relative to their size;
             3. among the pairs that pass, take the one with the largest rims - a real seam
                carries the biggest boundary rings, and taking the first match in index order
                would happily zip two 4-vertex slivers left over from dropped degenerate
                triangles, which is worse than leaving the big rims unstitched.
           `onPlane` is small (a handful of rims), so the quadratic scan is free. */
        std::vector<float> radii(onPlane.size(), 0.0f);
        for (size_t a = 0; a < onPlane.size(); ++a) {
            radii[a] = ringRadius(mesh, onPlane[a], plane.axis, centres[a]);
        }
        size_t bestA = 0, bestB = 0;
        float bestScore = -1.0f;
        bool havePair = false;
        for (size_t a = 0; a < onPlane.size(); ++a) {
            if (radii[a] < 1e-4f) continue;
            for (size_t b = a + 1; b < onPlane.size(); ++b) {
                if (radii[b] < 1e-4f) continue;
                const float ratio = radii[a] > radii[b] ? radii[a] / radii[b] : radii[b] / radii[a];
                if (ratio > 2.5f || ratio < 0.4f) continue;
                const float d = (centres[a] - centres[b]).length();
                const float avgR = (radii[a] + radii[b]) * 0.5f;
                if (d > 0.6f * avgR + 0.008f) continue;
                const float score = radii[a] + radii[b];
                if (score > bestScore) {
                    bestScore = score;
                    bestA = a;
                    bestB = b;
                    havePair = true;
                }
            }
        }
        if (!havePair) continue;
#ifdef AF_ISO_DEBUG
        /* Only the debug build uses these; declaring them unconditionally made the release build
           warn about variables that exist for a print that is not compiled in. */
        const float radiusA = radii[bestA], radiusB = radii[bestB];
        std::fprintf(stderr, "    chosen pair %zu/%zu rA=%.4f rB=%.4f d=%.4f\n",
                     bestA, bestB, radiusA, radiusB, (centres[bestA] - centres[bestB]).length());
#endif

        std::vector<uint32_t> ringA = onPlane[bestA];
        std::vector<uint32_t> ringB = onPlane[bestB];
        /* Both rims are ordered by angle about their own centre, so vertex i of one faces the
           same direction as vertex i of the other. The second ring is walked backwards so the
           strip's triangles end up wound consistently with the two shells it joins. */
        sortRingByAngle(mesh, &ringA, plane.axis, centres[bestA]);
        sortRingByAngle(mesh, &ringB, plane.axis, centres[bestB]);
        std::reverse(ringB.begin(), ringB.end());

        const size_t na = ringA.size(), nb = ringB.size();
        if (na < 3 || nb < 3) continue;

        /* Normalised arc-length parametrisations: they let the strip advance along whichever
           rim is "behind", so both rims are consumed exactly once and every A-edge and B-edge
           is used by exactly two triangles. That is what makes the seam manifold even when the
           two rims have different vertex counts (the torso samples at 11.5 mm, the hand at
           3.6 mm, so their counts differ by more than 3x). */
        auto cumulative = [&](const std::vector<uint32_t>& ring, const Vec3& c) {
            std::vector<float> arc(ring.size() + 1, 0.0f);
            float total = 0.0f;
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec3 a = mesh.positions[ring[i]] - c;
                const Vec3 b = mesh.positions[ring[(i + 1) % ring.size()]] - c;
                total += (b - a).length();
                arc[i + 1] = total;
            }
            if (total > 1e-9f) for (float& v : arc) v /= total;
            return arc;
        };
        const std::vector<float> arcA = cumulative(ringA, centres[bestA]);
        const std::vector<float> arcB = cumulative(ringB, centres[bestB]);

        const size_t before = mesh.indices.size();
        size_t i = 0, j = 0;
        size_t guard = 0;
        while ((i < na || j < nb) && guard++ < (na + nb) * 2 + 8) {
            const size_t ia = i % na, ia1 = (i + 1) % na;
            const size_t ib = j % nb, ib1 = (j + 1) % nb;
            /* Normalised position of the vertex each rim would advance to next. */
            const float ua = (i < na) ? arcA[i + 1] : 2.0f;
            const float ub = (j < nb) ? arcB[j + 1] : 2.0f;
            if (i < na && (ua <= ub || j >= nb)) {
                /* Triangle anchored on the next edge of rim A. */
                const uint32_t a0 = ringA[ia], a1 = ringA[ia1], b0 = ringB[ib];
                if (a0 != a1 && a0 != b0 && a1 != b0) {
                    mesh.indices.push_back(a0); mesh.indices.push_back(b0); mesh.indices.push_back(a1);
                }
                ++i;
            } else {
                /* Triangle anchored on the next edge of rim B. */
                const uint32_t b0 = ringB[ib], b1 = ringB[ib1], a0 = ringA[ia];
                if (b0 != b1 && b0 != a0 && b1 != a0) {
                    mesh.indices.push_back(b1); mesh.indices.push_back(a0); mesh.indices.push_back(b0);
                }
                ++j;
            }
        }

        /* Orientation check: the strip's faces must point away from the seam axis. If the
           majority points inward, the whole strip is reversed - a single decision for the seam
           keeps it consistent with itself. */
        const Vec3 seamCentre = (centres[bestA] + centres[bestB]) * 0.5f;
        float outward = 0.0f;
        for (size_t t = before; t + 2 < mesh.indices.size(); t += 3) {
            const Vec3 p0 = mesh.positions[mesh.indices[t]];
            const Vec3 p1 = mesh.positions[mesh.indices[t + 1]];
            const Vec3 p2 = mesh.positions[mesh.indices[t + 2]];
            const Vec3 n = (p1 - p0).cross(p2 - p0);
            const Vec3 c = (p0 + p1 + p2) / 3.0f - seamCentre;
            outward += n.dot(c);
        }
        if (outward < 0.0f) {
            for (size_t t = before; t + 2 < mesh.indices.size(); t += 3) {
                std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
            }
        }
        ++stitches;
    }
    return stitches;
}

/* ================================================================== hole filling ============ */
int fillHoles(Mesh& mesh, int maxLoopVertices) {
    const std::vector<std::vector<uint32_t>> loops = findBoundaryLoops(mesh);
    int filled = 0;
    for (const std::vector<uint32_t>& loop : loops) {
        if (static_cast<int>(loop.size()) > maxLoopVertices) continue;
        const size_t n = loop.size();
        if (n < 3) continue;
        Vec3 c(0, 0, 0);
        for (uint32_t v : loop) c += mesh.positions[v];
        c /= static_cast<float>(n);

        /* Orientation: the patch must close the shell, so it has to wind the same way as the
           triangles that already use these edges. Tried both ways and keep the one whose normal
           agrees with the average vertex normal of the rim. */
        Vec3 rimNormal(0, 0, 0);
        for (uint32_t v : loop) rimNormal += (v < mesh.normals.size()) ? mesh.normals[v] : Vec3(0, 1, 0);
        rimNormal = rimNormal.normalizedOr({0, 1, 0});
        const Vec3 a = mesh.positions[loop[0]] - c;
        Vec3 faceNormal(0, 0, 0);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 p0 = mesh.positions[loop[i]] - c;
            const Vec3 p1 = mesh.positions[loop[(i + 1) % n]] - c;
            faceNormal += p0.cross(p1);
        }
        faceNormal = faceNormal.normalizedOr(rimNormal);
        const bool flip = faceNormal.dot(rimNormal) < 0.0f;

        /* A single centroid vertex keeps the patch a proper fan and adds exactly one vertex,
           which is far cheaper than re-triangulating the rim into a strip. */
        const uint32_t centreId = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(c);
        mesh.normals.push_back(rimNormal);
        mesh.regions.push_back(mesh.regions.empty() ? 0 : mesh.regions[loop[0]]);
        mesh.curvature.push_back(mesh.curvature.empty() ? 1.0f : mesh.curvature[loop[0]]);
        if (mesh.uvs.size() >= mesh.positions.size() * 2) {
            mesh.uvs.push_back(mesh.uvs.size() > loop[0] * 2 + 1 ? mesh.uvs[loop[0] * 2] : 0.0f);
            mesh.uvs.push_back(mesh.uvs.size() > loop[0] * 2 + 1 ? mesh.uvs[loop[0] * 2 + 1] : 0.0f);
        } else {
            mesh.uvs.push_back(0.0f);
            mesh.uvs.push_back(0.0f);
        }
        if (!mesh.tangents.empty()) mesh.tangents.push_back(Vec4(1, 0, 0, 1));

        for (size_t i = 0; i < n; ++i) {
            const uint32_t v0 = loop[i];
            const uint32_t v1 = loop[(i + 1) % n];
            if (v0 == v1) continue;
            if (flip) {
                mesh.indices.push_back(centreId); mesh.indices.push_back(v1); mesh.indices.push_back(v0);
            } else {
                mesh.indices.push_back(centreId); mesh.indices.push_back(v0); mesh.indices.push_back(v1);
            }
        }
        ++filled;
    }
    return filled;
}

/* ================================================================== clipping ================ */
float distanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float den = ab.lengthSq();
    const float t = den < 1e-12f ? 0.0f : clampf((p - a).dot(ab) / den, 0.0f, 1.0f);
    return (p - (a + ab * t)).length();
}

ClipStats clipMeshByHalfSpace(Mesh& mesh, int axis, float value, bool keepBelow,
                              const std::function<bool(const Vec3&)>& inRegion,
                              const std::function<Vec3(const Vec3&)>& normalFn) {
    ClipStats stats;
    if (mesh.indices.size() < 3) return stats;
    const bool hasUv = mesh.uvs.size() >= mesh.positions.size() * 2;
    const bool hasTangent = mesh.tangents.size() == mesh.positions.size();

    auto coord = [&](const Vec3& p) { return (axis == 0) ? p.x : ((axis == 1) ? p.y : p.z); };
    auto insideOf = [&](const Vec3& p) {
        const float c = coord(p) - value;
        return keepBelow ? (c <= 0.0f) : (c >= 0.0f);
    };

    /* Crossing vertices are deduplicated on their quantised position: without this, the ring
       left behind by the clip has one vertex per incident triangle and cannot be stitched. */
    std::unordered_map<uint64_t, uint32_t> crossTable;
    auto crossingVertex = [&](uint32_t ia, uint32_t ib) -> uint32_t {
        const Vec3 a = mesh.positions[ia];
        const Vec3 b = mesh.positions[ib];
        const float ca = coord(a), cb = coord(b);
        const float den = cb - ca;
        float t = (std::fabs(den) < 1e-12f) ? 0.5f : ((value - ca) / den);
        t = clampf(t, 0.0f, 1.0f);
        Vec3 p = a + (b - a) * t;
        /* Snap the clipped axis to exactly the plane value, so the ring is perfectly planar. */
        if (axis == 0) p.x = value; else if (axis == 1) p.y = value; else p.z = value;
        const int64_t qx = static_cast<int64_t>(std::llround(p.x * 1.0e6));
        const int64_t qy = static_cast<int64_t>(std::llround(p.y * 1.0e6));
        const int64_t qz = static_cast<int64_t>(std::llround(p.z * 1.0e6));
        const uint64_t key = (static_cast<uint64_t>(qx & 0x1FFFFF) << 42) ^
                             (static_cast<uint64_t>(qy & 0x1FFFFF) << 21) ^
                             static_cast<uint64_t>(qz & 0x1FFFFF);
        const auto it = crossTable.find(key);
        if (it != crossTable.end()) return it->second;

        const uint32_t id = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(p);
        mesh.normals.push_back(normalFn ? normalFn(p).normalizedOr(Vec3(0, 1, 0))
                                        : (mesh.normals.empty() ? Vec3(0, 1, 0)
                                                                : (mesh.normals[ia] + mesh.normals[ib]).normalizedOr(Vec3(0, 1, 0))));
        mesh.regions.push_back(mesh.regions.empty() ? 0 : mesh.regions[t > 0.5f ? ib : ia]);
        mesh.curvature.push_back(mesh.curvature.empty() ? 1.0f : mesh.curvature[ia] * (1.0f - t) + mesh.curvature[ib] * t);
        if (hasUv) {
            mesh.uvs.push_back(mesh.uvs[ia * 2] * (1.0f - t) + mesh.uvs[ib * 2] * t);
            mesh.uvs.push_back(mesh.uvs[ia * 2 + 1] * (1.0f - t) + mesh.uvs[ib * 2 + 1] * t);
        } else {
            mesh.uvs.push_back(0.0f); mesh.uvs.push_back(0.0f);
        }
        if (hasTangent) {
            const Vec4 ta = mesh.tangents[ia], tb = mesh.tangents[ib];
            mesh.tangents.push_back(Vec4((ta.xyz() * (1.0f - t) + tb.xyz() * t).normalizedOr(Vec3(1, 0, 0)), ta.w));
        } else {
            mesh.tangents.push_back(Vec4(1, 0, 0, 1));
        }
        crossTable.emplace(key, id);
        ++stats.verticesCreated;
        return id;
    };

    std::vector<uint32_t> out;
    out.reserve(mesh.indices.size());
    uint32_t poly[8];
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t idx[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
        const Vec3 c = (mesh.positions[idx[0]] + mesh.positions[idx[1]] + mesh.positions[idx[2]]) / 3.0f;
        if (inRegion && !inRegion(c)) {              /* not our limb: leave it alone */
            out.push_back(idx[0]); out.push_back(idx[1]); out.push_back(idx[2]);
            continue;
        }
        bool in[3];
        for (int k = 0; k < 3; ++k) in[k] = insideOf(mesh.positions[idx[k]]);
        if (in[0] && in[1] && in[2]) {
            out.push_back(idx[0]); out.push_back(idx[1]); out.push_back(idx[2]);
            continue;
        }
        if (!in[0] && !in[1] && !in[2]) {
            ++stats.trianglesRemoved;
            continue;
        }
        ++stats.trianglesSplit;
        /* Sutherland-Hodgman against the half-space: the result is a polygon with 3 or 4
           vertices, kept in the triangle's winding order. */
        int n = 0;
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = idx[k], b = idx[(k + 1) % 3];
            const bool ia = in[k], ib2 = in[(k + 1) % 3];
            if (ia) poly[n++] = a;
            if (ia != ib2) poly[n++] = crossingVertex(a, b);
            if (n > 6) break;
        }
        for (int k = 2; k < n; ++k) {
            out.push_back(poly[0]); out.push_back(poly[k - 1]); out.push_back(poly[k]);
        }
    }
    mesh.indices.swap(out);
    return stats;
}

} // namespace af
