/*
 * AvatarForge :: af_gl.cpp -- EGL + OpenGL ES 3.1 renderer.
 *
 * EGL window surface -> MSAA HDR framebuffer -> resolve -> bloom -> composite -> window,
 * with a depth-only shadow pass. GLES 3.1 is required; unsupported initialization fails
 * explicitly. The OS-provided driver may itself use a translation layer, reported verbatim.
 */
#include "af_render.h"
#include "af_render_common.h"
#include "af_environment.h"
#include "af_diagnostics.h"
#include "af_shaders.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>            /* EGL_OPENGL_ES3_BIT_KHR */
#include <GLES3/gl31.h>
#include <android/native_window.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace af {
namespace {

/* ---- option sanitisation -------------------------------------------------------------------
 *
 * Every value below arrives from outside the renderer: the Kotlin settings store, a host app, the
 * C ABI or the Rust wrapper. A shadow map is `size * size * depth bytes` of GPU memory, so a stale
 * or hostile 65536 would ask the driver for 16 GB and turn a settings bug into a lost context on a
 * phone that was rendering fine a frame earlier. The renderer therefore owns the final word on
 * what it will allocate, and reports the clamped values back through its stats.
 */
float finiteOr(float value, float fallback, float lo, float hi) {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, lo, hi);
}

int clampedInt(int value, int fallback, int lo, int hi) {
    /* Non-positive means "caller left it unset", not "allocate zero": use the documented default. */
    if (value <= 0) return fallback;
    return std::clamp(value, lo, hi);
}

RendererOptions sanitizeOptions(RendererOptions o) {
    o.quality = (o.quality == QualityPreset::LOW || o.quality == QualityPreset::MEDIUM ||
                 o.quality == QualityPreset::HIGH) ? o.quality : QualityPreset::AUTO;
    /* 4x is the highest count this target set is budgeted for; more samples multiply the colour and
       depth renderbuffer cost without a visible gain at phone DPI. */
    o.msaaSamples = clampedInt(o.msaaSamples, 4, 1, 4);
    o.shadowMapSize = clampedInt(o.shadowMapSize, 2048, 512, 2048);
    o.bloomLevels = clampedInt(o.bloomLevels, 4, 0, 5);
    o.sunAzimuthDeg = finiteOr(o.sunAzimuthDeg, 42.0f, 0.0f, 360.0f);
    o.sunElevationDeg = finiteOr(o.sunElevationDeg, 38.0f, -5.0f, 90.0f);
    o.sunIntensity = finiteOr(o.sunIntensity, 1.5f, 0.0f, 16.0f);
    o.turbidity = finiteOr(o.turbidity, 2.6f, 0.5f, 20.0f);
    o.groundAlbedo = finiteOr(o.groundAlbedo, 0.28f, 0.0f, 1.0f);
    o.exposure = finiteOr(o.exposure, 0.65f, 0.05f, 8.0f);
    o.environmentIntensity = finiteOr(o.environmentIntensity, 0.65f, 0.0f, 4.0f);
    o.shadowSoftness = finiteOr(o.shadowSoftness, 1.0f, 0.25f, 8.0f);
    return o;
}

std::string hexCode(unsigned int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%04X", value);
    return text;
}

const char* glErrorName(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return "GL error";
    }
}

/* Optional GLES GPU timer queries. The extension is resolved at runtime and is never required for
   renderer startup. Each frame uses a free query from a small ring; pending results are polled but
   never waited on, and a disjoint event invalidates every outstanding measurement. */
struct GlesGpuTimer {
    using GenQueries = void (*)(GLsizei, GLuint*);
    using DeleteQueries = void (*)(GLsizei, const GLuint*);
    using BeginQuery = void (*)(GLenum, GLuint);
    using EndQuery = void (*)(GLenum);
    using GetQueryObjectuiv = void (*)(GLuint, GLenum, GLuint*);
    using GetQueryObjectui64v = void (*)(GLuint, GLenum, unsigned long long*);

    struct Slot { GLuint query = 0; bool pending = false; bool discarded = false; };
    static constexpr size_t kSlotCount = 3;
    static constexpr GLenum kTimeElapsed = 0x88BF;
    static constexpr GLenum kQueryResult = 0x8866;
    static constexpr GLenum kQueryResultAvailable = 0x8867;
    static constexpr GLenum kGpuDisjoint = 0x8FBB;

    GenQueries genQueries = nullptr;
    DeleteQueries deleteQueries = nullptr;
    BeginQuery beginQuery = nullptr;
    EndQuery endQuery = nullptr;
    GetQueryObjectuiv getQueryObjectuiv = nullptr;
    GetQueryObjectui64v getQueryObjectui64v = nullptr;
    std::array<Slot, kSlotCount> slots{};
    size_t nextSlot = 0;
    int activeSlot = -1;
    bool supported = false;
    bool disjointThisFrame = false;

    template <typename Function>
    static Function resolve(const char* name) {
        return reinterpret_cast<Function>(eglGetProcAddress(name));
    }

    void initialize(bool extensionPresent) {
        if (!extensionPresent) return;
        genQueries = resolve<GenQueries>("glGenQueriesEXT");
        deleteQueries = resolve<DeleteQueries>("glDeleteQueriesEXT");
        beginQuery = resolve<BeginQuery>("glBeginQueryEXT");
        endQuery = resolve<EndQuery>("glEndQueryEXT");
        getQueryObjectuiv = resolve<GetQueryObjectuiv>("glGetQueryObjectuivEXT");
        getQueryObjectui64v = resolve<GetQueryObjectui64v>("glGetQueryObjectui64vEXT");
        if (!genQueries || !deleteQueries || !beginQuery || !endQuery ||
            !getQueryObjectuiv || !getQueryObjectui64v) return;
        GLuint names[kSlotCount] = {};
        genQueries(static_cast<GLsizei>(kSlotCount), names);
        const GLenum error = glGetError();
        bool valid = error == GL_NO_ERROR;
        for (size_t i = 0; i < kSlotCount; ++i) {
            slots[i].query = names[i];
            valid = valid && names[i] != 0;
        }
        if (!valid) {
            if (std::any_of(names, names + kSlotCount, [](GLuint name) { return name != 0; }))
                deleteQueries(static_cast<GLsizei>(kSlotCount), names);
            slots = {};
            for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
            return;
        }
        supported = true;
    }

    void poll(RendererStats& stats) {
        if (!supported) return;
        disjointThisFrame = false;
        GLint disjoint = GL_FALSE;
        glGetIntegerv(kGpuDisjoint, &disjoint);
        if (disjoint == GL_TRUE) {
            disjointThisFrame = true;
            stats.gpuMs = -1.0f;
            for (Slot& slot : slots) if (slot.pending) slot.discarded = true;
        }
        for (Slot& slot : slots) {
            if (!slot.pending) continue;
            GLuint available = GL_FALSE;
            getQueryObjectuiv(slot.query, kQueryResultAvailable, &available);
            if (available != GL_TRUE) continue;
            if (!disjoint && !slot.discarded) {
                unsigned long long elapsedNs = 0;
                getQueryObjectui64v(slot.query, kQueryResult, &elapsedNs);
                const double elapsedMs = static_cast<double>(elapsedNs) / 1000000.0;
                if (elapsedNs > 0 && std::isfinite(elapsedMs) &&
                    elapsedMs <= static_cast<double>(std::numeric_limits<float>::max())) {
                    stats.gpuMs = static_cast<float>(elapsedMs);
                } else {
                    stats.gpuMs = -1.0f;
                }
            }
            slot.pending = false;
            slot.discarded = false;
        }
    }

    void begin() {
        if (!supported || activeSlot >= 0 || disjointThisFrame) return;
        for (size_t offset = 0; offset < kSlotCount; ++offset) {
            const size_t index = (nextSlot + offset) % kSlotCount;
            if (slots[index].pending) continue;
            beginQuery(kTimeElapsed, slots[index].query);
            activeSlot = static_cast<int>(index);
            return;
        }
    }

    void end() {
        if (!supported || activeSlot < 0) return;
        endQuery(kTimeElapsed);
        Slot& slot = slots[static_cast<size_t>(activeSlot)];
        slot.pending = true;
        slot.discarded = false;
        nextSlot = (static_cast<size_t>(activeSlot) + 1) % kSlotCount;
        activeSlot = -1;
    }

    void destroy() {
        if (deleteQueries) {
            GLuint names[kSlotCount] = {};
            for (size_t i = 0; i < kSlotCount; ++i) names[i] = slots[i].query;
            if (std::any_of(names, names + kSlotCount, [](GLuint name) { return name != 0; }))
                deleteQueries(static_cast<GLsizei>(kSlotCount), names);
        }
        slots = {};
        supported = false;
        disjointThisFrame = false;
        activeSlot = -1;
    }
};

