/* Host regression tests for the actual GLES backend and backend selector.
 * GLES/EGL are stateful test doubles: these tests prove API sequencing, error reporting and
 * cleanup, NOT GPU compatibility or visible rendering. A physical-device pass is still required.
 */
#include "gl_mock.h"
#include "af_render.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include "../../src/render/af_render_capi.cpp"

namespace af {
Renderer* createGlRenderer(ANativeWindow*, const RendererOptions&, std::string*);
}
/* Selector-only test double; the Vulkan command path is NOT mocked by this suite. */
namespace af_test_vk { extern int attempts, destroys; extern bool initSucceeds; void reset(); }
namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
ANativeWindow* window() { return reinterpret_cast<ANativeWindow*>(uintptr_t(10)); }
af::RendererOptions options() {
    af::RendererOptions o{};
    o.preferVulkan = false;  // Most tests exercise GLES directly; Vulkan has a separate explicit case.
    o.msaaSamples = 2;
    return o;
}
bool hasExtent(const std::map<GLuint, std::pair<GLsizei, GLsizei>>& sizes, int width, int height) {
    for (const auto& entry : sizes) {
        if (entry.second.first == width && entry.second.second == height) return true;
    }
    return false;
}
bool resourcesReleased() {
    const auto& s = af_test::state;
    return s.livePrograms == 0 && s.liveShaders == 0 && s.liveBuffers == 0 && s.liveTextures == 0 &&
           s.liveQueries == 0 &&
           s.liveFramebuffers == 0 && s.liveRenderbuffers == 0 && s.liveVaos == 0 &&
           s.contextsCreated == s.contextsDestroyed && s.surfacesCreated == s.surfacesDestroyed;
}
void successAndOwnership() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "GLES initializes without an engine attached");
    check(error.empty(), "successful initialization has no error");
    check(r && r->stats().frameMs < 0.0f && r->stats().fps < 0.0f && r->stats().cpuMs < 0.0f,
          "timing is explicitly unknown before a measured frame");
    check(af_test::state.glErrors.empty(), "no invalid sampler/VAO calls during initialization");
    check(af_test::state.legacyExtensionQueries == 0 && af_test::state.indexedExtensionQueries > 0,
          "ES 3.1 capabilities use indexed extensions, not glGetString(GL_EXTENSIONS)");
    check(contains(af_test::state.lastContextLog, "AF_GLES_CONTEXT backend=gles31"),
          "only a successfully created app GLES context is logged");
    check(af_test::state.samplerWrites.size() >= 10, "sampler uniforms are assigned to bound programs");
    check(af_test::state.liveVaos == 4, "all four VAOs exist");
    bool validAttributes = true, validAvatarElements = false;
    for (const auto& entry : af_test::state.vaos) {
        for (const auto& attr : entry.second.attributes) validAttributes &= attr.second.buffer != 0;
        if (entry.second.attributes.size() == 5) validAvatarElements = entry.second.elementBuffer != 0;
    }
    check(validAttributes, "every configured vertex attribute references allocated storage");
    check(validAvatarElements, "avatar VAO captures a non-zero element buffer");
    if (r) {
        r->attachEngine(reinterpret_cast<af_engine*>(uintptr_t(20)));
        r->renderFrame(0.0f);
        check(r->stats().frameMs < 0.0f && r->stats().fps < 0.0f,
              "zero first delta remains explicitly unmeasured");
        r->renderFrame(1.0f / 60.0f);
        check(std::fabs(r->stats().frameMs - (1000.0f / 60.0f)) < 0.02f &&
              std::fabs(r->stats().fps - 60.0f) < 0.1f,
              "first valid sample seeds frame time instead of reporting an inflated FPS");
        const float expectedEma = (1000.0f / 60.0f) * 0.94f + (1000.0f / 30.0f) * 0.06f;
        r->renderFrame(1.0f / 30.0f);
        check(std::fabs(r->stats().frameMs - expectedEma) < 0.02f &&
              std::fabs(r->stats().fps - 1000.0f / expectedEma) < 0.1f,
              "second valid sample follows the documented 0.06 EMA");
        check(r->stats().triangles == 1, "attached fixture reaches the real draw path");
    check(r->stats().textureBytes > 0, "uploaded atlas texture memory is reported, not zero");
        check(af_test::state.anisotropyWrites == AF_TEX_COUNT && af_test::state.lastAnisotropy == 4.0f,
              "atlas enables supported 4x anisotropy for oblique textures during clips");
        check(af_test::state.legacyExtensionQueries == 0,
              "atlas upload does not make an invalid legacy extension query on ES 3.1");
        check(r->stats().frames == 3 && r->stats().presentRequestsAccepted == 3,
              "frame counters advance only after accepted presents");
        check(r->stats().frameAttempts == 3 && r->stats().frameSubmissions == 3,
              "attempt and submission counters are distinct from accepted presents");
        check(r->frameError().empty(), "successful frames record no frame error");
        check(af_test::state.glErrors.empty(), "camera UBO slices stay in bounds across alternating frames");
        check(af_test::state.cameraBuffer != 0 &&
              af_test::state.bufferSizes[af_test::state.cameraBuffer] >= 1024,
              "two 448-byte camera slices are allocated with 256-byte alignment");
        bool drewWithElements = false;
        for (const auto& draw : af_test::state.elementDraws) drewWithElements |= draw.first != 0 && draw.second != 0;
        check(drewWithElements, "indexed draws use the VAO's element buffer");
    }
    r.reset();
    check(resourcesReleased(), "successful renderer teardown releases every tracked resource");
}
void glesGpuTimerQueries() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r && !r->stats().gpuTimingSupported && r->stats().gpuMs < 0.0f,
          "GLES without the timer extension reports GPU timing as unsupported and unknown");
    check(af_test::state.timerQueriesGenerated == 0,
          "GLES without the timer extension makes no timer-query allocations");
    r.reset();
    check(resourcesReleased(), "unsupported GLES timer path leaves renderer ownership clean");

    af_test::reset();
    af_test::state.extensions = "GL_EXT_disjoint_timer_query";
    af_test::state.timerFunctionsAvailable = false;
    r.reset(af::createGlRenderer(window(), options(), &error));
    check(r && !r->stats().gpuTimingSupported && r->stats().gpuMs < 0.0f,
          "advertised timer extension with missing entry points remains optional");
    check(af_test::state.timerQueriesGenerated == 0 && af_test::state.glErrors.empty(),
          "missing timer procedures neither allocate queries nor fail GLES initialization");
    r.reset();
    check(resourcesReleased(), "missing GLES timer procedures do not leak resources");

    af_test::reset();
    af_test::state.extensions = "GL_EXT_disjoint_timer_query";
    r.reset(af::createGlRenderer(window(), options(), &error));
    check(r && r->stats().gpuTimingSupported && af_test::state.timerQueriesGenerated == 3,
          "supported GLES context allocates a bounded three-query timing ring");
    if (r) {
        r->renderFrame(1.0f / 60.0f);
        check(r->stats().gpuMs < 0.0f && af_test::state.timerQueryValueReads == 0,
              "pending GLES timer results remain unknown without blocking for a value");
        check(af_test::state.timerQueryBeginCalls == 1 && af_test::state.timerQueryEndCalls == 1 &&
              af_test::state.glFinishCalls == 0,
              "GLES GPU work is bracketed without glFinish synchronization");
        af_test::state.timerResultAvailable = true;
        r->renderFrame(1.0f / 60.0f);
        check(std::fabs(r->stats().gpuMs - 4.2f) < 0.001f &&
              af_test::state.timerQueryValueReads == 1,
              "completed nanosecond query is converted to finite milliseconds");
        af_test::state.timerResultAvailable = false;
        af_test::state.gpuDisjoint = true;
        const int beginsBeforeDisjoint = af_test::state.timerQueryBeginCalls;
        r->renderFrame(1.0f / 60.0f);
        check(r->stats().gpuMs < 0.0f &&
              af_test::state.timerQueryBeginCalls == beginsBeforeDisjoint,
              "GPU disjoint invalidates old timing and suppresses a query for that frame");
        af_test::state.gpuDisjoint = false;
        af_test::state.timerResultAvailable = true;
        r->renderFrame(1.0f / 60.0f);
        check(r->stats().gpuMs < 0.0f,
              "a completed result from a disjoint interval is discarded");
        r->renderFrame(1.0f / 60.0f);
        check(std::fabs(r->stats().gpuMs - 4.2f) < 0.001f && r->frameError().empty(),
              "GLES timing resumes only after a fresh valid query completes");
    }
    r.reset();
    check(af_test::state.liveQueries == 0 &&
          af_test::state.timerQueriesDeleted == af_test::state.timerQueriesGenerated,
          "GLES timer-query objects are deleted with the current context");
    check(resourcesReleased(), "timer-query teardown releases all renderer resources");
}
void swapFailureIsRecoverableByOwner() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "swap-failure fixture initializes");
    if (r) {
        af_test::state.fault = af_test::Fault::SwapBuffers;
        r->renderFrame(1.0f / 60.0f);
        check(!r->frameError().empty(), "failed eglSwapBuffers is exposed as a frame error");
        check(contains(r->frameError(), "eglSwapBuffers"), "frame error names the failed operation");
        check(r->stats().frameAttempts == 1, "failed present still counts as an attempt");
        check(r->stats().frames == 0 && r->stats().presentRequestsAccepted == 0,
              "failed present is not counted as a rendered frame");
    }
    r.reset();
    check(resourcesReleased(), "swap-failure teardown releases tracked resources");
}
void resizeAllocationFailureRestoresTargetsAndSignalsOwner() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "resize-failure fixture initializes");
    if (r) {
        const int framebuffersBefore = af_test::state.liveFramebuffers;
        const int renderbuffersBefore = af_test::state.liveRenderbuffers;
        const int texturesBefore = af_test::state.liveTextures;
        af_test::state.incompleteNextTextureFramebuffer = true;
        check(!r->resize(900, 1600), "failed target allocation returns resize failure to the owner");
        check(contains(r->frameError(), "GLES resize target allocation failed") &&
              contains(r->frameError(), "previous targets restored") &&
              contains(r->frameError(), "HDR colour framebuffer"),
              "resize failure carries the allocation cause and confirms target restoration");
        check(af_test::state.liveFramebuffers == framebuffersBefore &&
              af_test::state.liveRenderbuffers == renderbuffersBefore &&
              af_test::state.liveTextures == texturesBefore,
              "failed resize releases partial targets and restores the prior target set");
        r->renderFrame(1.0f / 60.0f);
        check(r->frameError().empty() && r->stats().frames == 1,
              "renderer can submit a frame with restored targets after reporting resize failure");
    }
    r.reset();
    check(resourcesReleased(), "resize-failure teardown releases every tracked resource");
}
void automaticResizeRetryUsesChangedEglExtent() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "automatic-resize retry fixture initializes");
    if (r) {
        check(hasExtent(af_test::state.textureSizes, 720, 1280) &&
              hasExtent(af_test::state.renderbufferSizes, 720, 1280),
              "initial targets use the mock EGL surface extent");
        const int drawsBefore = af_test::state.draws;
        const int framesBefore = r->stats().frames;
        const int submissionsBefore = r->stats().frameSubmissions;

        af_test::state.surfaceWidth = 900;
        af_test::state.surfaceHeight = 1600;
        af_test::state.incompleteNextTextureFramebuffer = true;
        r->renderFrame(1.0f / 60.0f);
        check(contains(r->frameError(), "GLES resize target allocation failed") &&
              contains(r->frameError(), "previous targets restored"),
              "first frame detects the changed EGL extent and restores targets after allocation failure");
        check(af_test::state.draws == drawsBefore && r->stats().frameSubmissions == submissionsBefore &&
              r->stats().frames == framesBefore,
              "failed resize does not draw or submit a frame against mismatched extents");
        check(hasExtent(af_test::state.textureSizes, 720, 1280) &&
              hasExtent(af_test::state.renderbufferSizes, 720, 1280) &&
              !hasExtent(af_test::state.textureSizes, 900, 1600) &&
              !hasExtent(af_test::state.renderbufferSizes, 900, 1600),
              "failed new-size allocations are discarded and old-size targets remain live");

        r->renderFrame(1.0f / 60.0f);
        check(r->frameError().empty() && r->stats().frames == framesBefore + 1 &&
              r->stats().frameSubmissions == submissionsBefore + 1,
              "next frame automatically retries the still-current EGL extent and recovers");
        check(af_test::state.draws > drawsBefore &&
              hasExtent(af_test::state.textureSizes, 900, 1600) &&
              hasExtent(af_test::state.renderbufferSizes, 900, 1600) &&
              !hasExtent(af_test::state.textureSizes, 720, 1280) &&
              !hasExtent(af_test::state.renderbufferSizes, 720, 1280),
              "recovered frame draws only after targets match the new EGL dimensions");
    }
    r.reset();
    check(resourcesReleased(), "automatic-resize retry teardown releases every tracked resource");
}
void resizeTargetRestoreFailurePreservesFatalDiagnostic() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "resize-restore-failure fixture initializes");
    if (r) {
        af_test::state.incompleteNextTextureFramebuffer = true;
        af_test::state.rejectNextMsaa = true;
        af_test::state.rejectSingleSample = true;
        check(!r->resize(900, 1600), "resize returns failure when old targets cannot be restored");
        check(contains(r->frameError(), "previous targets could not be restored") &&
              contains(r->frameError(), "single-sample render-target allocation"),
              "fatal resize failure reports both the original and restoration errors");
        r->renderFrame(1.0f / 60.0f);
        check(contains(r->frameError(), "previous targets could not be restored"),
              "fatal frame rejection preserves the detailed resize diagnostic");
        check(af_test::state.draws == 0,
              "renderer does not submit draws after target restoration fails");
    }
    r.reset();
    check(resourcesReleased(), "failed target restoration teardown releases every tracked resource");
}
void failureCase(af_test::Fault fault, const char* expected, const char* label, bool ext = false) {
    af_test::reset();
    af_test::state.fault = fault;
    af_test::state.rejectSingleSample = fault == af_test::Fault::MsaaStorage;
    af_test::state.rejectNextMsaa = fault == af_test::Fault::MsaaStorage;
    af_test::state.rejectFloatHdr = fault == af_test::Fault::HdrFramebuffer;
    if (ext) af_test::state.extensions = "GL_EXT_color_buffer_float";
    std::string error = "stale error must be replaced";
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r == nullptr, label);
    check(contains(error, expected), (std::string(label) + ": diagnostic contains " + expected + "; got " + error).c_str());
    check(resourcesReleased(), "failed initialization releases partial GPU resources");
}
void msaaFallback() {
    af_test::reset(); af_test::state.rejectNextMsaa = true;
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "unsupported MSAA count falls back to one sample");
    check(r && r->stats().msaaSamples == 1, "fallback reports the actual sample count");
    check(af_test::state.msaaAttempts == 4, "MSAA fallback retries storage exactly once");
    check(af_test::state.glErrors.empty(), "expected MSAA rejection is consumed before later error checks");
    r.reset(); check(resourcesReleased(), "MSAA fallback teardown is clean");
}
void hdrFallback() {
    af_test::reset();
    af_test::state.extensions = "GL_EXT_color_buffer_float";
    af_test::state.rejectFloatHdr = true;
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, ("advertised but unusable float attachments fall back to RGBA8: " + error).c_str());
    check(r && !r->info().supportsFloatHdr, "RGBA8 fallback is reported honestly");
    check(af_test::state.glErrors.empty(), "float rejection is consumed before the RGBA8 retry");
    r.reset();
    check(resourcesReleased(), "HDR fallback does not leak the rejected target set");
}
void capabilityTokens() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r && !r->info().supportsFloatHdr, "extension-name prefix does not falsely enable HDR");
    check(r && r->info().supportsAnisotropy, "complete anisotropy extension token is recognized");
    r.reset(); af_test::reset();
    af_test::state.extensions = "GL_EXT_color_buffer_float";
    r.reset(af::createGlRenderer(window(), options(), &error));
    check(r && r->info().supportsFloatHdr, "exact float colour extension enables HDR");
    check(r && !r->info().supportsAnisotropy, "missing anisotropy extension is reported as unsupported");
    if (r) r->attachEngine(reinterpret_cast<af_engine*>(uintptr_t(20)));
    check(af_test::state.anisotropyWrites == 0, "unsupported anisotropy is never requested at atlas upload");
}

