#!/usr/bin/env python3
"""Generate tiny, valid GLB corpus seeds plus hostile accessor/bufferView variants.

Usage: python3 native/tests/make_glb_fuzz_corpus.py /tmp/avatarforge-glb-corpus
"""
import json
import pathlib
import struct
import sys

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build-glb-fuzz/corpus")
OUT.mkdir(parents=True, exist_ok=True)
BIN = struct.pack("<3f", 0.0, 0.0, 0.0)


def make_glb(*, count=1, accessor_offset=0, view_offset=0, view_length=12,
             stride=None, buffer_index=0, sparse=None):
    view = {"buffer": buffer_index, "byteOffset": view_offset, "byteLength": view_length}
    if stride is not None:
        view["byteStride"] = stride
    accessor = {"bufferView": 0, "byteOffset": accessor_offset,
                "componentType": 5126, "count": count, "type": "VEC3"}
    if sparse is not None:
        accessor.pop("bufferView", None)
        accessor["sparse"] = sparse
    doc = {
        "asset": {"version": "2.0", "generator": "AvatarForge fuzz seed"},
        "buffers": [{"byteLength": len(BIN)}],
        "bufferViews": [view],
        "accessors": [accessor],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0
    }
    js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    binary = BIN + b" " * ((-len(BIN)) % 4)
    total = 12 + 8 + len(js) + 8 + len(binary)
    return struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(binary), 0x004E4942) + binary


cases = {
    "valid.glb": {},
    "accessor_count_overflow.glb": {"count": (1 << 63) - 1},
    "accessor_offset_overflow.glb": {"accessor_offset": (1 << 64) - 1},
    "bufferView_offset_overflow.glb": {"view_offset": (1 << 64) - 1},
    "bufferView_length_overflow.glb": {"view_offset": (1 << 64) - 8, "view_length": 32},
    "stride_above_limit.glb": {"stride": 256},
    "stride_below_limit.glb": {"stride": 2},
    "invalid_buffer_index.glb": {"buffer_index": 1},
}
for name, kwargs in cases.items():
    (OUT / name).write_bytes(make_glb(**kwargs))
print(f"Wrote {len(cases)} GLB fuzz seeds to {OUT}")