const char* eglErrorName(EGLint error) {
    switch (error) {
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "EGL error";
    }
}

bool checkGlError(const char* operation, std::string* error) {
    /* Drain the queue so a failure cannot leak into the next initialization stage. Keep the
       first error: it is the cause, while subsequent errors are often its consequences. */
    GLenum first = GL_NO_ERROR;
    int count = 0;
    for (GLenum current = glGetError(); current != GL_NO_ERROR; current = glGetError()) {
        if (first == GL_NO_ERROR) first = current;
        if (++count == 16) break;
    }
    if (first == GL_NO_ERROR) return true;
    if (error) {
        *error = std::string(operation) + ": " + glErrorName(first) + " (" + hexCode(first) + ")";
        if (count > 1) *error += " and " + std::to_string(count - 1) + " further error(s)";
    }
    return false;
}

void failEgl(const char* operation, std::string* error) {
    if (!error) return;
    const EGLint code = eglGetError();
    *error = std::string(operation) + ": " + eglErrorName(code) + " (" + hexCode(code) + ")";
}

bool compileShader(GLenum type, const char* source, const char* label, GLuint* out, std::string* error) {
    const char* stage = type == GL_VERTEX_SHADER ? "vertex shader " : "fragment shader ";
    const GLuint shader = glCreateShader(type);
    if (!shader || !checkGlError("glCreateShader", error)) {
        if (error && error->empty()) *error = std::string("glCreateShader returned zero for ") + stage + label;
        if (shader) glDeleteShader(shader);
        return false;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 1 ? len : 1), '\0');
        glGetShaderInfoLog(shader, len, nullptr, &log[0]);
        if (error) *error = std::string(stage) + label + ": " + log;
        glDeleteShader(shader);
        return false;
    }
    if (!checkGlError("glCompileShader", error)) {
        glDeleteShader(shader);
        return false;
    }
    *out = shader;
    return true;
}

class GlRenderer final : public Renderer {
public:
    bool init(ANativeWindow* window, const RendererOptions& options, std::string* error);
    ~GlRenderer() override;

    bool resize(int width, int height) override;
    ResizeOutcome resizeWithResult(int width, int height) override;
    ResizeOutcome lastFrameResizeOutcome() const override { return lastFrameResizeOutcome_; }
    void attachEngine(af_engine* engine) override;
    void invalidateMesh() override;
    void setCamera(const CameraState& camera) override { camera_ = camera; }
    void setOptions(const RendererOptions& options) override;
    RendererOptions options() const override { return options_; }
    void renderFrame(float deltaSeconds) override;
    void setSkeletonVisible(bool visible) override { skeletonVisible_ = visible; }
    void setFlatShading(bool flat) override { flatShading_ = flat; }
    void setDiagnosticRung(int rung) override;
    int diagnosticRung() const override { return static_cast<int>(diagnosticRung_); }
    void setHighlightRegion(int region) override { highlightRegion_ = region; }
    void setClip(int clip, float speed, bool loop) override;
    int currentClip() const override { return clip_; }
    float clipTime() const override { return clipTime_; }
    void waitIdle() override { glFinish(); }
    RendererStats stats() const override { return stats_; }
    std::string frameError() const override { return frameError_; }
    RendererInfo info() const override { return info_; }

private:
    bool createContext(ANativeWindow* window, std::string* error);
    bool createPrograms(std::string* error);
    bool createGeometryResources(std::string* error);
    bool createUniformBuffers(std::string* error);
    bool createPlaceholderAtlas(std::string* error);
    bool createTargets(std::string* error);
    void uploadAtlas(std::string* error);
    void ensureGeometryBuffers();
    void writeDiagnosticMaterial();
    void rebuildMaterialBlocks();
    void buildSkeletonLines();
    void updateScene(float deltaSeconds);
    void updateAnimation(float deltaSeconds);
    void drawShadowPass();
    void drawForwardPass();
    void drawBloom();
    void drawComposite();
    void destroyTargets();
    void destroyAll();
    bool makeFramebufferWithTexture(GLuint* fbo, GLuint* texture, int w, int h, GLenum internalFormat,
                                    GLenum format, GLenum type, const char* label, std::string* error);

    GLuint linkProgram(const char* vsName, const char* fsName, std::string* error);

    /* ---- EGL / context ---- */
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    uint32_t uboAlignment_ = 256;
    uint32_t cameraStride_ = 512;

    /* ---- programs and uniform locations ---- */
    GLuint forwardProgram_ = 0, shadowProgram_ = 0, groundProgram_ = 0, lineProgram_ = 0;
    GLuint bloomDownProgram_ = 0, bloomUpProgram_ = 0, compositeProgram_ = 0;
    GLuint avatarVao_ = 0, groundVao_ = 0, emptyVao_ = 0, lineVao_ = 0;

    /* ---- targets ---- */
    GLuint hdrFbo_ = 0, hdrTexture_ = 0;              /* resolved HDR image           */
    GLuint msaaFbo_ = 0, msaaColorRb_ = 0, msaaDepthRb_ = 0;
    GLuint shadowFbo_ = 0, shadowTexture_ = 0;
    GLuint bloomFbo_[5] = {0, 0, 0, 0, 0};
    GLuint bloomTexture_[5] = {0, 0, 0, 0, 0};
    int bloomWidth_[5] = {0, 0, 0, 0, 0};
    int bloomHeight_[5] = {0, 0, 0, 0, 0};
    int bloomLevels_ = 0;
    GLenum hdrInternalFormat_ = GL_RGBA16F;

    /* ---- buffers ---- */
    GLuint cameraUbo_ = 0, postUbo_ = 0, materialUbo_ = 0;
    GLuint vertexVbo_ = 0, indexIbo_ = 0, groundVbo_ = 0, lineVbo_ = 0;
    GLuint atlasTexture_[AF_TEX_COUNT] = {0, 0, 0, 0, 0};
    bool atlasUploaded_ = false;
    GLint64 vertexCapacity_ = 0, indexCapacity_ = 0, materialCapacity_ = 0, lineCapacity_ = 0;

    /* ---- uniform locations ---- */
    struct Uniforms {
        GLint baseColor = -1, normal = -1, metalRough = -1, occlusion = -1, subsurface = -1, shadow = -1;
        GLint source = -1, bloom = -1;
    } uForward_{}, uShadow_{}, uGround_{}, uLine_{}, uPost_{};

    /* ---- state ---- */
    RendererOptions options_{};
    RendererOptions activeTargetOptions_{};  /* last target set that completed successfully */
    RendererInfo info_{};
    RendererStats stats_{};
    af_engine* engine_ = nullptr;
    CameraState camera_{};
    FrameScene scene_{};
    EnvironmentParams env_{};
    ShGlobal sh_{};
    bool meshDirty_ = true;
    DiagnosticRung diagnosticRung_ = DiagnosticRung::PRODUCTION_AVATAR;
    DiagnosticMesh diagnosticMesh_{};
    float diagnosticTimeSeconds_ = 0.0f;
    bool diagnosticDirty_ = true;
    int highlightRegion_ = -1;
    bool skeletonVisible_ = false;
    bool flatShading_ = false;
    int clip_ = -1;
    float clipSpeed_ = 1.0f;
    bool clipLoop_ = true;
    float clipTime_ = 0.0f;
    int32_t vertexCount_ = 0;
    int32_t indexCount_ = 0;
    GlesGpuTimer gpuTimer_{};
    uint32_t materialStride_ = 256;
    int materialCount_ = 0;
    uint32_t lineVertexCount_ = 0;
    int drawCalls_ = 0;
    int triangles_ = 0;
    bool targetsDirty_ = false;
    bool contextLost_ = false;
    std::string frameError_;
    ResizeOutcome lastFrameResizeOutcome_{};
};

/* ============================================================================================
 *  Context
 * ============================================================================================ */