void angleDriverIsStillGles() {
    af_test::reset();
    af_test::state.rendererName = "ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)";
    af_test::state.vendorName = "Samsung Electronics Co., Ltd.";
    af_test::state.version = "OpenGL ES 3.2 Driver version: 25.6.4";
    std::string error;
    std::unique_ptr<af::Renderer> r(af::Renderer::create(window(), options(), &error));
    check(r && r->info().backend == "gles31" && af_test::state.contextsCreated == 1 &&
          af_test_vk::attempts == 0,
          "ANGLE-on-Vulkan system driver does not select or initialize an app Vulkan backend");
    check(r && r->info().deviceName == af_test::state.rendererName &&
          r->info().apiVersion == af_test::state.version && r->info().driver == af_test::state.vendorName,
          "the app exposes the actual GL_RENDERER, GL_VERSION and GL_VENDOR from its own context");
    check(contains(af_test::state.lastContextLog, "backend=gles31 GL_RENDERER=\"ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)\""),
          "device report can distinguish GLES API from the system's ANGLE/Vulkan implementation");
    if (r) {
        r->attachEngine(reinterpret_cast<af_engine*>(uintptr_t(20)));
        r->setClip(3, 1.0f, true);
        r->renderFrame(1.0f / 60.0f);
    }
    check(r && r->stats().frames == 1 && r->frameError().empty(),
          "fixture submits an animated-avatar frame through the native GLES path");
    r.reset(); check(resourcesReleased(), "translated GLES context teardown releases resources");
}
/* A settings value is not a GPU allocation licence. These two cases used to reach glTexImage2D and
   glRenderbufferStorageMultisample unchanged, so a stale/corrupt preference could ask for a 16 GB
   shadow map and lose the context on a device that was rendering fine a frame earlier. */
