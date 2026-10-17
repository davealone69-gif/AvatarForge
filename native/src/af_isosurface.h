/*
 * af_isosurface.h -- grid sampling, marching-tetrahedra extraction and cut stitching.
 *
 * Coordinate conventions: X = right (+X is the model's LEFT side), Y = up, Z = forward is -Z.
 * The isosurface is the zero level set of the body field; positive = outside the body.
 */
#ifndef AF_ISOSURFACE_H
#define AF_ISOSURFACE_H

#include "af_mesh.h"
#include "af_threads.h"
#include <functional>
#include <vector>

namespace af {

/* ------------------------------------------------------------------ grid -------------------- */

struct DenseGrid {
    int nx = 0, ny = 0, nz = 0;
    float cell = 0.01f;
    Vec3 origin{0, 0, 0};              /* world position of sample (0,0,0)          */
    std::vector<float> values;

    void allocate(int nX, int nY, int nZ, float c, const Vec3& o) {
        nx = nX; ny = nY; nz = nZ; cell = c; origin = o;
        values.assign(static_cast<size_t>(nx) * ny * nz, 1.0f);
    }
    size_t index(int x, int y, int z) const {
        return static_cast<size_t>(x) + static_cast<size_t>(nx) * (static_cast<size_t>(y) + static_cast<size_t>(ny) * z);
    }
    Vec3 position(int x, int y, int z) const {
        return Vec3(origin.x + x * cell, origin.y + y * cell, origin.z + z * cell);
    }
    float at(int x, int y, int z) const { return values[index(x, y, z)]; }
    bool contains(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }
    /** Trilinear sample; used for normals and for the region classifier. */
    float sample(const Vec3& p) const;
    Vec3 gradient(const Vec3& p, float h) const;
    uint64_t bytes() const { return values.size() * sizeof(float); }
};

/**
 * A local cut: samples inside `boxMin..boxMax` that lie on the wrong side of `planeValue`
 * (along `axis`) are forced positive, so the extracted isosurface ends exactly on the plane.
 *
 * This is how a limb is separated from the torso: without it, the torso's surface would not be
 * clipped where the arm leaves the grid, and the resulting boundary ring could not be stitched
 * to the arm's ring.
 */
struct LocalCut {
    /* A cut region: either an axis-aligned box or a sphere. The shape limits WHERE the cut
       applies; the half-space decides WHICH SIDE is removed. */
    enum Shape { BOX, SPHERE };
    Shape shape = BOX;
    int   axis = 1;
    float planeValue = 0.0f;
    bool  keepBelow = true;            /* true: keep samples below the plane                */
    Vec3  boxMin{0, 0, 0};
    Vec3  boxMax{0, 0, 0};
    Vec3  centre{0, 0, 0};
    float radius = 0.05f;