bool GlRenderer::createContext(ANativeWindow* window, std::string* error) {
    (void)eglGetError();   /* Do not attribute an older thread-local error to this operation. */
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) { failEgl("eglGetDisplay", error); return false; }
    if (!eglInitialize(display_, nullptr, nullptr)) { failEgl("eglInitialize", error); return false; }

    /* Require ES 3.1: ES3_BIT_KHR with a 3.x context, plus a depth buffer for the shadow pass and
       a window surface that EGL itself considers RGB-capable. Multisampling is done with
       renderbuffers (the window surface is only ever blitted to), so no MSAA config is needed. */
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,   /* ES 3.x renderable */
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint configCount = 0;
    if (!eglChooseConfig(display_, configAttribs, &config_, 1, &configCount)) {
        failEgl("eglChooseConfig", error);
        return false;
    }
    if (configCount == 0) {
        *error = "no EGL config with an ES3 renderable type and a 24-bit depth buffer";
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) { failEgl("eglCreateContext(ES3)", error); return false; }

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) { failEgl("eglCreateWindowSurface", error); return false; }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        failEgl("eglMakeCurrent", error);
        return false;
    }
    if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &surfaceWidth_) ||
        !eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfaceHeight_)) {
        failEgl("eglQuerySurface(size)", error);
        return false;
    }
    (void)glGetError();   /* GL error state becomes valid only after eglMakeCurrent. */

    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* rendererName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    if (!version) { *error = "glGetString(GL_VERSION) returned null"; return false; }

    int major = 0, minor = 0;
    std::sscanf(version, "OpenGL ES %d.%d", &major, &minor);
    if (major * 10 + minor < 31) {
        *error = std::string("OpenGL ES 3.1 is required, this device reports: ") + version;
        return false;
    }

    GLint align = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    uboAlignment_ = uint32_t(align > 0 ? align : 256);

    /* In an ES 3.x context extensions are indexed, not one space-separated GL_EXTENSIONS
       string. Querying glGetString(GL_EXTENSIONS) can produce GL_INVALID_ENUM on conforming
       drivers; doing so again at atlas upload could stop the first animated frame. */
    GLint extensionCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
    std::vector<std::string> extensions;
    for (GLint i = 0; i < extensionCount; ++i) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (name) extensions.emplace_back(name);
    }
    auto hasExtension = [&extensions](const char* name) {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    };
    /* ES 3.2 makes half-float colour attachments renderable in core. */
    const bool floatColour = major > 3 || (major == 3 && minor >= 2) ||
                             hasExtension("GL_EXT_color_buffer_float") ||
                             hasExtension("GL_EXT_color_buffer_half_float");
    hdrInternalFormat_ = floatColour ? GL_RGBA16F : GL_RGBA8;
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    info_.backend = "gles31";
    info_.deviceName = rendererName ? rendererName : "unknown GPU";
    info_.apiVersion = version;
    info_.driver = vendor ? vendor : "unknown vendor";
    info_.supportsFloatHdr = floatColour;
    info_.supportsMsaa = maxSamples > 1;
    info_.supportsShadows = true;
    info_.supportsAnisotropy = hasExtension("GL_EXT_texture_filter_anisotropic");
    if (!checkGlError("context capability queries", error)) return false;
    gpuTimer_.initialize(hasExtension("GL_EXT_disjoint_timer_query"));
    stats_.gpuTimingSupported = gpuTimer_.supported;
    return true;
}

/* ============================================================================================
 *  Programs
 * ============================================================================================ */
GLuint GlRenderer::linkProgram(const char* vsName, const char* fsName, std::string* error) {
    const AfShaderGl* vs = afShaderGl(vsName, AF_STAGE_VERT);
    const AfShaderGl* fs = afShaderGl(fsName, AF_STAGE_FRAG);
    if (!vs || !fs) {
        *error = std::string("shader source missing for ") + vsName;
        return 0;
    }
    GLuint vsobj = 0, fsobj = 0;
    if (!compileShader(GL_VERTEX_SHADER, vs->source, vsName, &vsobj, error)) return 0;
    if (!compileShader(GL_FRAGMENT_SHADER, fs->source, fsName, &fsobj, error)) {
        glDeleteShader(vsobj);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (!program || !checkGlError("glCreateProgram", error)) {
        if (error && error->empty()) *error = std::string("glCreateProgram returned zero for ") + vsName;
        if (program) glDeleteProgram(program);
        glDeleteShader(vsobj);
        glDeleteShader(fsobj);
        return 0;
    }
    glAttachShader(program, vsobj);
    glAttachShader(program, fsobj);
    glLinkProgram(program);
    glDeleteShader(vsobj);
    glDeleteShader(fsobj);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 1 ? len : 1), '\0');
        glGetProgramInfoLog(program, len, nullptr, &log[0]);
        *error = std::string("link ") + vsName + "/" + fsName + ": " + log;
        glDeleteProgram(program);
        return 0;
    }
    if (!checkGlError("glLinkProgram", error)) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool GlRenderer::createPrograms(std::string* error) {
    if (!(forwardProgram_ = linkProgram("forward", "forward", error))) return false;
    if (!(shadowProgram_ = linkProgram("shadow", "shadow", error))) return false;
    if (!(groundProgram_ = linkProgram("ground", "ground", error))) return false;
    if (!(lineProgram_ = linkProgram("line", "line", error))) return false;
    if (!(bloomDownProgram_ = linkProgram("fullscreen", "bloom_down", error))) return false;
    if (!(bloomUpProgram_ = linkProgram("fullscreen", "bloom_up", error))) return false;
    if (!(compositeProgram_ = linkProgram("fullscreen", "composite", error))) return false;

    /* Uniform blocks are bound by index rather than relying on the layout(binding=) qualifier,
       which ES 3.1 accepts but not every driver implements identically. */
    const GLuint programs[7] = {forwardProgram_, shadowProgram_, groundProgram_, lineProgram_,
                                bloomDownProgram_, bloomUpProgram_, compositeProgram_};
    bool hasCameraBlock = false;
    for (GLuint p : programs) {
        const GLuint cameraIndex = glGetUniformBlockIndex(p, "CameraBlock");
        if (cameraIndex != GL_INVALID_INDEX) {
            glUniformBlockBinding(p, cameraIndex, 0);
            hasCameraBlock = true;
        }
        const GLuint materialIndex = glGetUniformBlockIndex(p, "MaterialBlock");
        if (materialIndex != GL_INVALID_INDEX) glUniformBlockBinding(p, materialIndex, 1);
        const GLuint postIndex = glGetUniformBlockIndex(p, "PostBlock");
        if (postIndex != GL_INVALID_INDEX) glUniformBlockBinding(p, postIndex, 2);
    }
    if (!hasCameraBlock) {
        *error = "no program declared the CameraBlock uniform block (shader/ABI mismatch)";
        return false;
    }

    if (!checkGlError("uniform block bindings", error)) return false;

    /* Sampler uniforms belong to the CURRENT program, not to the program returned by
       glGetUniformLocation. Without glUseProgram these writes raise GL_INVALID_OPERATION;
       a later error check then misreports the failure as a render-target/init failure. */
    GLint previousProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    auto unit = [](GLuint p, const char* name, GLint value) {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) {
            glUseProgram(p);
            glUniform1i(loc, value);
        }
    };
    unit(forwardProgram_, "uBaseColorTex", 0);
    unit(forwardProgram_, "uNormalTex", 1);
    unit(forwardProgram_, "uMetalRoughTex", 2);
    unit(forwardProgram_, "uOcclusionTex", 3);
    unit(forwardProgram_, "uSubsurfaceTex", 4);
    unit(forwardProgram_, "uShadowMap", 5);
    unit(groundProgram_, "uShadowMap", 5);
    unit(bloomDownProgram_, "uSourceTex", 6);
    unit(bloomUpProgram_, "uSourceTex", 6);
    unit(compositeProgram_, "uSourceTex", 6);
    unit(compositeProgram_, "uBloomTex", 7);
    unit(shadowProgram_, "uBaseColorTex", 0);   /* present only to keep the block uniform */
    glUseProgram(GLuint(previousProgram));
    if (!checkGlError("sampler uniform assignments", error)) return false;

    uForward_.baseColor = glGetUniformLocation(forwardProgram_, "uBaseColorTex");
    uForward_.normal = glGetUniformLocation(forwardProgram_, "uNormalTex");
    uForward_.metalRough = glGetUniformLocation(forwardProgram_, "uMetalRoughTex");
    uForward_.occlusion = glGetUniformLocation(forwardProgram_, "uOcclusionTex");
    uForward_.subsurface = glGetUniformLocation(forwardProgram_, "uSubsurfaceTex");
    uPost_.source = glGetUniformLocation(bloomDownProgram_, "uSourceTex");
    return true;
}

/* ============================================================================================
 *  Targets
 * ============================================================================================ */
bool GlRenderer::makeFramebufferWithTexture(GLuint* fbo, GLuint* texture, int w, int h,
                                            GLenum internalFormat, GLenum format, GLenum type,
                                            const char* label, std::string* error) {
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(internalFormat), w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    if (!checkGlError(label, error)) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        *error = std::string(label) + ": incomplete framebuffer (" + hexCode(status) +
                 "), format " + hexCode(internalFormat) + ", " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    return true;
}