void vulkanSelection() {
    af_test::reset(); af_test_vk::reset();
    af::RendererOptions request = options();
    request.preferVulkan = true;
    af_test_vk::initSucceeds = true;
    std::string error = "stale";
    std::unique_ptr<af::Renderer> r(af::Renderer::create(window(), request, &error));
    check(r && r->info().backend == "vulkan" && error.empty(),
          "explicit Vulkan choice reaches the separate native Vulkan factory");
    check(af_test_vk::attempts == 1 && af_test::state.contextsCreated == 0,
          "successful native Vulkan selection does not create an EGL/GLES context");
    r.reset();
    check(af_test_vk::destroys == 1, "native Vulkan backend is owned and destroyed with the wrapper");

    af_test::reset(); af_test_vk::reset(); error = "stale";
    r.reset(af::Renderer::create(window(), request, &error));
    check(af_test_vk::attempts == 1 && r && r->info().backend == "gles31" && error.empty() &&
          af_test::state.contextsCreated == 1,
          "failed Vulkan initialization falls back to a real GLES renderer with actual backend info");
    r.reset(); check(resourcesReleased(), "GLES fallback resources are released");

    af_test::reset(); af_test_vk::reset();
    af_test::state.fault = af_test::Fault::EglContext;
    r.reset(af::Renderer::create(window(), request, &error));
    check(!r && af_test_vk::attempts == 1 && contains(error, "Vulkan: vkCreateInstance") &&
          contains(error, "OpenGL ES 3.1") && contains(error, "EGL_BAD_CONFIG"),
          "failure of both APIs reports both exact initialization reasons");
    af_test::reset(); af_test_vk::reset();
}

