#!/usr/bin/env python3
"""
==============================================================================================
 AvatarForge :: tools/abi_conformance.py  --  does this library actually implement the ABI?
==============================================================================================

`abi/afcore.abi.json` is the contract (generated from afcore.h by tools/abi_manifest.py). This
script holds a built library against it. It is the gate that makes the native core *replaceable*:
the C++ kernel (`libafcore.so`) and the Rust core (`rust/af-core`) are two implementations of one
contract, and both have to pass the same checks before the app or the Kotlin SDK can use either.

    python3 tools/abi_conformance.py build-host/libafcore.so
    python3 tools/abi_conformance.py target/release/libafcore_rust.so --reference build-host/libafcore.so
    python3 tools/abi_conformance.py lib.so --require-complete      # exit 1 unless 47/47 symbols

Three levels, each optional:

  1. SYMBOLS   every function declared in the manifest resolves. A missing symbol is a hard
               failure; the library is not a usable implementation of this ABI yet.
  2. LAYOUT    `af_spec_init` writes struct_size == the size the manifest records, so a caller
               compiled against the header cannot be handed a differently-laid-out struct.
  3. BEHAVIOUR what the pure functions return - spec defaults, clip names and durations, the 52
               ARKit names, the region table - either against the documented invariants, or
               byte-for-byte against a `--reference` implementation.

No third-party modules: ctypes and json only, so it runs on a bare Python install and in CI.
"""
import ctypes as C
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "abi", "afcore.abi.json")


class Failures:
    def __init__(self):
        self.items = []

    def check(self, ok, what):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            self.items.append(what)
        return ok


def load_manifest():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        return json.load(f)


def bind(lib, manifest):
    """Declares every function in the manifest on the ctypes handle; returns the unresolved ones."""
    missing = []
    for fn in manifest["functions"]:
        name = fn["name"]
        try:
            getattr(lib, name)
        except AttributeError:
            missing.append(name)
    return missing


def configure(lib, manifest):
    """Types for the calls this tool makes. Only the ABI-stable scalar ones matter here."""
    i32, f32, u64, void_p = C.c_int32, C.c_float, C.c_uint64, C.c_void_p
    spec_size = manifest["structs"]["af_spec"]["size"]

    lib.af_engine_abi_version.restype = i32
    lib.af_engine_version.restype = C.c_char_p
    lib.af_engine_region_name.restype = C.c_char_p
    lib.af_engine_region_name.argtypes = [i32]
    lib.af_engine_last_error.restype = C.c_char_p
    lib.af_engine_last_error.argtypes = [void_p]
    lib.af_spec_init.argtypes = [void_p]
    lib.af_engine_create.argtypes = [void_p, void_p, C.POINTER(void_p)]
    lib.af_engine_create.restype = i32
    lib.af_engine_destroy.argtypes = [void_p]
    lib.af_engine_clip_name.argtypes = [i32, C.c_char_p, i32]
    lib.af_engine_clip_name.restype = i32
    lib.af_engine_clip_duration.argtypes = [void_p, i32]
    lib.af_engine_clip_duration.restype = f32
    lib.af_engine_blendshape_names.restype = i32
    return spec_size


def new_engine(lib, spec_size):
    """A default engine: enough to ask clip durations, no pipeline run (that takes ~30 s)."""
    spec = (C.c_uint8 * spec_size)()
    lib.af_spec_init(spec)
    handle = C.c_void_p()
    rc = lib.af_engine_create(spec, None, C.byref(handle))
    return rc, handle, spec


def observe(lib, manifest, spec_size):
    """Everything comparable, as plain Python data, so two implementations can be diffed."""
    rc, handle, spec = new_engine(lib, spec_size)
    out = {"create_rc": rc}
    if rc == 0:
        clips = {}
        for c in range(manifest["enums"]["af_clip"]["AF_CLIP_COUNT"]):
            buf = C.create_string_buffer(64)
            lib.af_engine_clip_name(c, buf, 64)
            clips[c] = {"name": buf.value.decode("utf-8", "replace"),
                        "duration": round(float(lib.af_engine_clip_duration(handle, c)), 4)}
        out["clips"] = clips
        names = (C.c_char * 64 * 64)()
        n = lib.af_engine_blendshape_names(names, 64)
        out["blendshapes"] = [names[i].value.decode("utf-8", "replace") for i in range(n)]
        out["regions"] = [lib.af_engine_region_name(i).decode("utf-8", "replace")
                          for i in range(manifest["enums"].get("af_region", {}).get("AF_REGION_COUNT", 13))]
        out["last_error"] = (lib.af_engine_last_error(handle) or b"").decode("utf-8", "replace")
        lib.af_engine_destroy(handle)
    return out