bool GlRenderer::createTargets(std::string* error) {
    const int w = surfaceWidth_ > 0 ? surfaceWidth_ : 1080;
    const int h = surfaceHeight_ > 0 ? surfaceHeight_ : 1920;
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    const int samples = std::max(1, std::min(options_.msaaSamples, int(maxSamples)));
    options_.msaaSamples = samples;

    /* Resolved HDR image, sampled by bloom and composite. */
    if (!makeFramebufferWithTexture(&hdrFbo_, &hdrTexture_, w, h, hdrInternalFormat_, GL_RGBA,
                                   hdrInternalFormat_ == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE,
                                   "HDR colour framebuffer", error)) {
        return false;
    }

    /* Multisampled renderbuffers the forward pass draws into; blitted into hdrFbo_ afterwards. */
    glGenFramebuffers(1, &msaaFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo_);
    glGenRenderbuffers(1, &msaaColorRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaColorRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, hdrInternalFormat_, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColorRb_);
    glGenRenderbuffers(1, &msaaDepthRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaDepthRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msaaDepthRb_);
    const bool msaaAllocated = checkGlError("MSAA storage allocation", nullptr);
    if (!msaaAllocated || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        /* The device refused the requested format/sample count. Consume that expected rejection
           before retrying both attachments at 1x, so it is not mistaken for a later failure. */
        glBindRenderbuffer(GL_RENDERBUFFER, msaaDepthRb_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msaaDepthRb_);
        glDeleteRenderbuffers(1, &msaaColorRb_);
        glGenRenderbuffers(1, &msaaColorRb_);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaColorRb_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, hdrInternalFormat_, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColorRb_);
        if (!checkGlError("single-sample render-target allocation", error)) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        const GLenum singleSampleStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (singleSampleStatus != GL_FRAMEBUFFER_COMPLETE) {
            *error = "MSAA framebuffer incomplete at 1x (" + hexCode(singleSampleStatus) +
                     "), format " + hexCode(hdrInternalFormat_) + ", " + std::to_string(w) + "x" + std::to_string(h);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        options_.msaaSamples = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Shadow map: a depth texture with hardware comparison, so the shader's 5 taps each get a
       2x2 filtered comparison for free. */
    const int shadowSize = options_.enableShadows ? options_.shadowMapSize : 512;
    glGenTextures(1, &shadowTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadowSize, shadowSize, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &shadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTexture_, 0);
    const GLenum noColour = GL_NONE;
    glDrawBuffers(1, &noColour);                /* depth-only: explicitly disable colour output */
    glReadBuffer(GL_NONE);
    if (!checkGlError("shadow framebuffer allocation", error) ||
        glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        if (error->empty()) *error = "shadow framebuffer incomplete";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Bloom chain. */
    bloomLevels_ = options_.enableBloom ? std::max(1, std::min(5, options_.bloomLevels)) : 0;
    int level = 0;
    for (int i = 0; i < bloomLevels_; ++i) {
        const int lw = std::max(1, w >> (i + 1));
        const int lh = std::max(1, h >> (i + 1));
        if (lw < 8 || lh < 8) break;
        if (!makeFramebufferWithTexture(&bloomFbo_[level], &bloomTexture_[level], lw, lh,
                                        hdrInternalFormat_, GL_RGBA,
                                        hdrInternalFormat_ == GL_RGBA16F ? GL_HALF_FLOAT
                                                                         : GL_UNSIGNED_BYTE,
                                        "bloom colour framebuffer", error)) {
            return false;
        }
        bloomWidth_[level] = lw;
        bloomHeight_[level] = lh;
        ++level;
    }
    bloomLevels_ = level;
    stats_.bloomLevels = bloomLevels_;
    stats_.msaaSamples = options_.msaaSamples;
    stats_.shadowMapSize = options_.enableShadows ? options_.shadowMapSize : 0;
    return checkGlError("render-target setup", error);
}

/* ============================================================================================
 *  Setup
 * ============================================================================================ */
bool GlRenderer::createGeometryResources(std::string* error) {
    const float quad[12] = {-1.0f, 0.0f, -1.0f, 1.0f, 0.0f, -1.0f,
                            -1.0f, 0.0f,  1.0f, 1.0f, 0.0f,  1.0f};
    glGenBuffers(1, &groundVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, groundVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    lineCapacity_ = GLint64(4096 * 28);
    glGenBuffers(1, &lineVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, lineCapacity_, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!checkGlError("ground/skeleton buffer allocation", error)) return false;

    /* Names may exist before their data store is allocated. The avatar's stores are sized on
       attachment, but its VAO needs non-zero buffer bindings now. */
    glGenBuffers(1, &vertexVbo_);
    glGenBuffers(1, &indexIbo_);
    glGenVertexArrays(1, &avatarVao_);
    glGenVertexArrays(1, &groundVao_);
    glGenVertexArrays(1, &emptyVao_);
    glGenVertexArrays(1, &lineVao_);

    /* A VAO records buffer names and the element-buffer binding. Allocate and bind the storage
       BEFORE describing attributes: binding name zero is not a forward declaration and leaves
       the VAO unusable even if a later call creates a buffer with the intended name. */
    glBindVertexArray(avatarVao_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VertexLayout::stride,
                          reinterpret_cast<const void*>(size_t(VertexLayout::posOffset)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VertexLayout::stride,
                          reinterpret_cast<const void*>(size_t(VertexLayout::normalOffset)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, VertexLayout::stride,
                          reinterpret_cast<const void*>(size_t(VertexLayout::tangentOffset)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, VertexLayout::stride,
                          reinterpret_cast<const void*>(size_t(VertexLayout::uvOffset)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, VertexLayout::stride,
                          reinterpret_cast<const void*>(size_t(VertexLayout::colorOffset)));
    glBindVertexArray(0);

    glBindVertexArray(groundVao_);
    glBindBuffer(GL_ARRAY_BUFFER, groundVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
    glBindVertexArray(0);

    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 7, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 7,
                          reinterpret_cast<const void*>(sizeof(float) * 3));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return checkGlError("vertex array configuration", error);
}

bool GlRenderer::createUniformBuffers(std::string* error) {
    /* CameraUbo is 448 bytes. On a 256-byte-aligned device two slices need 1024 bytes, not 512;
       otherwise the second frame's glBindBufferRange/glBufferSubData runs past the allocation. */
    cameraStride_ = uint32_t((sizeof(CameraUbo) + uboAlignment_ - 1) / uboAlignment_) * uboAlignment_;
    glGenBuffers(1, &cameraUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(cameraStride_) * 2, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &postUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, postUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(uboAlignment_) * 16, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &materialUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, materialUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(uboAlignment_) * 32, nullptr, GL_DYNAMIC_DRAW);
    materialCapacity_ = GLint64(uboAlignment_) * 32;
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return checkGlError("uniform buffer allocation", error);
}

bool GlRenderer::createPlaceholderAtlas(std::string* error) {
    /* 1x1 white, so sampler bindings are valid before an engine is attached. */
    const GLubyte white[4] = {255, 255, 255, 255};
    stats_.textureBytes = AF_TEX_COUNT * 4ull;
    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        glGenTextures(1, &atlasTexture_[i]);
        glBindTexture(GL_TEXTURE_2D, atlasTexture_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return checkGlError("placeholder atlas allocation", error);
}

bool GlRenderer::init(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    options_ = sanitizeOptions(options);
    std::string detail;
    auto failed = [error](const char* stage, const std::string& reason) {
        if (error) *error = std::string(stage) + ": " + (reason.empty() ? "unspecified failure" : reason);
        return false;
    };
    if (!createContext(window, &detail)) return failed("EGL context", detail);
    if (!createPrograms(&detail)) return failed("shader programs", detail);
    if (!createGeometryResources(&detail)) return failed("geometry resources", detail);
    if (!createUniformBuffers(&detail)) return failed("uniform buffers", detail);
    if (!createPlaceholderAtlas(&detail)) return failed("placeholder atlas", detail);
    if (!createTargets(&detail)) {
        if (hdrInternalFormat_ != GL_RGBA16F) return failed("render targets", detail);
        /* An advertised extension is not proof that this driver can allocate the requested
           attachment. Try the core RGBA8 path once, and publish the format actually in use. */
        destroyTargets();
        hdrInternalFormat_ = GL_RGBA8;
        info_.supportsFloatHdr = false;
        detail.clear();
        if (!createTargets(&detail)) return failed("render targets (RGBA8 fallback)", detail);
    }
    activeTargetOptions_ = options_;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    if (!checkGlError("initial render state", &detail)) return failed("render state", detail);

    if (engine_) {
        uploadAtlas(&detail);
        if (!checkGlError("atlas upload", &detail)) return failed("atlas upload", detail);
        rebuildMaterialBlocks();
        if (!checkGlError("material upload", &detail)) return failed("material upload", detail);
    }
    /* This is the app's own current EGL context, not SurfaceFlinger's renderer. An ANGLE
       GL_RENDERER that says "on Vulkan" describes the system's GLES implementation, not an
       AvatarForge Vulkan initialization or dependency. Log only after successful setup. */
    __android_log_print(ANDROID_LOG_INFO, "AvatarForgeRenderer",
                        "AF_GLES_CONTEXT backend=%s GL_RENDERER=\"%s\" GL_VERSION=\"%s\" GL_VENDOR=\"%s\"",
                        info_.backend.c_str(), info_.deviceName.c_str(),
                        info_.apiVersion.c_str(), info_.driver.c_str());
    return true;
}

/* ============================================================================================
 *  Geometry and atlas
 * ============================================================================================ */
void GlRenderer::ensureGeometryBuffers() {
    const bool diagnostic = diagnosticRung_ != DiagnosticRung::PRODUCTION_AVATAR;
    const bool animatedDiagnostic = diagnosticRung_ == DiagnosticRung::SKINNED_MESH ||
                                    diagnosticRung_ == DiagnosticRung::BLENDSHAPE_MESH;
    if (diagnostic) {
        if (diagnosticDirty_ || animatedDiagnostic) {
            if (!buildDiagnosticMesh(diagnosticRung_, diagnosticTimeSeconds_, &diagnosticMesh_)) {
                vertexCount_ = indexCount_ = 0;
                return;
            }
        }
        const int32_t vcount = static_cast<int32_t>(diagnosticMesh_.vertices.size());
        const int32_t icount = static_cast<int32_t>(diagnosticMesh_.indices.size());
        const af_vertex* verts = diagnosticMesh_.vertices.empty() ? nullptr : diagnosticMesh_.vertices.data();
        const uint32_t* indices = diagnosticMesh_.indices.empty() ? nullptr : diagnosticMesh_.indices.data();
        if (!verts || vcount <= 0) {
            vertexCount_ = indexCount_ = 0;
            diagnosticDirty_ = false;
            meshDirty_ = false;
            return;
        }

        const GLint64 vbytes = GLint64(vcount) * VertexLayout::stride;
        glBindVertexArray(avatarVao_);
        glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
        if (vbytes > vertexCapacity_) {
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vbytes + vbytes / 4), nullptr, GL_DYNAMIC_DRAW);
            vertexCapacity_ = vbytes + vbytes / 4;
            vertexCount_ = 0;
        }
        if (diagnosticDirty_ || animatedDiagnostic || vertexCount_ != vcount) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vbytes), verts);
            vertexCount_ = vcount;
        }
        if (icount > 0 && indices) {
            const GLint64 ibytes = GLint64(icount) * sizeof(uint32_t);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
            if (ibytes > indexCapacity_) {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(ibytes + ibytes / 4), nullptr, GL_DYNAMIC_DRAW);
                indexCapacity_ = ibytes + ibytes / 4;
                indexCount_ = 0;
            }
            if (diagnosticDirty_ || animatedDiagnostic || indexCount_ != icount) {
                glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(ibytes), indices);
                indexCount_ = icount;
            }
        } else {
            indexCount_ = 0;
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        diagnosticDirty_ = false;
        meshDirty_ = false;
        return;
    }

    if (!engine_) {
        vertexCount_ = indexCount_ = 0;
        return;
    }
    const int32_t vcount = af_engine_vertex_count(engine_);
    const int32_t icount = af_engine_index_count(engine_);
    const af_vertex* verts = af_engine_vertices(engine_);
    const uint32_t* indices = af_engine_indices(engine_);
    if (vcount <= 0 || !verts) return;

    const GLint64 vbytes = GLint64(vcount) * VertexLayout::stride;
    /* The element-array binding is VAO state. Lazy allocations must happen with the avatar
       VAO bound, just like the vertex attribute bindings configured during initialization. */
    glBindVertexArray(avatarVao_);
    if (vertexVbo_ == 0) glGenBuffers(1, &vertexVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
    if (vbytes > vertexCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vbytes + vbytes / 4), nullptr, GL_DYNAMIC_DRAW);
        vertexCapacity_ = vbytes + vbytes / 4;
        vertexCount_ = 0;   /* force a full upload below */
    }
    if (meshDirty_ || vertexCount_ != vcount) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vbytes), verts);
        vertexCount_ = vcount;
    }

    if (icount > 0 && indices) {
        const GLint64 ibytes = GLint64(icount) * sizeof(uint32_t);
        if (indexIbo_ == 0) glGenBuffers(1, &indexIbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
        if (ibytes > indexCapacity_) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(ibytes + ibytes / 4), nullptr, GL_STATIC_DRAW);
            indexCapacity_ = ibytes + ibytes / 4;
            indexCount_ = 0;
        }
        if (indexCount_ != icount) {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(ibytes), indices);
            indexCount_ = icount;
        }
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    meshDirty_ = false;
}