void optionSanitisation() {
    af_test::reset();
    af::RendererOptions hostile = options();
    hostile.enableShadows = true;
    hostile.shadowMapSize = 65536;
    hostile.msaaSamples = 64;
    hostile.exposure = std::nanf("");
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), hostile, &error));
    check(r != nullptr, ("hostile option values still initialize: " + error).c_str());
    check(r && r->stats().shadowMapSize == 2048, "shadow map size is clamped to the 2048 budget");
    check(r && r->stats().msaaSamples >= 1 && r->stats().msaaSamples <= 4,
          "MSAA request is clamped to 4 samples");
    if (r) {
        af::RendererOptions live = options();
        live.enableShadows = true;
        live.shadowMapSize = 100000;
        live.enableBloom = true;
        live.bloomLevels = 99;
        r->setOptions(live);
        r->renderFrame(1.0f / 60.0f);
        r->renderFrame(1.0f / 60.0f);
        check(r->stats().shadowMapSize == 2048, "setOptions clamps a later shadow map request");
        check(r->stats().bloomLevels >= 0 && r->stats().bloomLevels <= 5,
              "setOptions clamps the bloom chain length");
        check(r->frameError().empty(), "clamped options still render a frame");
    }
    r.reset();
    check(resourcesReleased(), "clamped-option teardown releases tracked resources");
}