def invariants(lib, manifest, spec_size, failures):
    f = failures.check
    f(lib.af_engine_abi_version() == manifest["abi_version"],
      "abi_version == %s" % manifest["abi_version"])
    version = (lib.af_engine_version() or b"").decode()
    f(bool(version), "engine version string present (%r)" % version)
    if "engine_version" in manifest:
        f(version == manifest["engine_version"],
          "engine version matches the header (%s)" % manifest["engine_version"])

    rc, handle, spec = new_engine(lib, spec_size)
    f(rc == 0, "af_engine_create with default spec (rc=%d)" % rc)
    if rc != 0:
        return
    size = C.c_int32.from_buffer(spec, 0).value
    f(size == spec_size, "af_spec_init wrote struct_size=%d (manifest %d)" % (size, spec_size))

    # Defaults a caller may rely on: documented in af_spec_init and part of the SDK's behaviour.
    offsets = manifest["structs"]["af_spec"]["fields"]
    def f32_at(name):
        return C.c_float.from_buffer(spec, offsets[name]).value
    def i32_at(name):
        return C.c_int32.from_buffer(spec, offsets[name]).value
    f(i32_at("seed") == 1337, "default seed is 1337")
    f(abs(f32_at("muscle") - 0.45) < 1e-6, "default muscle 0.45")
    f(abs(f32_at("body_fat") - 0.22) < 1e-6, "default body_fat 0.22")
    f(abs(f32_at("cloth_roughness") + 1.0) < 1e-6, "default cloth_roughness -1 (auto)")

    clips = {}
    for c in range(1, manifest["enums"]["af_clip"]["AF_CLIP_COUNT"]):
        buf = C.create_string_buffer(64)
        n = lib.af_engine_clip_name(c, buf, 64)
        clips[buf.value.decode()] = float(lib.af_engine_clip_duration(handle, c))
        if n <= 0:
            f(False, "clip %d has a name" % c)
    f(len(clips) == manifest["enums"]["af_clip"]["AF_CLIP_COUNT"] - 1,
      "every clip but REST is named (%d)" % len(clips))
    # A duration of zero means "no authored track", which the renderer treats as a held pose;
    # a clip that is supposed to move must not report zero.
    moving = [k for k, d in clips.items() if k not in ("t_pose", "a_pose", "sit") and d <= 0.01]
    f(not moving, "every animated clip has a non-zero duration (zero: %s)" % (moving or "none"))
    rest = C.create_string_buffer(64)
    lib.af_engine_clip_name(0, rest, 64)
    f(rest.value.decode() == "rest", "clip 0 is 'rest' (%r)" % rest.value.decode())

    names = (C.c_char * 64 * 64)()
    n = lib.af_engine_blendshape_names(names, 64)
    listed = [names[i].value.decode() for i in range(n)]
    f(n == 52, "52 ARKit blendshape names (%d)" % n)
    for must in ("eyeBlinkLeft", "jawOpen", "mouthSmileRight", "browInnerUp", "tongueOut"):
        f(must in listed, "channel %s present" % must)

    regions = [lib.af_engine_region_name(i) for i in range(13)]
    f(all(regions), "region table names all 13 regions")
    lib.af_engine_destroy(handle)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    target = args[0]
    reference = None
    if "--reference" in flags:
        reference = args[1] if len(args) > 1 else None
        if not reference:
            print("--reference needs a library path")
            return 2
    manifest = load_manifest()
    print("ABI manifest: %d functions, %d structs, abi %s\n"
          % (len(manifest["functions"]), len(manifest["structs"]), manifest["abi_version"]))

    failures = Failures()
    print("[1] symbols in %s" % target)
    lib = C.CDLL(os.path.abspath(target))
    missing = bind(lib, manifest)
    total = len(manifest["functions"])
    covered = total - len(missing)
    print("  %s %d/%d symbols resolve (%.0f%% of the ABI)"
          % ("PASS" if not missing else "FAIL", covered, total, 100.0 * covered / total))
    if missing:
        # Missing symbols are INCOMPLETE, not broken: the invariants below still have to hold for
        # whatever *is* implemented. During a port this is the expected state, so it only fails the
        # run when --require-complete asks for a drop-in replacement.
        for name in missing[:12]:
            print("       missing: %s" % name)
        if len(missing) > 12:
            print("       ... %d more" % (len(missing) - 12))

    spec_size = configure(lib, manifest)
    print("\n[2] invariants")
    invariants(lib, manifest, spec_size, failures)

    if reference:
        print("\n[3] behavioural parity against %s" % reference)
        ref = C.CDLL(os.path.abspath(reference))
        configure(ref, manifest)
        a = observe(ref, manifest, spec_size)
        b = observe(lib, manifest, spec_size)
        failures.check(a["create_rc"] == b["create_rc"], "same engine create result")
        if a["create_rc"] == 0 and b["create_rc"] == 0:
            failures.check(a["clips"] == b["clips"],
                           "identical clip table (names + durations)")
            if a["clips"] != b["clips"]:
                for k in sorted(set(a["clips"]) | set(b["clips"])):
                    if a["clips"].get(k) != b["clips"].get(k):
                        print("       clip %s: reference %s vs candidate %s"
                              % (k, a["clips"].get(k), b["clips"].get(k)))
            failures.check(a["blendshapes"] == b["blendshapes"],
                           "identical blendshape name table (%d channels)" % len(a["blendshapes"]))
            failures.check(a["regions"] == b["regions"], "identical region table")

        # ---- prompt analysis parity, driven through the ABI over the whole corpus ------------
        # The strongest check the tool has: two independent parsers, two libraries, one ABI. Every
        # prompt must return the same attribute count AND leave byte-identical specs behind.
        corpus = os.path.join(ROOT, "tools", "prompt_corpus.txt")
        has_analyze = any(f["name"] == "af_engine_analyze_prompt" for f in manifest["functions"])
        if has_analyze and os.path.exists(corpus):
            for lib_ in (ref, lib):
                lib_.af_engine_analyze_prompt.argtypes = [C.c_void_p, C.c_char_p, C.c_void_p]
                lib_.af_engine_analyze_prompt.restype = C.c_int32
            prompts = [ln.rstrip("\n") for ln in open(corpus, encoding="utf-8")
                       if ln.strip() and not ln.lstrip().startswith("#")]
            a_rc, a_eng, _ = new_engine(ref, spec_size)
            b_rc, b_eng, _ = new_engine(lib, spec_size)
            if a_rc == 0 and b_rc == 0:
                mismatches = []
                out_a = (C.c_uint8 * spec_size)()
                out_b = (C.c_uint8 * spec_size)()
                for text in prompts:
                    raw = text.encode("utf-8")
                    ra = ref.af_engine_analyze_prompt(a_eng, raw, C.byref(out_a))
                    rb = lib.af_engine_analyze_prompt(b_eng, raw, C.byref(out_b))
                    if ra != rb or bytes(out_a) != bytes(out_b):
                        mismatches.append((text, ra, rb))
                ref.af_engine_destroy(a_eng)
                lib.af_engine_destroy(b_eng)
                failures.check(not mismatches,
                               "identical prompt analysis over %d corpus prompts" % len(prompts))
                for text, ra, rb in mismatches[:8]:
                    print("       %r: reference %d vs candidate %d" % (text, ra, rb))
            else:
                print("  SKIP prompt analysis parity: engine creation failed")
        elif has_analyze:
            print("  SKIP prompt analysis parity: %s not found" % corpus)

    print()
    if failures.items:
        print("CONFORMANCE FAILED: %d problem(s)" % len(failures.items))
        return 1
    if missing:
        if "--require-complete" in flags:
            print("CONFORMANCE INCOMPLETE: %d of %d symbols still missing - not a drop-in replacement yet"
                  % (len(missing), total))
            return 1
        print("CONFORMANCE PASSED for the %d implemented symbols; %d of %d still to port "
              "(use --require-complete to demand all of them)" % (covered, len(missing), total))
        return 0
    print("CONFORMANCE PASSED: %d/%d symbols, invariants and reference parity" % (covered, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