void GlRenderer::writeDiagnosticMaterial() {
    MaterialUbo block{};
    block.baseColorFactor[0] = block.baseColorFactor[1] = block.baseColorFactor[2] = 1.0f;
    block.baseColorFactor[3] = 1.0f;
    block.pbr[1] = 0.52f;
    block.pbr[2] = 1.5f;
    block.pbr2[2] = float(AF_KIND_GENERIC);
    block.texFlags[0] = diagnosticMesh_.textured ? 1.0f : 0.0f;
    /* Even the 1x1 placeholder is a valid sampled image, so the quad still exercises the sampler
       path before a production engine has supplied its baked atlas. */
    block.texFlags[3] = 1.0f;
    block.region[0] = -1.0f;
    glBindBuffer(GL_UNIFORM_BUFFER, materialUbo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(block), &block);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GlRenderer::uploadAtlas(std::string* error) {
    if (!engine_) return;
    uint64_t atlasBytes = 0;
    bool anyAtlas = false;
    for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
        af_texture tex{};
        if (af_engine_texture(engine_, slot, &tex) != AF_OK) continue;
        if (!tex.data || tex.width <= 0 || tex.height <= 0) continue;

        const GLenum internal = (tex.channels == 1) ? GL_R8
                               : (slot == AF_TEX_BASE_COLOR) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        const GLenum format = (tex.channels == 1) ? GL_RED : GL_RGBA;
        glBindTexture(GL_TEXTURE_2D, atlasTexture_[slot]);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), tex.width, tex.height, 0, format,
                     GL_UNSIGNED_BYTE, tex.data);
        glGenerateMipmap(GL_TEXTURE_2D);   /* Driver-generated mip chain. */
        /* Account for the mip chain the driver just allocated, not only level 0. */
        {
            uint32_t mw = uint32_t(tex.width), mh = uint32_t(tex.height);
            const uint32_t bpp = tex.channels == 1 ? 1u : 4u;
            for (uint32_t level = 0; level < 13; ++level) {
                atlasBytes += uint64_t(mw) * mh * bpp;
                if (mw == 1 && mh == 1) break;
                mw = mw > 1 ? mw / 2 : 1;
                mh = mh > 1 ? mh / 2 : 1;
            }
        }
        anyAtlas = true;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* The generated chain must not collapse to magenta when a driver limits mip depth. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 12);

        /* Anisotropy keeps atlas detail readable on oblique surfaces during clip playback.
           Use the capability captured with glGetStringi at context creation; querying the
           legacy extension string here can invalidate an otherwise good GLES frame. */
        if (info_.supportsAnisotropy) {
            GLfloat maxAniso = 1.0f;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &maxAniso);
            if (std::isfinite(maxAniso) && maxAniso >= 1.0f) {
                glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */,
                                std::min(4.0f, maxAniso));
            }
        }
    }
    atlasUploaded_ = true;
    stats_.textureBytes = anyAtlas ? atlasBytes : AF_TEX_COUNT * 4ull;
    (void)error;
}

