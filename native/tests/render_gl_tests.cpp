/*
 * Execute the production GLES backend on Mesa, not a mock renderer. Only EGL's window creation
 * is replaced by a headless pbuffer; shaders, buffers, targets, draws and readback are real GLES.
 * Run via tools/test_renderer.sh. No Android SDK, display server or physical GPU is needed.
 */
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <android/native_window.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int g_alignment = 0;
static std::vector<unsigned char> g_pixels;
static int g_width = 0, g_height = 0;
static bool g_failNextSwap = false;
static EGLint g_injectedEglError = EGL_SUCCESS;

static EGLBoolean choosePbufferConfig(EGLDisplay display, const EGLint* attrs, EGLConfig* configs,
                                      EGLint size, EGLint* count) {
    std::vector<EGLint> pbufferAttrs;
    for (int i = 0; attrs[i] != EGL_NONE; i += 2) {
        pbufferAttrs.push_back(attrs[i]);
        pbufferAttrs.push_back(attrs[i] == EGL_SURFACE_TYPE ? EGL_PBUFFER_BIT : attrs[i + 1]);
    }
    pbufferAttrs.push_back(EGL_NONE);
    return eglChooseConfig(display, pbufferAttrs.data(), configs, size, count);
}

static EGLSurface createPbufferWindow(EGLDisplay display, EGLConfig config, ANativeWindow* window,
                                      const EGLint*) {
    g_width = window->width;
    g_height = window->height;
    const EGLint attrs[] = {EGL_WIDTH, g_width, EGL_HEIGHT, g_height, EGL_NONE};
    return eglCreatePbufferSurface(display, config, attrs);
}

static EGLBoolean captureAndSwap(EGLDisplay display, EGLSurface surface) {
    if (g_failNextSwap) {
        g_failNextSwap = false;
        g_injectedEglError = EGL_BAD_ALLOC; // Transient presentation refusal, not context loss.
        return EGL_FALSE;
    }
    g_pixels.resize(size_t(g_width) * g_height * 4);
    glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
    return eglSwapBuffers(display, surface);
}

static EGLint queryEglError() {
    const EGLint injected = g_injectedEglError;
    g_injectedEglError = EGL_SUCCESS;
    return injected != EGL_SUCCESS ? injected : eglGetError();
}

static void queryInteger(GLenum name, GLint* value) {
    glGetIntegerv(name, value);
    // Mesa commonly reports 16. Emulate larger legal alignments to cover mobile drivers too.
    if (name == GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT && g_alignment > 0) {
        if (g_alignment < *value || g_alignment % *value != 0) {
            std::fprintf(stderr, "test alignment %d is incompatible with device alignment %d\n",
                         g_alignment, *value);
            std::exit(2);
        }
        *value = g_alignment;
    }
}

#define eglGetError queryEglError
#define eglChooseConfig choosePbufferConfig
#define eglCreateWindowSurface createPbufferWindow
#define eglSwapBuffers captureAndSwap
#define glGetIntegerv queryInteger
#include "render_test_profile.h"
#define makeGpuProfile rendererTestProfile
#include "../src/render/af_gl.cpp"
#undef makeGpuProfile
#undef glGetIntegerv
#undef eglSwapBuffers
#undef eglCreateWindowSurface
#undef eglChooseConfig
#undef eglGetError

static int g_checks = 0, g_failures = 0;
#define CHECK(condition, message) do { ++g_checks; if (!(condition)) { \
    ++g_failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); } } while (0)

static bool noGlErrors(const char* stage) {
    bool ok = true;
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR;) {
        std::fprintf(stderr, "GLES error 0x%04x after %s\n", unsigned(error), stage);
        ok = false;
    }
    CHECK(ok, stage);
    return ok;
}

#include "render_test_fixture.h"

static size_t redPixels() {
    size_t red = 0;
    for (size_t i = 0; i + 3 < g_pixels.size(); i += 4)
        if (g_pixels[i] > 50 && g_pixels[i] > g_pixels[i + 1] * 1.5 &&
            g_pixels[i] > g_pixels[i + 2] * 1.5) ++red;
    return red;
}

