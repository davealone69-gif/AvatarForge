#!/usr/bin/env python3
"""
glb_census.py -- what is actually inside a generated .glb?

Prints, per material and per region: triangle count, vertex share, material properties and which
textures it references. This is the quickest way to answer "did the generator really put clothing /
hair / teeth in this avatar, or did it quietly skip them?" - it reads the exported file, not the
engine's own opinion of itself.

Usage: glb_census.py <file.glb>
"""
import json
import struct
import sys

REGIONS = ["skin", "eye_left", "eye_right", "teeth", "tongue", "lips", "hair", "lash",
           "nail", "cloth_upper", "cloth_lower", "shoe", "gear"]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    data = open(path, "rb").read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        print("not a glb")
        return 1
    json_len, json_type = struct.unpack_from("<II", data, 12)
    doc = json.loads(data[20:20 + json_len].decode("utf-8"))

    print("file      : %s (%.2f MB, glTF %d)" % (path, len(data) / 1048576.0, version))
    counts = doc.get("extras", {}).get("counts", {})
    print("counts    : %s" % ", ".join("%s=%s" % (k, v) for k, v in counts.items()))
    uv = doc.get("extras", {}).get("uv", {})
    if uv:
        print("uv        : %s charts, coverage %.2f, %.0f texels/m" %
              (uv.get("charts"), uv.get("coverage", 0), uv.get("texelsPerMeter", 0)))
    spec = doc.get("extras", {}).get("spec", {})
    if spec:
        print("spec      : sex=%s build=%s height=%.3f outfit=%s footwear=%s hair=%s" %
              (spec.get("sex"), spec.get("build"), spec.get("height_m", 0),
               spec.get("outfit"), spec.get("footwear"), spec.get("hair_style")))

    prims = doc["meshes"][0]["primitives"]
    mats = doc.get("materials", [])
    total = 0
    rows = []
    for p in prims:
        idx = doc["accessors"][p["indices"]]
        tris = idx["count"] // 3
        total += tris
        mat = mats[p["material"]]
        region = p.get("extras", {}).get("region", mat.get("extras", {}).get("region", -1))
        pbr = mat.get("pbrMetallicRoughness", {})
        rows.append((region, mat["name"], tris,
                     pbr.get("baseColorFactor", [1, 1, 1, 1]),
                     pbr.get("metallicFactor"), pbr.get("roughnessFactor"),
                     len(p.get("targets", [])),
                     "T" if "baseColorTexture" in pbr else "-",
                     "N" if "normalTexture" in mat else "-"))
    rows.sort(key=lambda r: -r[2])
    print("\n%-12s %-12s %10s %6s %7s %7s %6s %s" %
          ("region", "material", "triangles", "share", "metal", "rough", "morphs", "maps"))
    for region, name, tris, base, metal, rough, morphs, t, n in rows:
        label = REGIONS[region] if 0 <= region < len(REGIONS) else "?"
        print("%-12s %-12s %10d %5.1f%% %7.3f %7.3f %6d  %s%s   base=(%.2f %.2f %.2f)" %
              (label, name, tris, 100.0 * tris / max(1, total), metal or 0, rough or 0, morphs, t, n,
               base[0], base[1], base[2]))
    print("%-12s %-12s %10d" % ("TOTAL", "", total))

    present = {REGIONS[r[0]] for r in rows if 0 <= r[0] < len(REGIONS)}
    missing = [r for r in REGIONS if r not in present]
    print("\npresent   : %s" % ", ".join(sorted(present)))
    if missing:
        print("absent    : %s" % ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