void renderAbi() {
    af_test::reset(); af_test_vk::reset(); af_test_vk::initSucceeds = false;
    af_render_options o{};
    af_render_options_defaults(&o);
    check(sizeof(o) == 64 && o.quality == 0 && o.msaa_samples == 4, "render options ABI remains layout-compatible");
    void* handle = af_renderer_create(window(), &o);
    check(handle != nullptr && af_test_vk::attempts == 1 && af_test::state.contextsCreated == 1,
          "C ABI defaults to Vulkan and falls back to GLES when Vulkan initialization fails");
    af_render_stats stats{};
    check(af_renderer_stats(handle, &stats) == AF_OK && sizeof(stats) == 72 && stats.gpu_ms == -1.0f &&
          stats.frame_ms < 0.0f && stats.fps < 0.0f && stats.cpu_ms < 0.0f,
          "C telemetry preserves unknown timing sentinels and aligned layout");
    af_renderer_frame(handle, 1.0f / 60.0f);
    check(af_renderer_stats(handle, &stats) == AF_OK && stats.frame_attempts == 1 &&
          stats.frame_submissions == 1 && stats.present_requests_accepted == 1,
          "C telemetry distinguishes attempts, submissions and accepted presents");
    check(stats.frame_ms > 0.0f && stats.fps > 0.0f && stats.cpu_ms >= 0.0f,
          "C telemetry becomes measured only after an accepted frame");
    af_renderer_destroy(handle);
}
void selector() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::Renderer::create(window(), options(), &error));
    check(r != nullptr && error.empty() && af_test::state.contextsCreated == 1,
          "factory initializes exactly one GLES context");
    r.reset(); af_test::reset();
    check(af::Renderer::create(nullptr, options(), &error) == nullptr && contains(error, "no native window"),
          "factory rejects a missing window");
    af_test::reset(); af_test::state.fault = af_test::Fault::EglContext;
    r.reset(af::Renderer::create(window(), options(), &error));
    check(r == nullptr && contains(error, "OpenGL ES 3.1") && contains(error, "EGL_BAD_CONFIG"),
          "GLES failure is reported directly without an alternate API");
    r.reset(); af_test::reset(); error = "stale";
    r.reset(af::Renderer::create(window(), options(), &error));
    check(r != nullptr && error.empty(), "fresh GLES attempt succeeds and clears stale error");
}

