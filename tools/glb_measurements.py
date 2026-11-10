#!/usr/bin/env python3
"""Read-only GLB 2.0 measurement extractor.

Reports node-transformed world-space mesh bounds and skin joint origins. It never assumes
GLB coordinates are metres. Supply --meters-per-unit only when scale is independently known.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
import struct
import sys
from typing import Any

MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
COMPONENTS = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
              5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
TYPE_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}


class GlbError(ValueError):
    pass


def read_glb(path: str) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    raw = open(path, "rb").read()
    if len(raw) < 20:
        raise GlbError("file is shorter than a GLB header plus JSON chunk")
    magic, version, declared = struct.unpack_from("<III", raw, 0)
    if magic != MAGIC:
        raise GlbError("bad GLB magic")
    if version != 2:
        raise GlbError(f"unsupported GLB version {version}; expected 2")
    if declared != len(raw):
        raise GlbError(f"declared GLB length {declared} != actual {len(raw)}")
    off, chunks = 12, []
    while off < len(raw):
        if off + 8 > len(raw):
            raise GlbError("truncated chunk header")
        length, kind = struct.unpack_from("<II", raw, off)
        start, end = off + 8, off + 8 + length
        if end > len(raw):
            raise GlbError("chunk extends beyond file")
        chunks.append((kind, raw[start:end]))
        off = end
    if not chunks or chunks[0][0] != JSON_CHUNK:
        raise GlbError("first chunk must be JSON")
    if len(chunks[0][1]) % 4:
        raise GlbError("JSON chunk length must be a multiple of four bytes")
    if any(len(payload) % 4 for kind, payload in chunks if kind == BIN_CHUNK):
        raise GlbError("BIN chunk length must be a multiple of four bytes")
    if sum(1 for kind, _ in chunks if kind == JSON_CHUNK) != 1:
        raise GlbError("GLB must contain exactly one JSON chunk")
    if sum(1 for kind, _ in chunks if kind == BIN_CHUNK) > 1:
        raise GlbError("multiple BIN chunks are unsupported")
    try:
        doc = json.loads(chunks[0][1].decode("utf-8").rstrip(" \t\r\n\0"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GlbError(f"invalid GLB JSON: {exc}") from exc
    if doc.get("asset", {}).get("version") != "2.0":
        raise GlbError("asset.version must be 2.0")
    blob = next((payload for kind, payload in chunks if kind == BIN_CHUNK), b"")
    buffers = doc.get("buffers", [])
    if buffers:
        if len(buffers) != 1 or "uri" in buffers[0]:
            raise GlbError("only one embedded GLB buffer is supported")
        declared_buffer_length = buffers[0].get("byteLength")
        if not isinstance(declared_buffer_length, int) or declared_buffer_length < 0:
            raise GlbError("embedded buffer has invalid byteLength")
        if declared_buffer_length > len(blob):
            raise GlbError("buffer byteLength exceeds BIN chunk")
        # Exclude GLB BIN chunk alignment padding from all subsequent accessor bounds checks.
        blob = blob[:declared_buffer_length]
    elif blob:
        raise GlbError("BIN chunk exists without a buffer declaration")
    return doc, blob, {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                       "glb_version": version, "json_chunk_bytes": len(chunks[0][1]),
                       "bin_chunk_bytes": len(blob)}


def accessor_values(doc: dict[str, Any], blob: bytes, accessor_index: int) -> list[tuple[float, ...]]:
    accessors = doc.get("accessors", [])
    if not isinstance(accessor_index, int) or not 0 <= accessor_index < len(accessors):
        raise GlbError(f"accessor index out of range: {accessor_index}")
    acc = accessors[accessor_index]
    ctype, atype, count = acc.get("componentType"), acc.get("type"), acc.get("count")
    if ctype not in COMPONENTS or atype not in TYPE_WIDTH or not isinstance(count, int) or count < 0:
        raise GlbError(f"accessor {accessor_index} has invalid type/count")
    fmt, component_size = COMPONENTS[ctype]
    width = TYPE_WIDTH[atype]
    elem_size = component_size * width
    values = [(0.0,) * width for _ in range(count)]
    views = doc.get("bufferViews", [])

    def decode_from_view(view_index: int, byte_offset: int, n: int, stride: int | None = None):
        if not 0 <= view_index < len(views):
            raise GlbError(f"accessor {accessor_index}: bufferView index out of range")
        view = views[view_index]
        if view.get("buffer", 0) != 0:
            raise GlbError("only embedded buffer 0 is supported")
        base = int(view.get("byteOffset", 0)) + byte_offset
        step = int(stride or elem_size)
        if step < elem_size:
            raise GlbError(f"accessor {accessor_index}: byteStride smaller than element")
        view_end = int(view.get("byteOffset", 0)) + int(view.get("byteLength", 0))
        if base < int(view.get("byteOffset", 0)) or (n and base + (n - 1) * step + elem_size > view_end):
            raise GlbError(f"accessor {accessor_index}: data exceeds bufferView")
        if n and base + (n - 1) * step + elem_size > len(blob):
            raise GlbError(f"accessor {accessor_index}: data exceeds BIN chunk")
        out = []
        for i in range(n):
            vals = struct.unpack_from("<" + fmt * width, blob, base + i * step)
            if acc.get("normalized") and ctype != 5126:
                if ctype in (5120, 5122):
                    maximum = 127.0 if ctype == 5120 else 32767.0
                    vals = tuple(max(-1.0, v / maximum) for v in vals)
                else:
                    maximum = {5121: 255.0, 5123: 65535.0, 5125: 4294967295.0}[ctype]
                    vals = tuple(v / maximum for v in vals)
            out.append(tuple(float(v) for v in vals))
        return out

    if "bufferView" in acc:
        vi = acc["bufferView"]
        view = views[vi]
        values = decode_from_view(vi, int(acc.get("byteOffset", 0)), count, view.get("byteStride"))
    sparse = acc.get("sparse")
    if sparse:
        n = sparse.get("count", 0)
        if not isinstance(n, int) or not 0 <= n <= count:
            raise GlbError(f"accessor {accessor_index}: invalid sparse count")
        ix = sparse["indices"]
        if ix.get("componentType") not in (5121, 5123, 5125):
            raise GlbError(f"accessor {accessor_index}: invalid sparse index type")
        if ix["bufferView"] >= len(views):
            raise GlbError("sparse index bufferView out of range")
        iv = views[ix["bufferView"]]
        if iv.get("buffer", 0) != 0:
            raise GlbError("sparse indices must use embedded buffer 0")
        ifmt, isize = COMPONENTS[ix["componentType"]]
        ip = int(iv.get("byteOffset", 0)) + int(ix.get("byteOffset", 0))
        if ip + n * isize > int(iv.get("byteOffset", 0)) + int(iv.get("byteLength", 0)):
            raise GlbError("sparse indices exceed bufferView")
        indices = [struct.unpack_from("<" + ifmt, blob, ip + i * isize)[0] for i in range(n)]
        if indices != sorted(set(indices)) or any(i >= count for i in indices):
            raise GlbError("sparse indices must be unique, increasing and in range")
        vx = sparse["values"]
        sparse_values = decode_from_view(vx["bufferView"], int(vx.get("byteOffset", 0)), n)
        for i, value in zip(indices, sparse_values):
            values[i] = value
    if any(not all(math.isfinite(x) for x in row) for row in values):
        raise GlbError(f"accessor {accessor_index}: non-finite value")
    return values


def mat_mul(a: list[float], b: list[float]) -> list[float]:
    # glTF matrices are column-major.
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
            for c in range(4) for r in range(4)]


def node_local_matrix(node: dict[str, Any]) -> list[float]:
    if "matrix" in node:
        m = node["matrix"]
        if len(m) != 16 or not all(math.isfinite(float(v)) for v in m):
            raise GlbError("node matrix must contain 16 finite values")
        return [float(v) for v in m]
    t = [float(v) for v in node.get("translation", [0, 0, 0])]
    q = [float(v) for v in node.get("rotation", [0, 0, 0, 1])]
    s = [float(v) for v in node.get("scale", [1, 1, 1])]
    if len(t) != 3 or len(q) != 4 or len(s) != 3:
        raise GlbError("node TRS has invalid vector length")
    if not all(math.isfinite(v) for v in t + q + s):
        raise GlbError("node TRS contains non-finite values")
    qn = math.sqrt(sum(v * v for v in q))
    if qn < 1e-12:
        raise GlbError("node quaternion has zero length")
    x, y, z, w = [v / qn for v in q]
    sx, sy, sz = s
    # Rotation matrix multiplied by scale, column-major.
    return [
        (1-2*(y*y+z*z))*sx, (2*(x*y+z*w))*sx, (2*(x*z-y*w))*sx, 0,
        (2*(x*y-z*w))*sy, (1-2*(x*x+z*z))*sy, (2*(y*z+x*w))*sy, 0,
        (2*(x*z+y*w))*sz, (2*(y*z-x*w))*sz, (1-2*(x*x+y*y))*sz, 0,
        t[0], t[1], t[2], 1
    ]


def transform_point(m: list[float], p: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = p
    out = (m[0]*x + m[4]*y + m[8]*z + m[12],
           m[1]*x + m[5]*y + m[9]*z + m[13],
           m[2]*x + m[6]*y + m[10]*z + m[14])
    if not all(math.isfinite(v) for v in out):
        raise GlbError("world-space transform produced non-finite coordinate")
    return out


def hierarchy(doc: dict[str, Any]) -> tuple[list[list[float]], list[int | None]]:
    nodes = doc.get("nodes", [])
    parents: list[int | None] = [None] * len(nodes)
    for parent, node in enumerate(nodes):
        for child in node.get("children", []):
            if not isinstance(child, int) or not 0 <= child < len(nodes):
                raise GlbError(f"node {parent} has invalid child index")
            if parents[child] is not None:
                raise GlbError(f"node {child} has multiple parents")
            parents[child] = parent
    worlds: list[list[float] | None] = [None] * len(nodes)
    visiting: set[int] = set()

    def resolve(i: int) -> list[float]:
        if worlds[i] is not None:
            return worlds[i]  # type: ignore[return-value]
        if i in visiting:
            raise GlbError("cycle in node hierarchy")
        visiting.add(i)
        local = node_local_matrix(nodes[i])
        parent = parents[i]
        world = mat_mul(resolve(parent), local) if parent is not None else local
        visiting.remove(i)
        worlds[i] = world
        return world
    for i in range(len(nodes)):
        resolve(i)
    return [m for m in worlds if m is not None], parents


def bounds(points):
    if not points:
        return None
    return {"min": [min(p[i] for p in points) for i in range(3)],
            "max": [max(p[i] for p in points) for i in range(3)],
            "dimensions": [max(p[i] for p in points) - min(p[i] for p in points) for i in range(3)]}


def extract(path: str, meters_per_unit: float | None = None) -> dict[str, Any]:
    doc, blob, file_info = read_glb(path)
    worlds, parents = hierarchy(doc)
    nodes = doc.get("nodes", [])
    meshes = doc.get("meshes", [])
    mesh_instances = []
    all_world_points = []
    primitive_count = vertex_count = triangle_count = 0
    warnings = []
    scenes = doc.get("scenes", [])
    active_scene = doc.get("scene", 0)
    if scenes:
        if not isinstance(active_scene, int) or not 0 <= active_scene < len(scenes):
            raise GlbError(f"active scene index out of range: {active_scene}")
        roots = scenes[active_scene].get("nodes", [])
    else:
        roots = [i for i, parent in enumerate(parents) if parent is None]
        active_scene = None
    active_nodes: set[int] = set()
    stack = list(roots)
    while stack:
        current = stack.pop()
        if not isinstance(current, int) or not 0 <= current < len(nodes):
            raise GlbError(f"scene references invalid root/child node {current}")
        if current in active_nodes:
            continue
        active_nodes.add(current)
        stack.extend(nodes[current].get("children", []))
    for ni, node in enumerate(nodes):
        if ni not in active_nodes or "mesh" not in node:
            continue
        mi = node["mesh"]
        if not isinstance(mi, int) or not 0 <= mi < len(meshes):
            raise GlbError(f"node {ni} references invalid mesh {mi}")
        prims = meshes[mi].get("primitives", [])
        instance_points = []
        p_rows = []
        for pi, prim in enumerate(prims):
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs:
                warnings.append(f"mesh {mi} primitive {pi} has no POSITION accessor")
                continue
            position_accessors = doc.get("accessors", [])
            position_index = attrs["POSITION"]
            if not isinstance(position_index, int) or not 0 <= position_index < len(position_accessors):
                raise GlbError(f"mesh {mi} primitive {pi} references invalid POSITION accessor")
            position_accessor = position_accessors[position_index]
            if position_accessor.get("type") != "VEC3" or position_accessor.get("componentType") != 5126:
                raise GlbError(f"mesh {mi} primitive {pi} POSITION must be a FLOAT VEC3 accessor")
            positions = accessor_values(doc, blob, position_index)
            local_positions = [tuple(p[:3]) for p in positions]
            is_skinned = "skin" in node
            world_positions = [] if is_skinned else [transform_point(worlds[ni], p) for p in local_positions]
            instance_points.extend(world_positions)
            all_world_points.extend(world_positions)
            primitive_count += 1
            vertex_count += len(positions)
            mode = prim.get("mode", 4)
            if not isinstance(mode, int) or mode not in range(7):
                raise GlbError(f"mesh {mi} primitive {pi} has invalid glTF primitive mode {mode}")
            indices = prim.get("indices")
            element_count = len(positions)
            if indices is not None:
                accessors = doc.get("accessors", [])
                if not isinstance(indices, int) or not 0 <= indices < len(accessors):
                    raise GlbError(f"mesh {mi} primitive {pi} references invalid index accessor {indices}")
                index_accessor = accessors[indices]
                if index_accessor.get("type") != "SCALAR" or index_accessor.get("componentType") not in (5121, 5123, 5125):
                    raise GlbError(f"mesh {mi} primitive {pi} indices must use an unsigned SCALAR accessor")
                idx = accessor_values(doc, blob, indices)
                if any(v < 0 or v >= len(positions) or not float(v).is_integer() for row in idx for v in row):
                    raise GlbError(f"mesh {mi} primitive {pi} contains an out-of-range or non-integer index")
                element_count = len(idx)
            if mode == 4:
                tris = element_count // 3
                if element_count % 3:
                    warnings.append(f"mesh {mi} primitive {pi}: TRIANGLES element count is not divisible by 3; trailing elements ignored")
            elif mode in (5, 6):
                tris = max(0, element_count - 2)
            else:
                tris = 0
            if mode in (0, 1, 2, 3):
                warnings.append(f"mesh {mi} primitive {pi}: primitive mode {mode} is not a triangle topology; triangle count is zero")
            triangle_count += tris
            p_rows.append({"primitive": pi, "mode": mode, "position_accessor": attrs["POSITION"],
                           "vertex_count": len(positions), "element_count": element_count, "triangle_count": tris,
                           "local_accessor_min": doc["accessors"][attrs["POSITION"]].get("min"),
                           "local_accessor_max": doc["accessors"][attrs["POSITION"]].get("max")})
        mesh_instances.append({"node_index": ni, "node_name": node.get("name", f"node_{ni}"),
                               "mesh_index": mi, "mesh_name": meshes[mi].get("name", f"mesh_{mi}"),
                               "skinned": "skin" in node,
                               "local_geometry_bounds": bounds([tuple(p[:3]) for prim in prims
                                   if "POSITION" in prim.get("attributes", {})
                                   for p in accessor_values(doc, blob, prim["attributes"]["POSITION"])]),
                               "world_bounds": bounds(instance_points) if "skin" not in node else None,
                               "bounds_status": "UNAVAILABLE_WITHOUT_SKIN_DEFORMATION" if "skin" in node else "COMPUTED_FROM_NODE_WORLD_TRANSFORM",
                               "primitives": p_rows})
        if "skin" in node:
            warnings.append(f"node {ni}: skinned mesh world bounds are not computed; skin deformation and inverse-bind matrices must be applied")
    joint_rows = []
    for si, skin in enumerate(doc.get("skins", [])):
        joints = skin.get("joints", [])
        ibm_index = skin.get("inverseBindMatrices")
        ibms = accessor_values(doc, blob, ibm_index) if ibm_index is not None else None
        if ibms is not None and len(ibms) != len(joints):
            warnings.append(f"skin {si}: inverseBindMatrices count {len(ibms)} != joints {len(joints)}")
        for ji, ni in enumerate(joints):
            if not isinstance(ni, int) or not 0 <= ni < len(nodes):
                raise GlbError(f"skin {si} joint {ji} references invalid node")
            m = worlds[ni]
            pos = [m[12], m[13], m[14]]
            joint_rows.append({"skin_index": si, "joint_index": ji, "node_index": ni,
                               "name": nodes[ni].get("name", f"node_{ni}"),
                               "parent_node_index": parents[ni],
                               "world_position": pos,
                               "inverse_bind_matrix_present": ibms is not None})
    overall = bounds(all_world_points)
    scale = meters_per_unit
    if scale is not None and (not math.isfinite(scale) or scale <= 0):
        raise GlbError("--meters-per-unit must be a finite positive number")
    dimensions = overall["dimensions"] if overall else None
    height_axis = 1  # glTF has no mandatory up axis; this is a convention, not an assumption.
    height_units = dimensions[height_axis] if dimensions else None
    return {
        "schema": "avatarforge.glb-measurements.v1",
        "source": {"path": os.path.basename(path), **file_info},
        "asset": doc.get("asset", {}),
        "coordinate_system": {"axis_order": "X,Y,Z", "up_axis": "Y (assumed convention)",
                              "units": "metres" if scale else "unknown GLB coordinate units",
                              "meters_per_unit": scale,
                              "height_axis": "Y", "height_axis_is_convention_not_proven": True},
        "scene": {"scene_count": len(doc.get("scenes", [])),
                  "active_scene": active_scene,
                  "active_node_count": len(active_nodes),
                  "node_count": len(nodes), "mesh_count": len(meshes),
                  "mesh_instance_count": len(mesh_instances),
                  "primitive_count": primitive_count, "vertex_count_sum": vertex_count,
                  "triangle_count_sum": triangle_count,
                  "skin_count": len(doc.get("skins", [])),
                  "animation_count": len(doc.get("animations", []))},
        "geometry": {"world_bounds": overall,
                     "height_axis_units": height_units,
                     "height_m": height_units * scale if height_units is not None and scale else None,
                     "mesh_instances": mesh_instances},
        "skins": [{"skin_index": i, "name": skin.get("name"),
                   "skeleton_root_node": skin.get("skeleton"),
                   "joint_count": len(skin.get("joints", [])),
                   "inverse_bind_matrices_accessor": skin.get("inverseBindMatrices")}
                  for i, skin in enumerate(doc.get("skins", []))],
        "joints": joint_rows,
        "materials": [{"index": i, "name": m.get("name"),
                       "pbr_metallic_roughness": m.get("pbrMetallicRoughness", {}),
                       "normal_texture": m.get("normalTexture"),
                       "occlusion_texture": m.get("occlusionTexture"),
                       "emissive_texture": m.get("emissiveTexture")}
                      for i, m in enumerate(doc.get("materials", []))],
        "images": [{"index": i, "name": image.get("name"),
                    "mime_type": image.get("mimeType"), "buffer_view": image.get("bufferView"),
                    "uri_present": "uri" in image}
                   for i, image in enumerate(doc.get("images", []))],
        "measurement_confidence": {
            "world_mesh_bounds": "computed_from_POSITION_vertices_and_node_world_transforms_for_unskinned_mesh_instances_only",
            "world_mesh_bounds_scope": "active_scene; skinned mesh instances are excluded until skin deformation is evaluated",
            "height": "coordinate_units_only; metric height unavailable" if not scale else
                      "metric conversion supplied externally; validate scale provenance",
            "joint_positions": "computed_from_joint_node_world_origins",
            "anthropometric_circumferences": "not derivable from bounds/joint origins alone",
            "leg_length": "not assigned; requires validated anatomical joint-chain mapping"
        },
        "warnings": warnings
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input_glb")
    ap.add_argument("-o", "--output", help="JSON output path (default: stdout)")
    ap.add_argument("--meters-per-unit", type=float,
                    help="externally verified metres represented by one GLB coordinate unit")
    args = ap.parse_args()
    try:
        result = extract(args.input_glb, args.meters_per_unit)
        rendered = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(rendered)
        else:
            sys.stdout.write(rendered)
        return 0
    except (OSError, KeyError, IndexError, TypeError, ValueError, struct.error) as exc:
        print(f"glb_measurements: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
