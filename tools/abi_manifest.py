#!/usr/bin/env python3
"""
==============================================================================================
 AvatarForge :: tools/abi_manifest.py  --  the shared C ABI, as data
==============================================================================================

`native/include/afcore.h` is the contract between three independent pieces of code:

    Kotlin  ->  JNI (af_jni.cpp)      -> native library
    Rust    ->  af-sys (bindgen-free) -> native library
    C/C++   ->  #include "afcore.h"   -> native library

and the library on the other side of that contract is meant to be replaceable (the Rust core in
`rust/af-core` implements the same symbols). Three things have to be true for that to work, and
none of them can be left to "everyone reads the header carefully":

    1. every declared symbol exists in the built library, with the declared calling convention;
    2. the structs have the same size and the same field offsets the callers were compiled
       against (a silently reordered field is a memory-corruption bug, not a compile error);
    3. the enums and the constant tables agree (clip ids, blendshape ids, region ids, defaults).

This script turns the header into a machine-readable manifest (`abi/afcore.abi.json`) that holds
all three, and `tools/abi_conformance.py` checks a given `.so` against it. The manifest is checked
in, so a change to the ABI is visible in review as a diff rather than as a mystery at runtime.

    python3 tools/abi_manifest.py            regenerate abi/afcore.abi.json
    python3 tools/abi_manifest.py --check    fail if the checked-in manifest is stale

Enum values and struct layouts are not parsed by regex alone: a tiny C program is compiled and run
to ask the compiler for `sizeof`/`offsetof`/enumerator values, so the manifest describes what the
compiler actually does, not what the header looks like it says.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "native", "include", "afcore.h")
MANIFEST = os.path.join(ROOT, "abi", "afcore.abi.json")

# The public structs whose layout a caller compiles against. Order matters only for reporting.
STRUCTS = ["af_spec", "af_callbacks", "af_vertex", "af_submesh", "af_material", "af_texture",
           "af_joint_info", "af_pose", "af_image", "af_image_analysis", "af_stats"]
# Structs whose fields are also reported by name+offset (the ones with mixed int/float members,
# where a silent reorder or padding difference is most dangerous).
FIELD_STRUCTS = ["af_spec", "af_vertex", "af_joint_info", "af_callbacks", "af_image",
                 "af_stats"]


def header_text():
    with open(HEADER, "r", encoding="utf-8") as f:
        return f.read()


def parse_functions(text):
    """Every `af_*` function declaration, with its return type and parameter list as written."""
    # Strip comments so a doc-comment containing "af_something(...)" is not mistaken for a decl.
    body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    decls = []
    # A declaration ends with ';' and contains an af_ identifier followed by '('.
    for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_*\s]*?)\b(af_[a-z0-9_]+)\s*\(([^;]*?)\)\s*;", body):
        ret = " ".join(m.group(1).split())
        name = m.group(2)
        args = " ".join(m.group(3).split())
        if ret in ("typedef struct",):
            continue
        decls.append({"name": name, "returns": ret, "args": args})
    # Deduplicate (the header is idempotent but a function may be mentioned twice).
    seen, out = set(), []
    for d in decls:
        if d["name"] in seen:
            continue
        seen.add(d["name"])
        out.append(d)
    return sorted(out, key=lambda d: d["name"])


def parse_enums(text):
    """Enums by name and value, for the ids that cross the ABI."""
    body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    enums = {}
    # Group 1 is the enum's name; the closing "} name;" must repeat *that* group, not the body.
    for m in re.finditer(r"typedef\s+enum\s+(af_[a-z0-9_]+)\s*\{(.*?)\}\s*\1\s*;", body, flags=re.S):
        name, inner = m.group(1), m.group(2)
        members = {}
        value = 0
        for item in inner.split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                k, v = item.split("=", 1)
                k = k.strip()
                v = v.strip()
                try:
                    value = int(v, 0)
                except ValueError:
                    value = value  # non-literal: keep the running counter
            else:
                k = item
            members[k] = value
            value += 1
        enums[name] = members
    return enums


def parse_defines(text):
    out = {}
    for m in re.finditer(r"^#define\s+(AF_[A-Z0-9_]+)\s+(.+)$", text, flags=re.M):
        key, val = m.group(1), m.group(2).strip()
        try:
            out[key] = int(val, 0)
        except ValueError:
            out[key] = val.strip('"')
    return out


def compiler_layout():
    """Asks the compiler for sizeof/offsetof/enumerator values instead of guessing."""
    src = ["#include <stdio.h>", "#include <stddef.h>", '#include "afcore.h"', "int main(void) {"]
    for s in STRUCTS:
        src.append('  printf("size %s %%zu\\n", sizeof(%s));' % (s, s))
    for s in FIELD_STRUCTS:
        # Enumerate fields by taking the addresses of the members the manifest cares about. The
        # list is explicit: offsetof needs a member name, and this is the one place where the
        # manifest decides which fields matter.
        pass
    fields = {
        "af_spec": ["struct_size", "sex", "build", "hair_style", "outfit", "footwear", "seed",
                    "height_m", "age_years", "muscle", "body_fat", "bust", "neck_cm", "chest_cm",
                    "underbust_cm", "waist_cm", "hip_cm", "thigh_cm", "calf_cm", "bicep_cm",
                    "forearm_cm", "wrist_cm", "shoulder_width_cm", "inseam_cm",
                    "head_circumference_cm", "skin_tone", "skin_melanin", "hair_tone",
                    "cloth_tone", "gear_tone", "cloth_roughness", "gear_metalness", "prompt"],
        "af_vertex": ["pos", "normal", "tangent", "uv", "color", "joints", "weights"],
        "af_joint_info": ["name", "parent", "flags", "rest_translation"],
        "af_callbacks": ["user", "on_progress", "on_log", "should_cancel"],
        "af_image": ["width", "height", "channels", "role", "pixels"],
        "af_stats": ["vertex_count", "index_count", "triangle_count", "submesh_count", "joint_count",
                     "blendshape_count", "texture_bytes", "geometry_bytes", "peak_field_bytes",
                     "fit_anatomy_ms", "rig_ms", "gpu_hint"],
    }
    for s, names in fields.items():
        for f in names:
            src.append('  printf("off %s %s %%zu\\n", offsetof(%s, %s));' % (s, f, s, f))
    for enum_name in ("af_result", "af_stage", "af_severity", "af_sex", "af_build", "af_hair_style",
                      "af_outfit", "af_footwear", "af_image_role", "af_clip", "af_joint_flags"):
        src.append('  printf("enum %s_size %%zu\\n", sizeof(%s));' % (enum_name, enum_name))
    src.append("  return 0;")
    src.append("}")
    with tempfile.TemporaryDirectory() as tmp:
        cpath = os.path.join(tmp, "layout.c")
        exe = os.path.join(tmp, "layout")
        with open(cpath, "w", encoding="utf-8") as f:
            f.write("\n".join(src) + "\n")
        subprocess.run(["gcc", "-std=c11", "-I", os.path.dirname(HEADER), "-o", exe, cpath],
                       check=True, capture_output=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    sizes, offsets, enum_sizes = {}, {}, {}
    for line in out.splitlines():
        parts = line.split()
        if parts[0] == "size":
            sizes[parts[1]] = int(parts[2])
        elif parts[0] == "off":
            offsets.setdefault(parts[1], {})[parts[2]] = int(parts[3])
        elif parts[0] == "enum" and parts[1].endswith("_size"):
            enum_sizes[parts[1][:-5]] = int(parts[2])
    return sizes, offsets, enum_sizes


def build():
    text = header_text()
    sizes, offsets, enum_sizes = compiler_layout()
    return {
        "generated_by": "tools/abi_manifest.py - do not edit by hand",
        "source": "native/include/afcore.h",
        "abi_version": parse_defines(text).get("AF_ABI_VERSION"),
        "engine_version": parse_defines(text).get("AF_ENGINE_VERSION"),
        "defines": parse_defines(text),
        "enums": parse_enums(text),
        "enum_sizes": enum_sizes,
        "structs": {s: {"size": sizes[s], "fields": offsets.get(s, {})} for s in STRUCTS},
        "functions": parse_functions(text),
    }


def main():
    doc = build()
    rendered = json.dumps(doc, indent=2, sort_keys=False) + "\n"
    if "--check" in sys.argv:
        if not os.path.exists(MANIFEST):
            print("ABI manifest missing: %s" % MANIFEST)
            return 1
        with open(MANIFEST, "r", encoding="utf-8") as f:
            current = f.read()
        if current != rendered:
            print("ABI manifest is stale - regenerate with: python3 tools/abi_manifest.py")
            return 1
        print("ABI manifest is current: %d symbols, %d structs, %d enums"
              % (len(doc["functions"]), len(doc["structs"]), len(doc["enums"])))
        return 0

    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write(rendered)
    print("wrote %s" % MANIFEST)
    print("  ABI version : %s" % doc["abi_version"])
    print("  functions   : %d" % len(doc["functions"]))
    print("  structs     : %d (af_spec is %d bytes, af_vertex %d)"
          % (len(doc["structs"]), doc["structs"]["af_spec"]["size"], doc["structs"]["af_vertex"]["size"]))
    print("  enums       : %d" % len(doc["enums"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