void GlRenderer::rebuildMaterialBlocks() {
    if (!engine_) return;
    const int submeshCount = af_engine_submesh_count(engine_);
    const af_submesh* submeshes = af_engine_submeshes(engine_);
    const af_material* materials = af_engine_materials(engine_);
    const int materialCount = af_engine_material_count(engine_);
    if (submeshCount <= 0 || !submeshes) { materialCount_ = 0; return; }

    materialStride_ = uboAlignment_ > sizeof(MaterialUbo) ? uboAlignment_
                                                          : (sizeof(MaterialUbo) + 15u) / 16u * 16u;
    const GLint64 needed = GLint64(submeshCount + 1) * materialStride_;
    glBindBuffer(GL_UNIFORM_BUFFER, materialUbo_);
    if (needed > materialCapacity_) {
        glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(needed * 2), nullptr, GL_DYNAMIC_DRAW);
        materialCapacity_ = needed * 2;
    }

    for (int i = 0; i < submeshCount; ++i) {
        const af_submesh& sm = submeshes[i];
        af_material fallback{};
        fallback.base_color_factor[0] = fallback.base_color_factor[1] = fallback.base_color_factor[2] = 1.0f;
        fallback.base_color_factor[3] = 1.0f;
        fallback.roughness = 0.5f;
        fallback.ior = 1.4f;
        const af_material& m = (materials && sm.material_index >= 0 && sm.material_index < materialCount)
                                   ? materials[sm.material_index] : fallback;
        MaterialUbo block{};
        block.baseColorFactor[0] = m.base_color_factor[0];
        block.baseColorFactor[1] = m.base_color_factor[1];
        block.baseColorFactor[2] = m.base_color_factor[2];
        block.baseColorFactor[3] = m.base_color_factor[3];
        block.pbr[0] = m.metallic;
        block.pbr[1] = m.roughness;
        block.pbr[2] = m.ior;
        block.pbr[3] = m.sheen;
        block.pbr2[0] = m.subsurface;
        block.pbr2[1] = m.anisotropic;
        block.pbr2[2] = float(materialKindForRegion(sm.region, m.metallic));
        block.pbr2[3] = m.has_normal_tex ? 1.0f : 0.0f;
        block.texFlags[0] = m.has_base_color_tex ? 1.0f : 0.0f;
        block.texFlags[1] = m.has_mr_tex ? 1.0f : 0.0f;
        block.texFlags[2] = m.has_ao_tex ? 1.0f : 0.0f;
        block.texFlags[3] = atlasUploaded_ ? 1.0f : 0.0f;
        block.region[0] = float(sm.region);
        block.region[1] = (highlightRegion_ >= 0 && sm.region == highlightRegion_) ? 1.0f : 0.0f;
        /* Eye regions additionally carry the uv extent of their atlas island plus a flag: the eye
           shader needs it to turn a refraction offset in metres into an atlas offset, so the iris
           parallax is right at whatever texture resolution the projector chose. */
        block.region[2] = irisUvExtent(engine_, sm);
        block.region[3] = (block.pbr2[2] == float(AF_KIND_EYE)) ? 1.0f : 0.0f;
        glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(i) * materialStride_, sizeof(block), &block);
    }
    MaterialUbo ground{};
    ground.baseColorFactor[0] = ground.baseColorFactor[1] = ground.baseColorFactor[2] = 0.32f;
    ground.baseColorFactor[3] = 1.0f;
    ground.pbr[1] = 0.75f;
    ground.pbr[2] = 1.5f;
    ground.pbr2[2] = float(AF_KIND_GENERIC);
    ground.region[0] = -1.0f;
    glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(submeshCount) * materialStride_, sizeof(ground), &ground);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    materialCount_ = submeshCount;
}

void GlRenderer::buildSkeletonLines() {
    lineVertexCount_ = 0;
    if (!engine_ || !skeletonVisible_) return;
    const int J = af_engine_joint_count(engine_);
    if (J <= 0) return;
    std::vector<float> matrices(size_t(J) * 16);
    if (af_engine_joint_matrices(engine_, matrices.data(), J) != AF_OK) return;

    std::vector<float> verts;
    verts.reserve(size_t(J) * 2 * 7);
    for (int j = 0; j < J; ++j) {
        af_joint_info info{};
        if (af_engine_joint_info(engine_, j, &info) != AF_OK) continue;
        if (info.parent < 0 || info.parent >= J) continue;
        const float* a = &matrices[size_t(j) * 16];
        const float* b = &matrices[size_t(info.parent) * 16];
        float rgb[3] = {0.35f, 0.72f, 1.00f};
        if (info.flags & AF_JOINT_FACE) { rgb[0] = 1.00f; rgb[1] = 0.62f; rgb[2] = 0.18f; }
        if (info.flags & AF_JOINT_FINGER) { rgb[0] = 0.30f; rgb[1] = 1.00f; rgb[2] = 0.78f; }
        if (info.flags & AF_JOINT_TWIST) { rgb[0] = 0.78f; rgb[1] = 0.42f; rgb[2] = 1.00f; }
        verts.insert(verts.end(), {a[12], a[13], a[14], rgb[0], rgb[1], rgb[2], 1.0f});
        verts.insert(verts.end(), {b[12], b[13], b[14], rgb[0], rgb[1], rgb[2], 0.65f});
    }
    lineVertexCount_ = uint32_t(verts.size() / 7);
    if (lineVertexCount_ == 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    const GLsizeiptr bytes = GLsizeiptr(verts.size() * sizeof(float));
    if (GLint64(bytes) > lineCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, verts.data(), GL_DYNAMIC_DRAW);
        lineCapacity_ = bytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, verts.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* The VAO captured the buffer binding at creation time (it was created before the first
       upload), which is fine because glVertexAttribPointer stores the binding, not the data. */
}

/* ============================================================================================
 *  Per-frame
 * ============================================================================================ */
void GlRenderer::updateScene(float deltaSeconds) {
    if (diagnosticRung_ == DiagnosticRung::SKINNED_MESH ||
        diagnosticRung_ == DiagnosticRung::BLENDSHAPE_MESH) {
        if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0f)
            diagnosticTimeSeconds_ += std::min(deltaSeconds, 0.1f);
    }
    env_.sunAzimuthDeg = options_.sunAzimuthDeg;
    env_.sunElevationDeg = options_.sunElevationDeg;
    env_.sunIntensity = options_.sunIntensity;
    env_.turbidity = options_.turbidity;
    env_.groundAlbedo = options_.groundAlbedo;
    env_.intensity = options_.environmentIntensity;
    projectToSh(env_, &sh_, 1024);

    modelBounds(engine_, scene_.modelMin, scene_.modelMax, &scene_.hasModel);
    /* depthZeroToOne = false: GL clip space, which is also why the ES dialect of the shaders has
       AF_CLIP_Y = +1 and rescales the shadow lookup's z. */
    buildCameraUbo(&scene_, camera_, options_, env_, sh_, surfaceWidth_, surfaceHeight_);
    if (diagnosticRung_ != DiagnosticRung::PRODUCTION_AVATAR)
        scene_.camera.envFlags[0] = 0.0f;  /* diagnostic fixtures do not sample the production shadow map */
    scene_.camera.envFlags[3] = flatShading_ ? 1.0f : 0.0f;
    scene_.camera.uiParams[0] = float(highlightRegion_);
    scene_.camera.uiParams[1] = highlightRegion_ >= 0 ? 1.0f : 0.0f;
    scene_.camera.uiParams[2] = float(options_.shadowSoftness) /
                                float(options_.shadowMapSize ? options_.shadowMapSize : 2048);
    scene_.camera.uiParams[3] = 1.0f / float(surfaceHeight_ ? surfaceHeight_ : 1);

    glBindBuffer(GL_UNIFORM_BUFFER, cameraUbo_);
    const int frame = int(stats_.frames % 2);
    glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(frame) * cameraStride_, sizeof(CameraUbo), &scene_.camera);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    /* Post-processing uniform slices. */
    if (bloomLevels_ > 0) {
        PostUbo p{};
        const float hdrThreshold = (hdrInternalFormat_ == GL_RGBA16F) ? 1.0f : 0.62f;
        for (int i = 0; i < bloomLevels_; ++i) {
            p.params[0] = (i == 0) ? hdrThreshold : 0.0f;
            p.params[1] = 0.5f;
            p.params[2] = p.params[3] = 0.0f;
            const int sw = (i == 0) ? surfaceWidth_ : bloomWidth_[i - 1];
            const int sh = (i == 0) ? surfaceHeight_ : bloomHeight_[i - 1];
            p.texel[0] = 1.0f / float(sw ? sw : 1);
            p.texel[1] = 1.0f / float(sh ? sh : 1);
            p.texel[2] = p.texel[3] = 0.0f;
            glBindBuffer(GL_UNIFORM_BUFFER, postUbo_);
            glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(i) * uboAlignment_, sizeof(PostUbo), &p);
        }
        for (int j = 0; j + 1 < bloomLevels_; ++j) {
            p.params[0] = p.params[1] = p.params[2] = p.params[3] = 0.0f;
            p.texel[0] = 1.0f / float(bloomWidth_[j] ? bloomWidth_[j] : 1);
            p.texel[1] = 1.0f / float(bloomHeight_[j] ? bloomHeight_[j] : 1);
            p.texel[2] = p.texel[3] = 0.0f;
            glBindBuffer(GL_UNIFORM_BUFFER, postUbo_);
            glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(bloomLevels_ + j) * uboAlignment_,
                            sizeof(PostUbo), &p);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    PostUbo c{};
    c.params[2] = options_.enableBloom ? 0.055f : 0.0f;
    c.params[3] = options_.exposure;
    c.texel[0] = 1.0f / float(surfaceWidth_ ? surfaceWidth_ : 1);
    c.texel[1] = 1.0f / float(surfaceHeight_ ? surfaceHeight_ : 1);
    glBindBuffer(GL_UNIFORM_BUFFER, postUbo_);
    glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(2 * bloomLevels_) * uboAlignment_, sizeof(PostUbo), &c);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, postUbo_);

    rebuildMaterialBlocks();
    buildSkeletonLines();
}

