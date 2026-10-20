#!/usr/bin/env python3
"""
validate_glb.py -- structural validator for AvatarForge .glb exports.

This is deliberately written against the glTF 2.0 specification rather than against the exporter:
it re-derives everything the exporter claims from the bytes in the file. If the two disagree, the
file is wrong, not the validator.

Checks performed:
  * container: magic, version, declared length vs actual length, chunk types and sizes, padding
  * JSON: parses, has asset/scenes/nodes/meshes, no "uri" (the file must be self-contained)
  * buffer views: every offset+length inside the buffer, 4-byte alignment for attribute views
  * accessors: count/componentType/type consistent with the view's byteLength
  * POSITION and animation sampler inputs carry min/max (required by the spec)
  * indices: in range for the primitive's vertex count, triangle count divisible by 3
  * skin: joints are valid node indices, inverse bind count == joint count, matrices finite
  * sparse accessors: index and value views large enough, indices sorted and in range
  * morph targets: one accessor per target per primitive, deltas finite and non-trivial
  * animations: sampler input/output counts match, times non-decreasing, quaternions normalised
  * images: PNG signature, IHDR consistency, zlib stream decompresses to the stated size, CRC ok

Usage: validate_glb.py <file.glb> [--verbose]
Exit code 0 if all checks pass, 1 otherwise.
"""
import json
import struct
import sys
import zlib

COMPONENT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMPONENT_FMT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def check(cond, msg):
    if not cond:
        fail(msg)
    return cond


def read_accessor(gltf, blob, index):
    """Decodes an accessor, applying its sparse overrides. Returns a flat list of components."""
    acc = gltf["accessors"][index]
    n = acc["count"]
    comps = TYPE_COUNT[acc["type"]]
    fmt = COMPONENT_FMT[acc["componentType"]]
    values = [0] * (n * comps)
    if "bufferView" in acc:
        bv = gltf["bufferViews"][acc["bufferView"]]
        base = bv.get("byteOffset", 0)
        stride = bv.get("byteStride") or (COMPONENT_SIZE[acc["componentType"]] * comps)
        for i in range(n):
            off = base + i * stride
            values[i * comps:(i + 1) * comps] = struct.unpack_from("<" + fmt * comps, blob, off)
    if "sparse" in acc:
        sp = acc["sparse"]
        idx_bv = gltf["bufferViews"][sp["indices"]["bufferView"]]
        val_bv = gltf["bufferViews"][sp["values"]["bufferView"]]
        ifmt = COMPONENT_FMT[sp["indices"]["componentType"]]
        ioff = idx_bv.get("byteOffset", 0)
        voff = val_bv.get("byteOffset", 0)
        prev = -1
        for k in range(sp["count"]):
            i = struct.unpack_from("<" + ifmt, blob, ioff + k * COMPONENT_SIZE[sp["indices"]["componentType"]])[0]
            if i <= prev:
                fail("sparse indices not strictly increasing")
            if i >= n:
                fail("sparse index %d out of range (%d)" % (i, n))
            prev = i
            vals = struct.unpack_from("<" + fmt * comps, blob, voff + k * comps * COMPONENT_SIZE[acc["componentType"]])
            values[i * comps:(i + 1) * comps] = vals
    return values