    static LocalCut above(int ax, float value, const Vec3& mn, const Vec3& mx) {
        LocalCut c; c.shape = BOX; c.axis = ax; c.planeValue = value; c.keepBelow = true;
        c.boxMin = mn; c.boxMax = mx; return c;
    }
    static LocalCut below(int ax, float value, const Vec3& mn, const Vec3& mx) {
        LocalCut c; c.shape = BOX; c.axis = ax; c.planeValue = value; c.keepBelow = false;
        c.boxMin = mn; c.boxMax = mx; return c;
    }
    /**
     * Removes the part of a domain that lies ABOVE a cut plane inside a small sphere centred on
     * the limb axis. Used to separate a limb from the torso: the sphere is local enough that it
     * can never reach the hip or the thigh, which a footprint-shaped box would clip on a heavy
     * build, yet it is wide enough (1.8x the limb radius) to contain the whole limb section.
     */
    static LocalCut inSphereAbove(int ax, float value, const Vec3& c, float r) {
        LocalCut cut; cut.shape = SPHERE; cut.axis = ax; cut.planeValue = value;
        cut.keepBelow = true; cut.centre = c; cut.radius = r; return cut;
    }
    bool contains(const Vec3& p) const {
        if (shape == SPHERE) return (p - centre).lengthSq() <= radius * radius;
        return p.x >= boxMin.x && p.x <= boxMax.x && p.y >= boxMin.y && p.y <= boxMax.y &&
               p.z >= boxMin.z && p.z <= boxMax.z;
    }
};

/* ------------------------------------------------------------------ sampling ---------------- */

/**
 * Samples `field` into `grid`. `blockSize` enables empty-block skipping: blocks whose corner
 * samples are all further from the surface than the block diagonal cannot contain the
 * isosurface and are left at their "outside" fill value. Set blockSize = 1 to disable.
 */
void sampleField(DenseGrid& grid, const std::function<float(const Vec3&)>& field, int threads,
                 int blockSize = 8, const std::function<bool()>& cancel = nullptr,
                 const std::function<void(float)>& progress = nullptr,
                 const std::vector<LocalCut>& cuts = {});

/* ------------------------------------------------------------------ extraction -------------- */

/**
 * Extracts the zero level set as a triangle mesh. Vertices are shared between the cubes and
 * tetrahedra that touch the same grid edge, so the result is a closed, welded mesh (no
 * duplicate vertices along shared edges) with per-vertex region classification.
 */
Mesh extractIsosurface(const DenseGrid& grid,
                       const std::function<int(const Vec3&)>& regionFn,
                       const std::function<float(const Vec3&)>& field,
                       int threads = 1, const std::function<bool()>& cancel = nullptr);

/* ------------------------------------------------------------------ stitching -------------- */

struct CutPlane {
    int   axis = 1;
    float value = 0.0f;
};

/** Ordered boundary loops (vertex index rings) of a mesh, longest first. */
std::vector<std::vector<uint32_t>> findBoundaryLoops(const Mesh& mesh);

/** Prints a summary of the mesh's boundary loops (size + centroid) to stdout. */
void describeBoundaryLoops(const Mesh& mesh, const std::function<void(int, const Vec3&)>& fn);

/**
 * Stitches matching boundary loops that lie on the given cut planes: for each plane, loops
 * whose centroid is within `tolerance` of the plane are paired by closest centroid distance and
 * zipped by angle around the plane axis. Vertices of the smaller ring are used as-is and the
 * larger ring's triangles are re-triangulated against them, so both sides of the seam share
 * exactly one vertex ring.
 *
 * Returns the number of stitches performed.
 */
int stitchCutPlanes(Mesh& mesh, const std::vector<CutPlane>& planes, float tolerance = 0.004f);

/** Fills small holes (< maxLoopVertices) with fans. Returns the number of holes filled. */
int fillHoles(Mesh& mesh, int maxLoopVertices = 64);

struct ClipStats {
    size_t trianglesRemoved = 0;
    size_t trianglesSplit = 0;
    size_t verticesCreated = 0;
};

/**
 * Clips a mesh against a half-space, optionally restricted to a region.
 *
 * This is how limbs are separated from the torso. A *field* cut cannot do it: forcing outside
 * values inside a bounded box or sphere leaves the boundary of that region inside the limb, and
 * the isosurface then closes around it, producing a hollow bubble instead of an open ring. A
 * mesh clip has no such failure mode - triangles are removed and split exactly at the plane, so
 * the result is a clean planar boundary ring every time.
 *
 * `inRegion` (optional) restricts the clip to triangles whose centroid satisfies it, so the
 * torso is never touched while the arm that passes through its bounding volume is cut.
 * `normalFn` (optional) supplies analytic normals for the vertices created on the plane; without
 * it the normals are interpolated from the clipped edge.
 */
ClipStats clipMeshByHalfSpace(Mesh& mesh, int axis, float value, bool keepBelow,
                              const std::function<bool(const Vec3&)>& inRegion = nullptr,
                              const std::function<Vec3(const Vec3&)>& normalFn = nullptr);

/** Distance from a point to a segment, exposed for the limb tubes used by the clipper. */
float distanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b);

} // namespace af
#endif /* AF_ISOSURFACE_H */