void GlRenderer::updateAnimation(float deltaSeconds) {
    if (!engine_ || clip_ < 0) return;
    const float duration = af_engine_clip_duration(engine_, clip_);
    if (duration <= 0.0f) return;
    clipTime_ += deltaSeconds * clipSpeed_;
    if (clipLoop_) {
        clipTime_ = std::fmod(clipTime_, duration);
        if (clipTime_ < 0.0f) clipTime_ += duration;
    } else if (clipTime_ > duration) {
        clipTime_ = duration;
    }
    if (af_engine_apply_clip(engine_, clip_, clipTime_) == AF_OK) meshDirty_ = true;
}

void GlRenderer::setClip(int clip, float speed, bool loop) {
    clip_ = clip;
    clipSpeed_ = speed;
    clipLoop_ = loop;
    clipTime_ = 0.0f;
    if (engine_ && clip_ >= 0 && af_engine_apply_clip(engine_, clip_, 0.0f) == AF_OK) meshDirty_ = true;
}

void GlRenderer::setDiagnosticRung(int rung) {
    if (!diagnosticRungValid(rung)) return;
    const DiagnosticRung next = static_cast<DiagnosticRung>(rung);
    if (next == diagnosticRung_) return;
    const DiagnosticRung previous = diagnosticRung_;
    diagnosticRung_ = next;
    diagnosticTimeSeconds_ = 0.0f;
    diagnosticDirty_ = true;
    meshDirty_ = true;
    if (next == DiagnosticRung::PRODUCTION_AVATAR && previous != next && engine_)
        rebuildMaterialBlocks();
}

/* ============================================================================================
 *  Draw
 * ============================================================================================ */
void GlRenderer::drawShadowPass() {
    if (diagnosticRung_ != DiagnosticRung::PRODUCTION_AVATAR ||
        !options_.enableShadows || !engine_ || indexCount_ == 0) return;
    const int size = options_.shadowMapSize;
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, size, size);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* Slope-scaled bias is not part of ES; the equivalent constant is applied here and the
       normal-offset in the shader handles the rest. glPolygonOffset does exactly this. */
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.25f, 1.75f);
    glUseProgram(shadowProgram_);
    glBindVertexArray(avatarVao_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glDisable(GL_POLYGON_OFFSET_FILL);
    ++drawCalls_;
}

void GlRenderer::drawForwardPass() {
    const int frame = int(stats_.frames % 2);
    glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo_);
    glViewport(0, 0, surfaceWidth_, surfaceHeight_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClearColor(0.055f, 0.075f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBindBufferRange(GL_UNIFORM_BUFFER, 0, cameraUbo_, GLintptr(frame) * cameraStride_, sizeof(CameraUbo));

    /* ---- studio ground ---- */
    if (diagnosticRung_ == DiagnosticRung::PRODUCTION_AVATAR &&
        options_.groundPlane && engine_ && vertexCount_ > 0) {
        glDisable(GL_CULL_FACE);   /* the floor quad is visible from above and below */
        glUseProgram(groundProgram_);
        glBindVertexArray(groundVao_);
        glBindBuffer(GL_ARRAY_BUFFER, groundVbo_);
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, materialUbo_,
                          GLintptr(materialCount_) * materialStride_, sizeof(MaterialUbo));
        glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        glActiveTexture(GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glEnable(GL_CULL_FACE);
        ++drawCalls_;
    }

    /* ---- the avatar: one draw per submesh so each gets its own material block ---- */
    if (diagnosticRung_ == DiagnosticRung::PRODUCTION_AVATAR &&
        engine_ && vertexCount_ > 0 && indexCount_ > 0) {
        glUseProgram(forwardProgram_);
        glBindVertexArray(avatarVao_);
        glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);

        for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(GL_TEXTURE_2D, atlasTexture_[slot]);
        }
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        glActiveTexture(GL_TEXTURE0);

        const af_submesh* submeshes = af_engine_submeshes(engine_);
        const int n = af_engine_submesh_count(engine_);
        for (int i = 0; i < n; ++i) {
            glBindBufferRange(GL_UNIFORM_BUFFER, 1, materialUbo_, GLintptr(i) * materialStride_,
                              sizeof(MaterialUbo));
            const int offset = submeshes[i].index_offset;
            glDrawElements(GL_TRIANGLES, submeshes[i].index_count, GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(size_t(offset) * sizeof(uint32_t)));
            ++drawCalls_;
        }
        triangles_ = indexCount_ / 3;
    } else if (diagnosticRung_ != DiagnosticRung::CLEAR &&
               diagnosticRung_ != DiagnosticRung::PRODUCTION_AVATAR && vertexCount_ > 0) {
        writeDiagnosticMaterial();
        glUseProgram(forwardProgram_);
        glBindVertexArray(avatarVao_);
        glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
        for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(GL_TEXTURE_2D, atlasTexture_[slot]);
        }
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        glActiveTexture(GL_TEXTURE0);
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, materialUbo_, 0, sizeof(MaterialUbo));
        if (diagnosticMesh_.indexed && indexCount_ > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
            glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
            triangles_ = indexCount_ / 3;
        } else {
            glDrawArrays(GL_TRIANGLES, 0, vertexCount_);
            triangles_ = vertexCount_ / 3;
        }
        ++drawCalls_;
    }

    /* ---- skeleton overlay ---- */
    if (diagnosticRung_ == DiagnosticRung::PRODUCTION_AVATAR &&
        skeletonVisible_ && lineVertexCount_ >= 2) {
        glUseProgram(lineProgram_);
        glBindVertexArray(lineVao_);
        glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        glDrawArrays(GL_LINES, 0, GLsizei(lineVertexCount_));
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        ++drawCalls_;
    }

    /* ---- resolve the MSAA buffer into the HDR texture ---- */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hdrFbo_);
    glBlitFramebuffer(0, 0, surfaceWidth_, surfaceHeight_, 0, 0, surfaceWidth_, surfaceHeight_,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer::drawBloom() {
    if (bloomLevels_ <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glBindVertexArray(emptyVao_);

    for (int i = 0; i < bloomLevels_; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[i]);
        glViewport(0, 0, bloomWidth_[i], bloomHeight_[i]);
        glUseProgram(bloomDownProgram_);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, (i == 0) ? hdrTexture_ : bloomTexture_[i - 1]);
        glBindBufferRange(GL_UNIFORM_BUFFER, 2, postUbo_, GLintptr(i) * uboAlignment_, sizeof(PostUbo));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        ++drawCalls_;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = 0; i + 1 < bloomLevels_; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[i]);
        glViewport(0, 0, bloomWidth_[i], bloomHeight_[i]);
        glUseProgram(bloomUpProgram_);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, bloomTexture_[i + 1]);
        glBindBufferRange(GL_UNIFORM_BUFFER, 2, postUbo_, GLintptr(bloomLevels_ + i) * uboAlignment_,
                          sizeof(PostUbo));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        ++drawCalls_;
    }
    glDisable(GL_BLEND);
}