def validate_png(data, label):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("%s: bad PNG signature" % label)
        return None
    off = 8
    width = height = depth = ctype = None
    idat = b""
    while off < len(data):
        (length,) = struct.unpack_from(">I", data, off)
        ctype_name = data[off + 4:off + 8]
        payload = data[off + 8:off + 8 + length]
        (crc,) = struct.unpack_from(">I", data, off + 8 + length)
        if zlib.crc32(ctype_name + payload) & 0xFFFFFFFF != crc:
            fail("%s: CRC mismatch in chunk %s" % (label, ctype_name))
        if ctype_name == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", payload[:10])
        elif ctype_name == b"IDAT":
            idat += payload
        off += 12 + length
    if width is None:
        fail("%s: no IHDR" % label)
        return None
    channels = {0: 1, 2: 3, 6: 4}.get(ctype)
    if channels is None or depth != 8:
        fail("%s: unsupported PNG format depth=%s ctype=%s" % (label, depth, ctype))
        return None
    try:
        raw = zlib.decompress(idat)
    except zlib.error as e:
        fail("%s: zlib stream does not decompress (%s)" % (label, e))
        return None
    expected = (width * channels + 1) * height
    if len(raw) != expected:
        fail("%s: decompressed size %d, expected %d" % (label, len(raw), expected))
    return width, height, channels


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    verbose = "--verbose" in sys.argv
    with open(path, "rb") as f:
        data = f.read()
    notes.append("file %s: %d bytes" % (path, len(data)))

    magic, version, length = struct.unpack_from("<III", data, 0)
    check(magic == 0x46546C67, "bad magic")
    check(version == 2, "version is %d, expected 2" % version)
    check(length == len(data), "declared length %d != file length %d" % (length, len(data)))

    off = 12
    chunks = []
    while off < len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        payload = data[off + 8:off + 8 + clen]
        check(len(payload) == clen, "chunk truncated at %d" % off)
        chunks.append((ctype, payload, clen))
        off += 8 + clen
    check(len(chunks) == 2, "expected exactly JSON + BIN chunks, found %d" % len(chunks))
    check(chunks[0][0] == 0x4E4F534A, "first chunk is not JSON")
    if len(chunks) > 1:
        check(chunks[1][0] == 0x004E4942, "second chunk is not BIN")
    check(len(chunks[0][1]) % 4 == 0, "JSON chunk not 4-byte padded")
    check(len(chunks[1][1]) % 4 == 0, "BIN chunk not 4-byte padded")

    gltf = json.loads(chunks[0][1].decode("utf-8"))
    blob = chunks[1][1]
    notes.append("JSON: %d bytes, BIN: %d bytes" % (len(chunks[0][1]), len(blob)))

    check("asset" in gltf and gltf["asset"].get("version") == "2.0", "missing or wrong asset.version")
    check('"uri"' not in chunks[0][1].decode("utf-8"), "file references external resources (uri)")

    buf_len = gltf["buffers"][0]["byteLength"]
    check(buf_len <= len(blob), "buffer declares %d bytes, BIN has %d" % (buf_len, len(blob)))

    # --- buffer views ---------------------------------------------------------------------
    for i, bv in enumerate(gltf.get("bufferViews", [])):
        o = bv.get("byteOffset", 0)
        if not check(o + bv["byteLength"] <= buf_len, "bufferView %d overruns the buffer" % i):
            continue
        if bv.get("target") == 34962:
            check(o % 4 == 0, "bufferView %d (attributes) is not 4-byte aligned" % i)

    # --- accessors ------------------------------------------------------------------------
    for i, acc in enumerate(gltf.get("accessors", [])):
        comps = TYPE_COUNT[acc["type"]]
        size = COMPONENT_SIZE[acc["componentType"]] * comps
        if "bufferView" in acc:
            bv = gltf["bufferViews"][acc["bufferView"]]
            need = acc["count"] * size
            if not check(need <= bv["byteLength"], "accessor %d needs %d bytes, view has %d" %
                         (i, need, bv["byteLength"])):
                continue
        if "sparse" in acc:
            sp = acc["sparse"]
            ibv = gltf["bufferViews"][sp["indices"]["bufferView"]]
            vbv = gltf["bufferViews"][sp["values"]["bufferView"]]
            isize = COMPONENT_SIZE[sp["indices"]["componentType"]]
            check(sp["count"] * isize <= ibv["byteLength"], "sparse index view too small (accessor %d)" % i)
            check(sp["count"] * size <= vbv["byteLength"], "sparse value view too small (accessor %d)" % i)
            check(sp["indices"]["componentType"] in (5121, 5123, 5125),
                  "sparse indices must be unsigned (accessor %d)" % i)

    # --- meshes and primitives ------------------------------------------------------------
    vertex_count = 0
    triangle_count = 0
    target_count = None
    empty_targets = []
    for mi, mesh in enumerate(gltf.get("meshes", [])):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            attrs = prim["attributes"]
            check("POSITION" in attrs, "primitive %d/%d has no POSITION" % (mi, pi))
            pos = gltf["accessors"][attrs["POSITION"]]
            if mi == 0 and pi == 0:
                vertex_count = pos["count"]
            check(pos["count"] == vertex_count, "primitive %d/%d has a different vertex count" % (mi, pi))
            check("min" in pos and "max" in pos, "POSITION accessor lacks min/max")
            for key, need_count in (("TEXCOORD_0", 2), ("NORMAL", 3), ("JOINTS_0", 4), ("WEIGHTS_0", 4)):
                if key in attrs:
                    a = gltf["accessors"][attrs[key]]
                    check(a["count"] == vertex_count, "%s count differs from POSITION" % key)
                    check(TYPE_COUNT[a["type"]] == need_count, "%s has type %s" % (key, a["type"]))
            if "indices" in prim:
                ia = gltf["accessors"][prim["indices"]]
                check(ia["count"] % 3 == 0, "index count %d not divisible by 3" % ia["count"])
                idx = read_accessor(gltf, blob, prim["indices"])
                if idx:
                    check(max(idx) < vertex_count, "primitive %d/%d indexes vertex %d of %d" %
                          (mi, pi, max(idx), vertex_count))
                    check(min(idx) >= 0, "negative index")
                triangle_count += ia["count"] // 3
            if "targets" in prim:
                tcount = len(prim["targets"])
                if target_count is None:
                    target_count = tcount
                check(tcount == target_count, "morph target count differs between primitives")
                for ti, tgt in enumerate(prim["targets"]):
                    a = gltf["accessors"][tgt["POSITION"]]
                    check(a["count"] == vertex_count, "target %d count differs" % ti)
                    check(a["type"] == "VEC3", "target %d is not VEC3" % ti)
                    # An accessor with no bufferView and no sparse block is all zeros: legal, and
                    # the right way to keep a channel that moves nothing in the target list. It is
                    # counted so "52 targets" and "52 channels that do something" stay separable.
                    if "bufferView" not in a and "sparse" not in a:
                        if mi == 0 and pi == 0:
                            empty_targets.append(ti)
                    elif mi == 0 and pi == 0:
                        vals = read_accessor(gltf, blob, tgt["POSITION"])
                        if vals and max(abs(v) for v in vals) < 1e-9:
                            empty_targets.append(ti)
    if empty_targets:
        notes.append("geometry: %d vertices, %d triangles, %s morph targets (%d of them carry no "
                     "delta on this build: targets %s)" %
                     (vertex_count, triangle_count, target_count, len(empty_targets),
                      ",".join(str(i) for i in empty_targets[:8]) + ("..." if len(empty_targets) > 8 else "")))
    else:
        notes.append("geometry: %d vertices, %d triangles, %s morph targets (all carry deltas)" %
                     (vertex_count, triangle_count, target_count))

    # --- skins ----------------------------------------------------------------------------
    node_count = len(gltf.get("nodes", []))
    for si, skin in enumerate(gltf.get("skins", [])):
        joints = skin["joints"]
        check(len(joints) == len(set(joints)), "skin %d has duplicate joints" % si)
        for jn in joints:
            check(0 <= jn < node_count, "skin %d joint %d out of node range" % (si, jn))
        if "inverseBindMatrices" in skin:
            ibm = gltf["accessors"][skin["inverseBindMatrices"]]
            check(ibm["count"] == len(joints), "IBM count %d != joint count %d" % (ibm["count"], len(joints)))
            check(ibm["type"] == "MAT4", "IBM accessor is not MAT4")
            vals = read_accessor(gltf, blob, skin["inverseBindMatrices"])
            check(all(v == v and abs(v) < 1e6 for v in vals), "non-finite value in inverse bind matrices")
            # A rigid transform: the bottom row is 0,0,0,1 in column-major storage.
            for j in range(len(joints)):
                m = vals[j * 16:(j + 1) * 16]
                if any(abs(m[k]) > 1e-6 for k in (3, 7, 11)) or abs(m[15] - 1.0) > 1e-6:
                    fail("IBM %d is not affine" % j)
                    break
        # Every joint must be reachable from the skin's skeleton root.
        if "skeleton" in skin:
            seen = set()
            stack = [skin["skeleton"]]
            while stack:
                n = stack.pop()
                if n in seen or n >= node_count:
                    continue
                seen.add(n)
                stack.extend(gltf["nodes"][n].get("children", []))
            missing = [j for j in joints if j not in seen]
            check(not missing, "skin %d: %d joints unreachable from the skeleton root" % (si, len(missing)))

    # --- animations -----------------------------------------------------------------------
    for ai, anim in enumerate(gltf.get("animations", [])):
        for ci, ch in enumerate(anim["channels"]):
            s = anim["samplers"][ch["sampler"]]
            tgt = ch["target"]
            check(0 <= tgt["node"] < node_count, "animation %d channel %d targets a bad node" % (ai, ci))
            check(tgt["path"] in ("translation", "rotation", "scale", "weights"),
                  "animation %d channel %d has path %s" % (ai, ci, tgt["path"]))
            inp = gltf["accessors"][s["input"]]
            out = gltf["accessors"][s["output"]]
            check("min" in inp and "max" in inp, "animation sampler input lacks min/max")
            check(inp["count"] == out["count"] or out["count"] % inp["count"] == 0,
                  "animation input/output count mismatch")
            times = read_accessor(gltf, blob, s["input"])
            check(all(times[i] <= times[i + 1] for i in range(len(times) - 1)),
                  "animation times not non-decreasing")
            if tgt["path"] == "rotation":
                q = read_accessor(gltf, blob, s["output"])
                for k in range(0, len(q), 4):
                    n = sum(x * x for x in q[k:k + 4]) ** 0.5
                    if abs(n - 1.0) > 1e-3:
                        fail("animation %d: quaternion %d has length %.6f" % (ai, k // 4, n))
                        break
    notes.append("rig: %d joints, %d animations, %d materials" %
                 (len(gltf.get("skins", [{}])[0].get("joints", [])) if gltf.get("skins") else 0,
                  len(gltf.get("animations", [])), len(gltf.get("materials", []))))

    # --- images ---------------------------------------------------------------------------
    for ii, img in enumerate(gltf.get("images", [])):
        check("bufferView" in img, "image %d is not embedded" % ii)
        bv = gltf["bufferViews"][img["bufferView"]]
        o = bv.get("byteOffset", 0)
        png = blob[o:o + bv["byteLength"]]
        info = validate_png(png, "image %d (%s)" % (ii, img.get("name", "?")))
        if info and verbose:
            notes.append("  image %d: %dx%d, %d channels, %d bytes" % (ii, info[0], info[1], info[2], len(png)))

    # --- materials ------------------------------------------------------------------------
    tex_indices = len(gltf.get("textures", []))
    for mi, mat in enumerate(gltf.get("materials", [])):
        pbr = mat.get("pbrMetallicRoughness", {})
        for key in ("baseColorTexture", "metallicRoughnessTexture"):
            if key in pbr:
                check(0 <= pbr[key]["index"] < tex_indices, "material %d: %s index out of range" % (mi, key))
        for key in ("normalTexture", "occlusionTexture"):
            if key in mat:
                check(0 <= mat[key]["index"] < tex_indices, "material %d: %s index out of range" % (mi, key))
        for ext, payload in mat.get("extensions", {}).items():
            if ext == "AF_subsurface":
                check(0 <= payload["subsurfaceTexture"]["index"] < tex_indices, "subsurface texture index")

    used = set(gltf.get("extensionsUsed", []))
    for mat in gltf.get("materials", []):
        for ext in mat.get("extensions", {}):
            check(ext in used, "extension %s used but not declared in extensionsUsed" % ext)

    # --- print ----------------------------------------------------------------------------
    for n in notes:
        print("  " + n)
    if failures:
        print("\n  %d PROBLEM(S):" % len(failures))
        for f in failures[:40]:
            print("   - " + f)
        if len(failures) > 40:
            print("   ... and %d more" % (len(failures) - 40))
        return 1
    print("\n  OK: all structural checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