int main(int argc, char** argv) {
    g_alignment = argc > 1 ? std::atoi(argv[1]) : 0;
    ANativeWindow window{kRenderTestWidth, kRenderTestHeight};
    af::RendererOptions options;
    options.msaaSamples = 2;
    options.shadowMapSize = 64;
    options.enableBloom = false;
    options.groundPlane = false;
    std::string error;
    std::unique_ptr<af::Renderer> renderer(af::createGlRenderer(&window, options, &error));
    if (!renderer) {
        std::fprintf(stderr, "GLES initialization failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("GLES: %s; emulated UBO alignment=%d; profile=%s\n",
                renderer->info().deviceName.c_str(), g_alignment, renderer->info().gpuProfile.c_str());
    // Do not issue draws with invalid VAOs on the unfixed code: some drivers dereference them.
    if (!noGlErrors("renderer initialization")) return 1;

    renderer->renderFrame(1.0f / 60.0f);
    noGlErrors("empty scene frame");
    CHECK(renderer->stats().fps > 0, "an empty scene still produces live frames");

    af_spec spec;
    af_spec_init(&spec);
    af_engine* rawEngine = nullptr;
    CHECK(af_engine_create(&spec, nullptr, &rawEngine) == AF_OK, "create engine result");
    std::unique_ptr<af_engine, decltype(&af_engine_destroy)> engine(rawEngine, af_engine_destroy);
    CHECK(engine != nullptr, "create engine");
    const auto glb = triangleGlb();
    CHECK(af_engine_import_glb_from_memory(engine.get(), glb.data(), glb.size()) == AF_OK,
          "import triangle GLB fixture");
    renderer->attachEngine(engine.get());
    noGlErrors("attach engine");

    for (int frame = 0; frame < 4; ++frame) {
        af::CameraState camera;
        camera.yawDeg += float(frame) * 3.0f;
        renderer->setCamera(camera);
        renderer->renderFrame(1.0f / 60.0f);
        noGlErrors("alternating camera slices / frame");
        CHECK(redPixels() > 200, "visible triangle pixels, not just a clear colour or FPS counter");
    }
    CHECK(renderer->stats().triangles == 1, "real mesh submitted");
    CHECK(renderer->stats().fps > 0, "live frame timing");
    CHECK(!std::getenv("AF_RENDER_TEST_FULL_PROFILE") || renderer->stats().msaaSamples > 1,
          "full coverage mode really exercised multisample targets");

    const int acceptedFrames = renderer->stats().frames;
    g_failNextSwap = true;
    renderer->renderFrame(1.0f / 60.0f);
    CHECK(renderer->stats().frames == acceptedFrames, "failed swap does not publish a fake new frame");
    renderer->renderFrame(1.0f / 60.0f);
    noGlErrors("recovery after a transient swap failure");
    CHECK(renderer->stats().frames != acceptedFrames, "successful swap advances the frame sequence");

    const auto withoutGround = g_pixels;
    options.groundPlane = true;
    renderer->setOptions(options);
    renderer->renderFrame(1.0f / 60.0f);
    noGlErrors("ground pass");
    CHECK(g_pixels != withoutGround, "ground VAO produces visible pixels");

    options.enableBloom = true;
    options.bloomLevels = 5;
    renderer->setOptions(options);
    renderer->renderFrame(1.0f / 60.0f);
    noGlErrors("bloom / post slices");
    CHECK(redPixels() > 200, "geometry survives target rebuild");
    CHECK(!std::getenv("AF_RENDER_TEST_FULL_PROFILE") || renderer->stats().bloomLevels == 5,
          "full coverage mode really exercised five bloom levels");

    CHECK(renderer->resize(96, 160), "resize rebuilds targets");
    renderer->renderFrame(1.0f / 60.0f); // EGL reports the original pbuffer dimensions: resize back.
    noGlErrors("resize recovery");
    CHECK(redPixels() > 200, "geometry survives resize");

    options.enableShadows = false;
    options.enableBloom = false;
    options.msaaSamples = 1;
    renderer->setOptions(options);
    renderer->invalidateMesh();
    renderer->renderFrame(1.0f / 60.0f);
    noGlErrors("single-sample / shadows disabled");
    CHECK(redPixels() > 200, "geometry survives quality downgrade");
    for (int size : {4, 0, 1, 0}) {
        renderer->waitIdle();
        const auto replacement = triangleGlb(size);
        CHECK(af_engine_import_glb_from_memory(engine.get(), replacement.data(), replacement.size()) == AF_OK,
              "import replacement texture fixture");
        if (size > 0) {
            af_texture texture{};
            CHECK(af_engine_texture(engine.get(), AF_TEX_BASE_COLOR, &texture) == AF_OK &&
                  texture.width == size && texture.height == size,
                  "fixture has a real embedded base-colour texture");
        }
        renderer->attachEngine(engine.get());
        renderer->renderFrame(1.0f / 60.0f);
        noGlErrors("real / 1x1 / absent texture reattachment");
        const size_t visible = redPixels();
        std::printf("texture reattach: size=%d, visible red pixels=%zu\n", size, visible);
        CHECK(visible > 200, "texture replacement keeps opaque geometry visible");
    }
    renderer.reset(); // The engine must outlive the renderer.
    std::printf("GLES renderer: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