void defaultBackendSelection() {
    af_test::reset(); af_test_vk::reset(); af_test_vk::initSucceeds = true;
    std::string error;
    const af::RendererOptions defaults{};
    std::unique_ptr<af::Renderer> r(af::Renderer::create(window(), defaults, &error));
    check(defaults.preferVulkan && r && r->info().backend == "vulkan" && error.empty() &&
          af_test_vk::attempts == 1 && af_test::state.contextsCreated == 0,
          "native RendererOptions defaults to Vulkan without creating a GLES context");
    r.reset();
    af_test::reset(); af_test_vk::reset();
}
}  // namespace
int main() {
    successAndOwnership();
    glesGpuTimerQueries();
    failureCase(af_test::Fault::EglContext, "eglCreateContext(ES3): EGL_BAD_CONFIG (0x3005)", "EGL context failure");
    failureCase(af_test::Fault::FragmentCompile, "fragment shader forward: injected compile failure", "shader compile failure");
    failureCase(af_test::Fault::ProgramLink, "link forward/forward: injected link failure", "shader link failure");
    failureCase(af_test::Fault::VertexArray, "vertex array configuration: GL_INVALID_OPERATION (0x0502)", "vertex array failure");
    failureCase(af_test::Fault::MsaaStorage, "single-sample render-target allocation", "single-sample allocation failure");
    swapFailureIsRecoverableByOwner(); resizeAllocationFailureRestoresTargetsAndSignalsOwner();
    automaticResizeRetryUsesChangedEglExtent();
    resizeTargetRestoreFailurePreservesFatalDiagnostic();
    msaaFallback(); hdrFallback(); capabilityTokens(); angleDriverIsStillGles();
    selector(); defaultBackendSelection(); vulkanSelection(); optionSanitisation(); renderAbi();
    std::printf("Renderer host fault injection: %d checks, %d failure(s) (no physical GPU)\n", checks, failures);
    return failures ? 1 : 0;
}
