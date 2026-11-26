#!/usr/bin/env python3
"""Compare `af_engine_set_gpu_hint` and its stats output across native and Rust libraries.

Build both libraries first, then run:
    python3 tools/check_gpu_hint_parity.py build-host/libafcore.so rust/target/debug/libafcore.so
"""
import ctypes as C
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "abi", "afcore.abi.json")
AF_OK = 0
AF_ERR_INVALID_ARG = -1


def load_abi():
    with open(MANIFEST, "r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    stats = manifest["structs"]["af_stats"]
    spec_size = manifest["structs"]["af_spec"]["size"]
    hint_offset = stats["fields"]["gpu_hint"]
    stats_size = stats["size"]
    hint_size = 64
    if hint_offset + hint_size > stats_size:
        raise ValueError("af_stats.gpu_hint exceeds the manifest struct size")
    return spec_size, stats_size, hint_offset, hint_size


def open_library(path, spec_size):
    lib = C.CDLL(os.path.abspath(path))
    lib.af_spec_init.argtypes = [C.c_void_p]
    lib.af_spec_init.restype = None
    lib.af_engine_create.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p)]
    lib.af_engine_create.restype = C.c_int32
    lib.af_engine_destroy.argtypes = [C.c_void_p]
    lib.af_engine_destroy.restype = None
    lib.af_engine_stats.argtypes = [C.c_void_p, C.c_void_p]
    lib.af_engine_stats.restype = C.c_int32
    lib.af_engine_set_gpu_hint.argtypes = [C.c_void_p, C.c_char_p]
    lib.af_engine_set_gpu_hint.restype = C.c_int32
    return lib


def snapshot(lib, engine, stats_size, hint_offset, hint_size):
    raw = (C.c_ubyte * stats_size)()
    rc = lib.af_engine_stats(engine, C.cast(raw, C.c_void_p))
    if rc != AF_OK:
        raise AssertionError("af_engine_stats returned %d" % rc)
    data = bytes(raw)
    # Compare all defined fields before gpu_hint and the complete hint array. Ignore only
    # potential trailing C-struct padding after the final declared field.
    return data[:hint_offset], data[hint_offset:hint_offset + hint_size]


def run_cases(path, spec_size, stats_size, hint_offset, hint_size):
    lib = open_library(path, spec_size)
    spec = (C.c_ubyte * spec_size)()
    lib.af_spec_init(C.cast(spec, C.c_void_p))
    engine = C.c_void_p()
    rc = lib.af_engine_create(C.cast(spec, C.c_void_p), None, C.byref(engine))
    if rc != AF_OK or not engine.value:
        raise AssertionError("%s: default engine creation returned %d" % (path, rc))

    try:
        initial = snapshot(lib, engine, stats_size, hint_offset, hint_size)
        invalid_results = (
            lib.af_engine_set_gpu_hint(None, b"gles31"),
            lib.af_engine_set_gpu_hint(engine, None),
        )
        if invalid_results != (AF_ERR_INVALID_ARG, AF_ERR_INVALID_ARG):
            raise AssertionError("%s: null-argument results were %r" % (path, invalid_results))
        if snapshot(lib, engine, stats_size, hint_offset, hint_size) != initial:
            raise AssertionError("%s: rejected calls changed stats" % path)

        cases = (
            b"x" * 80,             # 63-byte payload plus terminator
            b"vk",                  # short update must preserve the old tail
            b"",                    # empty string only writes the first NUL
            b"g\x00ignored",        # C-string stops at its first NUL
        )
        results = []
        previous = initial[1]
        prefix_before = initial[0]
        for hint in cases:
            result = lib.af_engine_set_gpu_hint(engine, hint)
            if result != AF_OK:
                raise AssertionError("%s: setter returned %d for %r" % (path, result, hint))
            observed = snapshot(lib, engine, stats_size, hint_offset, hint_size)
            visible = hint.split(b"\0", 1)[0][:hint_size - 1]
            expected = visible + b"\0" + previous[len(visible) + 1:]
            if observed[1] != expected:
                raise AssertionError(
                    "%s: %r produced gpu_hint %r, expected %r" %
                    (path, hint, observed[1], expected)
                )
            if observed[0] != prefix_before:
                raise AssertionError("%s: setter changed non-hint stats" % path)
            results.append((result, observed[0], observed[1]))
            previous = observed[1]
        return initial, invalid_results, results
    finally:
        lib.af_engine_destroy(engine)


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    spec_size, stats_size, hint_offset, hint_size = load_abi()
    native = run_cases(argv[1], spec_size, stats_size, hint_offset, hint_size)
    rust = run_cases(argv[2], spec_size, stats_size, hint_offset, hint_size)
    if native != rust:
        print("FAIL: native and Rust GPU-hint behavior differs", file=sys.stderr)
        print("native: %r" % (native,), file=sys.stderr)
        print("rust:   %r" % (rust,), file=sys.stderr)
        return 1
    print("PASS: native and Rust match for null arguments, 63-byte truncation, short/empty updates, embedded NUL, and preserved stats")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
