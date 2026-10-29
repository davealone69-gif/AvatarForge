# ==============================================================================================
#  AvatarForge :: native/CMakeLists.txt
# ==============================================================================================
#
#  Builds the three native libraries the app ships:
#
#     libafcore.so       the kernel: anatomy -> SDF field -> isosurface -> UV -> texture bake ->
#                        rig -> blendshapes -> animation -> glTF export. No Android dependency
#                        beyond zlib and the C++ runtime, which is why the identical sources build
#                        and pass their test harness on a desktop.
#
#     libafrender.so     the custom renderer: OpenGL ES 3.1 only. Links EGL/GLES and Android system libraries; Vulkan is
#                        not compiled, linked, probed or shipped.
#
#     libavatarforge.so  the JNI bridge, which is the only place Kotlin meets C++.
#
#  The Gradle app module points at this file through externalNativeBuild, and the desktop test
#  harness in native/tests/ compiles the same kernel sources with a host compiler (see BUILD.md).
#
#  GLES shaders are checked in as GLSL ES text in af_shaders_gl.h, so a clean clone needs neither
#  Python nor glslc. The `regenerate_shaders` target updates the generated GLES shader table.
# ==============================================================================================

cmake_minimum_required(VERSION 3.22)
project(avatarforge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# -O3 with -ffast-math is deliberately NOT used for the kernel: the geometry pipeline relies on
# IEEE float semantics for its watertightness tests (signed-zero handling in the isosurface
# extractor), and fast-math is free to break exactly that.
# The optimisation level is a cache variable so a memory-constrained build machine can drop it to
# -O1: clang's -O3 on the kernel and the Vulkan backend needs well over a gigabyte of RAM per
# translation unit, and a machine with 2 GB total will be killed by the OOM reaper mid-build. The
# shipped configuration is -O3.
set(AF_OPT_FLAGS "-O3" CACHE STRING "Optimisation flags for the kernel and renderer")
set(AF_KERNEL_FLAGS ${AF_OPT_FLAGS} -fno-math-errno -fno-finite-math-only -ffp-contract=off)

# ----------------------------------------------------------------------------------------------
#  Kernel
# ----------------------------------------------------------------------------------------------
set(AF_KERNEL_SOURCES
    src/af_anatomy.cpp
    src/af_body.cpp
    src/af_engine.cpp
    src/af_field.cpp
    src/af_glb.cpp
    src/af_glb_read.cpp
    src/af_json.cpp
    src/af_image.cpp
    src/af_isosurface.cpp
    src/af_png.cpp
    src/af_png_decode.cpp
    src/af_prompt.cpp
    src/af_rig.cpp
    src/af_scene.cpp
    src/af_profiler.cpp
    src/af_spec.cpp
    src/af_texture.cpp
    src/af_uv.cpp)

add_library(afcore SHARED ${AF_KERNEL_SOURCES})
target_include_directories(afcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(afcore PRIVATE ${AF_KERNEL_FLAGS} -Wall -Wextra)
# zlib: on Android it is provided by the platform (ndk sysroot libz.so), on a desktop by -lz.
target_link_libraries(afcore PRIVATE z)
# The kernel is used from Java/Kotlin and from the Rust crates; a versioned soname would only get
# in the way, so it is fixed.
set_target_properties(afcore PROPERTIES VERSION "" SOVERSION "" PREFIX "lib")
# ----------------------------------------------------------------------------------------------
# Optional hostile-input fuzz target. Configure with Clang and
# -DAF_BUILD_GLB_FUZZ=ON; the importer, JSON parser, and PNG decoder are instrumented together.
# The regular Android build does not include or link this target.
# ----------------------------------------------------------------------------------------------
option(AF_BUILD_GLB_FUZZ "Build the GLB libFuzzer target (Clang only)" OFF)
if(AF_BUILD_GLB_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "AF_BUILD_GLB_FUZZ requires Clang/libFuzzer")
    endif()
    add_executable(af_glb_fuzz EXCLUDE_FROM_ALL
        tests/fuzz_glb_import.cpp
        src/af_glb_read.cpp
        src/af_json.cpp
        src/af_png_decode.cpp)
    target_include_directories(af_glb_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(af_glb_fuzz PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -Wall -Wextra)
    target_link_options(af_glb_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(af_glb_fuzz PRIVATE z)
    # Keep the parser/decoder in this executable so ASan/UBSan instrument the code under test.
endif()


# ----------------------------------------------------------------------------------------------
#  Renderer
# ----------------------------------------------------------------------------------------------
# Texture encoders are renderer-private: the public af_texture C ABI and exported GLB images stay
# raw. Build the ASTC core for the selected Android ABI without `native` ISA flags (those would
# target the build host when cross-compiling); ETC2 is the GLES 3.x / Vulkan fallback.
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_DIAGNOSTICS OFF CACHE BOOL "" FORCE)
set(ASTCENC_ASAN OFF CACHE BOOL "" FORCE)
set(ASTCENC_UBSAN OFF CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_INVARIANCE ON CACHE BOOL "" FORCE)
set(ASTCENC_BIG_ENDIAN OFF CACHE BOOL "" FORCE)
set(ASTCENC_BLOCK_MAX_TEXELS 216 CACHE STRING "" FORCE)
set(ASTCENC_ISA_NATIVE OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NONE OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE41 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_AVX2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SVE_128 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SVE_256 OFF CACHE BOOL "" FORCE)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(AF_ASTCENC_TARGET astcenc-neon-static)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(ASTCENC_ISA_SSE2 ON CACHE BOOL "" FORCE)
    set(AF_ASTCENC_TARGET astcenc-sse2-static)
else()
    # A generic scalar build is safe for older/less common NDK ABIs.
    set(ASTCENC_ISA_NONE ON CACHE BOOL "" FORCE)
    set(AF_ASTCENC_TARGET astcenc-none-static)
endif()
add_subdirectory(third_party/astcenc/Source)
set_target_properties(${AF_ASTCENC_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(third_party/etc2comp/EtcLib)
target_compile_features(EtcLib PRIVATE cxx_std_11)
set_target_properties(EtcLib PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(AF_RENDER_SOURCES
    src/render/af_render.cpp
    src/render/af_render_capi.cpp
    src/render/af_gl.cpp
    src/render/af_shaders.cpp
    src/render/af_avatar_culler.cpp
    src/render/af_avatar_lod.cpp
    src/render/af_gpu_profile.cpp
    src/render/af_texture_compression.cpp)

add_library(afrender SHARED ${AF_RENDER_SOURCES})
target_include_directories(afrender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/render
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/astcenc/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/etc2comp/EtcLib/Etc
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/etc2comp/EtcLib/EtcCodec)
target_compile_options(afrender PRIVATE ${AF_OPT_FLAGS} -Wall -Wextra)
target_link_libraries(afrender
    PUBLIC afcore
    PRIVATE EGL GLESv3 android log ${AF_ASTCENC_TARGET} EtcLib)

# ----------------------------------------------------------------------------------------------
#  JNI bridge
# ----------------------------------------------------------------------------------------------
add_library(avatarforge SHARED src/android/af_jni.cpp)
target_include_directories(avatarforge
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(avatarforge PRIVATE ${AF_OPT_FLAGS} -Wall -Wextra)
target_link_libraries(avatarforge PRIVATE afcore afrender android log)

# ----------------------------------------------------------------------------------------------
#  Optional: re-bake the shaders (needs Python 3 and glslc from the NDK's shader-tools)
# ----------------------------------------------------------------------------------------------
find_program(AF_PYTHON NAMES python3 python)
if(AF_PYTHON)
    add_custom_target(regenerate_shaders
        COMMAND ${AF_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_shaders.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        COMMENT "Re-baking GLSL -> SPIR-V (af_shaders_vk.h) and GLSL ES -> af_shaders_gl.h")
endif()
