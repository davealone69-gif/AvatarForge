/* Host regression tests for the actual GLES backend and backend selector.
 * GLES/EGL are stateful test doubles: these tests prove API sequencing, error reporting and
 * cleanup, NOT GPU compatibility or visible rendering. A physical-device pass is still required.
 */
#include "gl_mock.h"
#include "af_render.h"
#include <cstdio>
#include <memory>
#include <string>

namespace af {
Renderer* createGlRenderer(ANativeWindow*, const RendererOptions&, std::string*);
int vulkanAttempts = 0;
Renderer* createVulkanRenderer(ANativeWindow*, const RendererOptions&, std::string* error) {
    ++vulkanAttempts;
    if (error) *error = "injected Vulkan failure";
    return nullptr;
}
}
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
    o.preferVulkan = false;
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
}
void selector() {
    af_test::reset(); af::vulkanAttempts = 0;
    std::string error;
    std::unique_ptr<af::Renderer> r(af::Renderer::create(window(), options(), &error));
    check(r != nullptr && af::vulkanAttempts == 0, "explicit GLES choice does not attempt Vulkan");
    r.reset(); af_test::reset(); af::vulkanAttempts = 0;
    auto o = options(); o.preferVulkan = true;
    r.reset(af::Renderer::create(window(), o, &error));
    check(r != nullptr && af::vulkanAttempts == 1, "Vulkan failure still falls back to GLES");
    r.reset(); af_test::reset();
    af_test::state.fault = af_test::Fault::EglContext;
    r.reset(af::Renderer::create(window(), o, &error));
    check(r == nullptr && contains(error, "injected Vulkan failure") && contains(error, "EGL_BAD_CONFIG"),
          "combined failure preserves both backend causes");
    r.reset(); af_test::reset(); error.clear();
    r.reset(af::Renderer::create(window(), options(), &error));
    check(r != nullptr && error.empty(), "a fresh attempt can succeed after initialization failure");
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
    msaaFallback(); hdrFallback(); capabilityTokens(); selector();
    std::printf("Renderer host fault injection: %d checks, %d failure(s) (no physical GPU)\n", checks, failures);
    return failures ? 1 : 0;
}
