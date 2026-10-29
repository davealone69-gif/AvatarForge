#!/usr/bin/env python3
# ==============================================================================================
#  AvatarForge :: tools/embed_shaders.py
# ==============================================================================================
#
#  Turns native/src/render/shaders/*.vert|frag into the GLES shader table consumed by GlRenderer:
#
#      af_shaders_gl.h   "#version 310 es" GLSL source text
#
#  The single shipped backend is OpenGL ES 3.1. glslc is used only as an optional offline syntax
#  checker with the OpenGL target; no SPIR-V or Vulkan artifacts are generated.
#
#  Usage:  python3 tools/embed_shaders.py [--check]
#            --check   compile everything, report, but do not write the headers
#
#  The headers are checked in, so a normal APK build does not need glslc at all: the CMake target
#  regenerates them only when a shader source or this script is newer.
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHADER_DIR = os.path.join(ROOT, "native", "src", "render", "shaders")
OUT_DIR = os.path.join(ROOT, "native", "src", "render")

# Every (name, stage) the backends ask for. Missing entries are a hard failure: a pipeline that
# cannot find its shader must fail the frame, never fall back to an unlit path.
REQUIRED = {
    ("forward", "vert"), ("forward", "frag"),
    ("shadow", "vert"), ("shadow", "frag"),
    ("ground", "vert"), ("ground", "frag"),
    ("line", "vert"), ("line", "frag"),
    ("fullscreen", "vert"),
    ("bloom_down", "frag"), ("bloom_up", "frag"), ("composite", "frag"),
    ("forward_plus", "comp"),
}

GLSL_STAGE = {"vert": "vert", "frag": "frag", "comp": "comp"}
STAGE_ENUM = {"vert": "AF_STAGE_VERT", "frag": "AF_STAGE_FRAG", "comp": "AF_STAGE_COMP"}


def find_glslc():
    """Locates glslc: ANDROID_NDK first (it ships one), then PATH, then the usual install spots."""
    env = os.environ.get("ANDROID_NDK") or os.environ.get("ANDROID_NDK_HOME")
    if env:
        cand = os.path.join(env, "shader-tools", "linux-x86_64", "glslc")
        if os.path.isfile(cand):
            return cand
    found = shutil.which("glslc")
    if found:
        return found
    for cand in ("/usr/local/tc/ndk/shader-tools/linux-x86_64/glslc",
                 "/opt/android-ndk/shader-tools/linux-x86_64/glslc"):
        if os.path.isfile(cand):
            return cand
    return None


def inline_includes(path, seen=None):
    """Inlines #include "..." recursively, so both compilers see one self-contained translation
    unit and neither needs include-directory plumbing."""
    seen = seen or []
    src = open(path, "r", encoding="utf-8").read()
    out = []
    for line in src.splitlines(True):
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if not m:
            out.append(line)
            continue
        inc = os.path.join(os.path.dirname(path), m.group(1))
        if inc in seen:
            out.append("/* %s already included */\n" % m.group(1))
            continue
        out.append(inline_includes(inc, seen + [path]))
    return "".join(out)


def gl_source(stage_path, stage):
    body = inline_includes(stage_path)
    head = "#version 310 es\n"
    head += "#define AF_GLES 1\n"
    if stage == "vert":
        head += "#define AF_STAGE_VERTEX 1\n"
    head += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
    return head + body


def check_gles(glslc, glsl, stage, tmpdir, name):
    """
    Type-checks the GLES dialect of the same source.

    The GLES path is normally compiled by the phone's driver at runtime, which means a syntax error
    would only surface on a device - the worst possible place to find one. glslc's OpenGL target
    parses "#version 310 es" with GL semantics, so running it here turns "it compiles on the
    driver" into "it compiles on this machine, now".
    """
    src = os.path.join(tmpdir, "%s.gles.%s" % (name, stage))
    open(src, "w", encoding="utf-8").write(glsl)
    r = subprocess.run([glslc, "-fshader-stage=" + GLSL_STAGE[stage], "--target-env=opengl",
                        "-o", os.devnull, src], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("GLES dialect check failed on %s (%s)" % (name, stage))


def describe(name, stage, size):
    print("  %-12s %-4s  %d bytes of GLSL ES" % (name, stage, size))


def describe(name, stage, words, size=None):
    extra = "%d words" % len(words) if words is not None else "%d bytes of GLSL ES" % size
    print("  %-12s %-4s  %s" % (name, stage, extra))


def c_string(text):
    """Emits a C string literal, one source line per shader line (so compiler errors from the GL
    driver still point at something readable)."""
    out = []
    for line in text.splitlines(True):
        esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
        esc = esc.replace("\r", "").replace("\n", "\\n")
        out.append('    "%s"\n' % esc)
    return "".join(out)


def emit_gl(entries, path):
    L = []
    L.append("/*\n * af_shaders_gl.h -- GENERATED by tools/embed_shaders.py. Do not edit.\n"
             " *\n"
             " * GLES 3.10 sources for the sole OpenGL ES 3.1 backend.\n"
             " */\n")
    L.append("#ifndef AF_SHADERS_GL_H\n#define AF_SHADERS_GL_H\n\n")
    L.append('#include "af_shaders.h"\n\n')
    L.append("namespace af {\nnamespace shaders_gl {\n\n")
    for (name, stage), source in entries:
        L.append("static const char kSrc_%s_%s[] =\n" % (name, stage))
        L.append(c_string(source))
        L.append(";\n\n")
    L.append("static const AfShaderGl kTable[] = {\n")
    for (name, stage), _ in entries:
        L.append('    {"%s", %s, kSrc_%s_%s},\n'
                 % (name, STAGE_ENUM[stage], name, stage))
    L.append("};\n\n")
    L.append("} /* namespace shaders_gl */\n} /* namespace af */\n\n#endif\n")
    open(path, "w", encoding="utf-8").write("".join(L))


def main():
    check = "--check" in sys.argv
    glslc = find_glslc()

    stages = []
    for fn in sorted(os.listdir(SHADER_DIR)):
        base, ext = os.path.splitext(fn)
        if ext in (".vert", ".frag", ".comp"):
            stages.append((base, ext[1:], os.path.join(SHADER_DIR, fn)))

    missing = REQUIRED - {(n, s) for n, s, _ in stages}
    if missing:
        raise SystemExit("missing shader sources for: %s" % sorted(missing))

    print("AvatarForge shader embed: %d stages, glslc=%s" % (len(stages), glslc or "NOT FOUND"))

    gl_entries = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, stage, path in stages:
            gl = gl_source(path, stage)
            gl_entries.append(((name, stage), gl))
            if glslc:
                check_gles(glslc, gl, stage, tmp, name)
            describe(name, stage, len(gl))

    if check:
        print("--check: nothing written")
        return

    emit_gl(gl_entries, os.path.join(OUT_DIR, "af_shaders_gl.h"))
    print("wrote native/src/render/af_shaders_gl.h")


if __name__ == "__main__":
    main()