void GlRenderer::drawComposite() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, surfaceWidth_, surfaceHeight_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glBindVertexArray(emptyVao_);
    glUseProgram(compositeProgram_);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, hdrTexture_);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, bloomLevels_ > 0 ? bloomTexture_[0] : hdrTexture_);
    glActiveTexture(GL_TEXTURE0);
    glBindBufferRange(GL_UNIFORM_BUFFER, 2, postUbo_, GLintptr(2 * bloomLevels_) * uboAlignment_,
                      sizeof(PostUbo));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++drawCalls_;
    /* The window's default framebuffer has no depth; restore the state the next frame expects. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void GlRenderer::renderFrame(float deltaSeconds) {
    if (contextLost_ || display_ == EGL_NO_DISPLAY) {
        if (frameError_.empty()) frameError_ = "EGL context lost or display unavailable";
        return;
    }
    lastFrameResizeOutcome_ = {ResizeStatus::RESIZED, false, surfaceWidth_, surfaceHeight_,
                               surfaceWidth_, surfaceHeight_, {}};
    frameError_.clear();
    ++stats_.frameAttempts;
    if (targetsDirty_) {
        targetsDirty_ = false;
        const RendererOptions rollback = activeTargetOptions_;
        destroyTargets();
        std::string error;
        if (createTargets(&error)) {
            activeTargetOptions_ = options_;
        } else {
            /* A quality transition is never allowed to turn a valid frame into an empty stage.
               Clean partial allocations and reconstruct the last known-good target set in the same
               frame. Only a lost EGL context may stop drawing. */
            destroyTargets();
            options_ = rollback;
            std::string rollbackError;
            if (!createTargets(&rollbackError)) {
                contextLost_ = true;
                return;
            }
            activeTargetOptions_ = options_;
        }
    }

    drawCalls_ = 0;
    triangles_ = 0;

    /* A resize between frames walks the whole target set; the window surface itself is resized by
       the compositor and picked up through eglQuerySurface. */
    int w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    if (w > 0 && h > 0 && (w != surfaceWidth_ || h != surfaceHeight_)) {
        lastFrameResizeOutcome_ = resizeWithResult(w, h);
        if (lastFrameResizeOutcome_.status != ResizeStatus::RESIZED) {
            frameError_ = lastFrameResizeOutcome_.diagnostic;
            return;
        }
    }

    updateScene(deltaSeconds);
    updateAnimation(deltaSeconds);
    ensureGeometryBuffers();

    gpuTimer_.poll(stats_);
    gpuTimer_.begin();
    drawShadowPass();
    drawForwardPass();
    drawBloom();
    drawComposite();
    gpuTimer_.end();
    ++stats_.frameSubmissions;

    std::string frameError;
    if (!checkGlError("GLES frame", &frameError)) {
        frameError_ = frameError;
        contextLost_ = true;
        __android_log_print(ANDROID_LOG_ERROR, "AvatarForgeRenderer", "%s", frameError.c_str());
        return;
    }

    if (!eglSwapBuffers(display_, surface_)) {
        const EGLint err = eglGetError();
        frameError_ = "eglSwapBuffers failed (EGL error " + std::to_string(int(err)) + ")";
        contextLost_ = true;
        return;
    }

    ++stats_.presentRequestsAccepted;
    ++stats_.frames;
    updateFrameLoopTimingEstimate(stats_, deltaSeconds);
    stats_.drawCalls = drawCalls_;
    stats_.triangles = triangles_;
    stats_.vertices = vertexCount_;
}

/* ============================================================================================
 *  Interface plumbing and teardown
 * ============================================================================================ */
void GlRenderer::setOptions(const RendererOptions& options) {
    const RendererOptions sanitized = sanitizeOptions(options);
    const bool needsRebuild =
        sanitized.msaaSamples != options_.msaaSamples ||
        sanitized.enableShadows != options_.enableShadows ||
        (sanitized.enableShadows && sanitized.shadowMapSize != options_.shadowMapSize) ||
        sanitized.enableBloom != options_.enableBloom ||
        (sanitized.enableBloom && sanitized.bloomLevels != options_.bloomLevels);
    options_ = sanitized;
    if (needsRebuild && context_ != EGL_NO_CONTEXT) targetsDirty_ = true;
}

void GlRenderer::attachEngine(af_engine* engine) {
    engine_ = engine;
    meshDirty_ = true;
    atlasUploaded_ = false;
    if (engine_ && context_ != EGL_NO_CONTEXT) {
        std::string error;
        uploadAtlas(&error);
        rebuildMaterialBlocks();
    }
}

void GlRenderer::invalidateMesh() { meshDirty_ = true; }

bool GlRenderer::resize(int width, int height) {
    return resizeWithResult(width, height).status == ResizeStatus::RESIZED;
}

ResizeOutcome GlRenderer::resizeWithResult(int width, int height) {
    ResizeOutcome result;
    result.attempted = true;
    result.surfaceWidth = width;
    result.surfaceHeight = height;
    if (contextLost_ || display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT) {
        if (frameError_.empty()) frameError_ = "GLES resize failed: context or display unavailable";
        result.status = ResizeStatus::FATAL;
        result.targetWidth = 0;
        result.targetHeight = 0;
        result.diagnostic = frameError_;
        return result;
    }
    frameError_.clear();
    if (width <= 0 || height <= 0) {
        frameError_ = "GLES resize failed: invalid surface dimensions " +
                      std::to_string(width) + "x" + std::to_string(height);
        result.status = ResizeStatus::FATAL;
        result.targetWidth = surfaceWidth_;
        result.targetHeight = surfaceHeight_;
        result.diagnostic = frameError_;
        return result;
    }
    if (width == surfaceWidth_ && height == surfaceHeight_) {
        result.status = ResizeStatus::RESIZED;
        result.targetWidth = surfaceWidth_;
        result.targetHeight = surfaceHeight_;
        return result;
    }

    const int previousWidth = surfaceWidth_ > 0 ? surfaceWidth_ : 1080;
    const int previousHeight = surfaceHeight_ > 0 ? surfaceHeight_ : 1920;
    const RendererOptions previousOptions = activeTargetOptions_;
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    destroyTargets();
    std::string error;
    if (createTargets(&error)) {
        activeTargetOptions_ = options_;
        result.status = ResizeStatus::RESIZED;
        result.targetWidth = surfaceWidth_;
        result.targetHeight = surfaceHeight_;
        return result;
    }

    destroyTargets();
    surfaceWidth_ = previousWidth;
    surfaceHeight_ = previousHeight;
    options_ = previousOptions;

    std::string restoreError;
    if (createTargets(&restoreError)) {
        activeTargetOptions_ = previousOptions;
        frameError_ = "GLES resize target allocation failed: " +
                      (error.empty() ? std::string("unspecified failure") : error) +
                      "; previous targets restored";
        result.status = ResizeStatus::PREVIOUS_TARGETS_RESTORED;
        result.targetWidth = surfaceWidth_;
        result.targetHeight = surfaceHeight_;
    } else {
        destroyTargets();
        contextLost_ = true;
        frameError_ = "GLES resize target allocation failed: " +
                      (error.empty() ? std::string("unspecified failure") : error) +
                      "; previous targets could not be restored: " +
                      (restoreError.empty() ? std::string("unspecified failure") : restoreError);
        result.status = ResizeStatus::FATAL;
        result.targetWidth = 0;
        result.targetHeight = 0;
    }
    result.diagnostic = frameError_;
    return result;
}

void GlRenderer::destroyTargets() {
    auto dropFbo = [](GLuint* fbo, GLuint* tex) {
        if (*fbo) { glDeleteFramebuffers(1, fbo); *fbo = 0; }
        if (*tex) { glDeleteTextures(1, tex); *tex = 0; }
    };
    dropFbo(&hdrFbo_, &hdrTexture_);
    dropFbo(&shadowFbo_, &shadowTexture_);
    for (int i = 0; i < 5; ++i) dropFbo(&bloomFbo_[i], &bloomTexture_[i]);
    if (msaaFbo_) { glDeleteFramebuffers(1, &msaaFbo_); msaaFbo_ = 0; }
    if (msaaColorRb_) { glDeleteRenderbuffers(1, &msaaColorRb_); msaaColorRb_ = 0; }
    if (msaaDepthRb_) { glDeleteRenderbuffers(1, &msaaDepthRb_); msaaDepthRb_ = 0; }
}

void GlRenderer::destroyAll() {
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
        eglMakeCurrent(display_, surface_, surface_, context_)) {
        gpuTimer_.destroy();
        destroyTargets();
        const GLuint programs[7] = {forwardProgram_, shadowProgram_, groundProgram_, lineProgram_,
                                    bloomDownProgram_, bloomUpProgram_, compositeProgram_};
        for (GLuint p : programs) if (p) glDeleteProgram(p);
        const GLuint buffers[6] = {cameraUbo_, postUbo_, materialUbo_, vertexVbo_, indexIbo_, groundVbo_};
        for (GLuint b : buffers) if (b) glDeleteBuffers(1, &b);
        if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
        for (int i = 0; i < AF_TEX_COUNT; ++i) if (atlasTexture_[i]) glDeleteTextures(1, &atlasTexture_[i]);
        const GLuint vaos[4] = {avatarVao_, groundVao_, emptyVao_, lineVao_};
        for (GLuint v : vaos) if (v) glDeleteVertexArrays(1, &v);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (display_ != EGL_NO_DISPLAY) {
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
}

GlRenderer::~GlRenderer() { destroyAll(); }

} /* namespace */

Renderer* createGlRenderer(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    GlRenderer* r = new GlRenderer();
    if (!r->init(window, options, error)) {
        delete r;
        return nullptr;
    }
    return r;
}

} /* namespace af */
