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
#     libafrender.so     the GLES 3.1 renderer. Links libEGL/libGLESv3/libandroid/liblog;
#                        no Vulkan loader or alternate rendering framework is used.
#
#     libavatarforge.so  the JNI bridge, which is the only place Kotlin meets C++.
#
#  The Gradle app module points at this file through externalNativeBuild, and the desktop test
#  harness in native/tests/ compiles the same kernel sources with a host compiler (see BUILD.md).
#
#  The checked-in af_shaders_gl.h needs neither Python nor glslc for a normal build.
#  regenerate_shaders re-embeds the text; AF_GLSLC enables offline compiler validation.
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
# -O1: clang's -O3 on the kernel and renderer needs well over a gigabyte of RAM per
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
#  Renderer
# ----------------------------------------------------------------------------------------------
set(AF_RENDER_SOURCES
    src/render/af_render.cpp
    src/render/af_render_capi.cpp

    src/render/af_gl.cpp
    src/render/af_shaders.cpp)

add_library(afrender SHARED ${AF_RENDER_SOURCES})
target_include_directories(afrender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
target_compile_options(afrender PRIVATE ${AF_OPT_FLAGS} -Wall -Wextra)
target_link_libraries(afrender
    PUBLIC afcore
    PRIVATE EGL GLESv3 android log)

# ----------------------------------------------------------------------------------------------
#  JNI bridge
# ----------------------------------------------------------------------------------------------
add_library(avatarforge SHARED src/android/af_jni.cpp)
target_include_directories(avatarforge
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(avatarforge PRIVATE ${AF_OPT_FLAGS} -Wall -Wextra)
target_link_libraries(avatarforge PRIVATE afcore afrender android log)

# ----------------------------------------------------------------------------------------------
#  Optional: re-embed GLSL ES (Python 3; AF_GLSLC enables compiler validation)
# ----------------------------------------------------------------------------------------------
find_program(AF_PYTHON NAMES python3 python)
if(AF_PYTHON)
    add_custom_target(regenerate_shaders
        COMMAND ${AF_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_shaders.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        COMMENT "Re-embedding GLSL ES -> af_shaders_gl.h")
endif()
