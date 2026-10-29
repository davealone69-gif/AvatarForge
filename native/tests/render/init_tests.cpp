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
    o.msaaSamples = 2;
    return o;
}
bool resourcesReleased() {
    const auto& s = af_test::state;
    return s.livePrograms == 0 && s.liveShaders == 0 && s.liveBuffers == 0 && s.liveTextures == 0 &&
           s.liveFramebuffers == 0 && s.liveRenderbuffers == 0 && s.liveVaos == 0 &&
           s.contextsCreated == s.contextsDestroyed && s.surfacesCreated == s.surfacesDestroyed;
}
void successAndOwnership() {
    af_test::reset();
    std::string error;
    std::unique_ptr<af::Renderer> r(af::createGlRenderer(window(), options(), &error));
    check(r != nullptr, "GLES initializes without an engine attached");
    check(error.empty(), "successful initialization has no error");
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
        r->renderFrame(1.0f / 60.0f);
        r->renderFrame(1.0f / 60.0f);
        r->renderFrame(1.0f / 60.0f);
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
    af_render_options o{};
    af_render_options_defaults(&o);
    check(sizeof(o) == 64 && o.quality == 0 && o.msaa_samples == 4, "GLES options ABI has no backend-choice field");
    void* handle = af_renderer_create(window(), &o);
    check(handle != nullptr, "C ABI creates the GLES renderer");
    af_render_stats stats{};
    check(af_renderer_stats(handle, &stats) == AF_OK && sizeof(stats) == 72 && stats.gpu_ms == -1.0f,
          "C telemetry preserves unknown GPU time and aligned layout");
    af_renderer_frame(handle, 1.0f / 60.0f);
    check(af_renderer_stats(handle, &stats) == AF_OK && stats.frame_attempts == 1 &&
          stats.frame_submissions == 1 && stats.present_requests_accepted == 1,
          "C telemetry distinguishes attempts, submissions and accepted presents");
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
}  // namespace
int main() {
    successAndOwnership();
    failureCase(af_test::Fault::EglContext, "eglCreateContext(ES3): EGL_BAD_CONFIG (0x3005)", "EGL context failure");
    failureCase(af_test::Fault::FragmentCompile, "fragment shader forward: injected compile failure", "shader compile failure");
    failureCase(af_test::Fault::ProgramLink, "link forward/forward: injected link failure", "shader link failure");
    failureCase(af_test::Fault::VertexArray, "vertex array configuration: GL_INVALID_OPERATION (0x0502)", "vertex array failure");
    failureCase(af_test::Fault::MsaaStorage, "single-sample render-target allocation", "single-sample allocation failure");
    swapFailureIsRecoverableByOwner();
    msaaFallback(); hdrFallback(); capabilityTokens(); angleDriverIsStillGles();
    selector(); vulkanSelection(); optionSanitisation(); renderAbi();
    std::printf("Renderer host fault injection: %d checks, %d failure(s) (no physical GPU)\n", checks, failures);
    return failures ? 1 : 0;
}
