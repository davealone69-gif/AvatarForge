/*
 * af_body.h -- body mesh assembly: the stage that turns the analytic field into one real,
 * watertight, region-tagged triangle mesh.
 *
 * Pipeline (each step exists because the previous one leaves a specific artifact):
 *   union field   ->  ONE lattice over the union of every part's bounds, sampled from
 *                     smin(body, head, hands, feet). A single grid is what makes the result a
 *                     single shell: the earlier per-domain build had to cut, weld and restitch
 *                     four surfaces along the wrist/ankle/neck planes, and every stitch was a
 *                     place where a rim spacing mismatch could open a hole.
 *   marching tets ->  one extraction of the whole body, canonical edge keys so shared edges
 *                     always produce the same vertex
 *   red-green     ->  detailRounds conforming subdivisions, marked per EDGE (marking whole
 *                     triangles leaves T-junctions with the neighbours that share the edge) and
 *                     only where the surface actually deviates from the field, plus the
 *                     always-fine face regions. Midpoints are Newton-projected back onto the
 *                     field so subdivision follows the surface, not the chord.
 *   smooth        ->  Taubin lambda/mu passes, then re-projection: smoothing moves vertices off
 *                     the isosurface and the reprojection puts them back without eating the
 *                     crease removal that was the point of the pass.
 *   floor drop    ->  translates so the soles rest exactly on y = 0
 *   normals       ->  analytic field gradients (area-weighted normals on a 3 mm grid facet are
 *                     visibly faceted on a 2.87 mm face)
 *
 * The function is deterministic: same BodyMetrics + options => byte-identical mesh.
 */
#ifndef AF_BODY_H
#define AF_BODY_H

#include "af_field.h"
#include "af_mesh.h"
#include "af_isosurface.h"
#include <functional>

namespace af {

struct BodyBuildOptions {
    /*
     * Base lattice for the first extraction.
     *
     * 8 mm, not the 11.5 mm an earlier revision used. The lattice has to resolve the smallest
     * feature that must exist in the COARSE mesh, because refinement can only sharpen geometry
     * that was already extracted - it cannot invent what the lattice never saw. At 11.5 mm the
     * dental arches (about 17 mm tall including gums) and the palpebral opening fell between
     * sample rows and produced twelve vertices of teeth in the entire avatar. 8 mm grid for a
     * 1.8 m body is ~340k samples, which is a fraction of a second on a phone and 2.7 MB.
     */
    float bodyCell = 0.008f;
    /* Red-green subdivision rounds in the detail regions. Two rounds take the 8 mm base lattice
       to 2 mm where the surface actually needs it (faces, hands, feet, feature regions). */
    int   detailRounds = 2;
    /* Taubin smoothing passes (0 disables). One pass removes lattice terracing; more softens
       real anatomy. */
    int   smoothPasses = 1;
    bool  computeTangents = true;
    /*
     * Translates the mesh so its lowest vertex sits on y = 0. OFF by default, and deliberately so:
     * the mesh is extracted from the body field and every consumer of that field - texture baking,
     * rig fitting, cloth collision, the renderer's AO - queries distances against the field, so a
     * rigid translation of the mesh breaks the contract "the mesh lies on the field's zero set"
     * (measured: 27% of vertices end up more than 0.5 mm inside the surface). The field already
     * puts the soles on the contact plane through the sole clamp in BodyField::footShape, so the
     * mesh needs no correction. Export paths that must hand out a grounded model (GLB) apply the
     * offset once, at export, where the coordinate frame is frozen anyway.
     */
    bool  dropToFloor = false;
    int   threads = 0;
    std::function<bool()> cancelled;
    std::function<void(int, float)> progress;
};

/*
 * Topology quality tiers. The engine never claims a better tier than the mesh audit supports:
 * pinch points - vertices where the surface touches itself exactly at a grid coincidence - are
 * a known marching-tetrahedra artifact and are reported as counts, not hidden. WATERIGHT is
 * only reported when the audit is actually clean.
 */
enum class TopologyQuality { DEGRADED = 0, REPAIRED = 1, WATERTIGHT = 2 };

struct BodyBuildReport {
    size_t trunkTris = 0, headTris = 0, handTris = 0, footTris = 0;
    int    flippedTriangles = 0;
    int    debrisFaces = 0;      /* faces removed as sub-millimetre extraction debris */   /* winding repairs (see Mesh::orientConsistently) */
    int    interiorNormalVertices = 0; /* vertices whose normal had to come from the faces */
    int    stitches = 0;
    int    clippedTriangles = 0;
    int    holesFilled = 0;
    size_t openEdgesBeforeRepair = 0;
    size_t openEdgesMidRepair = 0;
    size_t openEdgesAfterRepair = 0;
    size_t nonManifoldEdges = 0;
    double millis = 0.0;
    bool   watertight = false;
    TopologyQuality quality = TopologyQuality::DEGRADED;
};

/**
 * The union field that the body builder solves: smooth-min of body, head, both hands and both
 * feet, with the per-side structure described in BodyField::body(). Exposed because every stage
 * that needs to know how far a point is from the avatar surface - texture baking, cloth collision,
 * rig fitting, picking - must query the SAME surface the mesh was extracted from. A second,
 * slightly different union would put the texture a millimetre off the geometry.
 */
float bodyUnionField(const BodyField& field, const Vec3& p);

/** Builds the full-body rest-pose mesh. Returns false only on cancellation or empty field. */
bool buildBodyMesh(const BodyField& field, const BodyBuildOptions& options,
                   Mesh* outMesh, BodyBuildReport* outReport);

} // namespace af
#endif /* AF_BODY_H */
