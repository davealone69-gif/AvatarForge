/*
 * ============================================================================================
 *  AvatarForge :: af_gl.cpp  --  the fallback renderer: EGL + OpenGL ES 3.1
 * ============================================================================================
 *
 *  Same shading model, same shader source, same frame structure as the Vulkan backend - this file
 *  is the other half of "runs on any Android device the app installs on":
 *
 *      EGL window surface -> MSAA HDR framebuffer -> resolve to HDR texture
 *                         -> bloom chain (down + additive up)
 *                         -> composite (exposure, ACES, vignette, dither, sRGB) -> window
 *      plus a depth-only shadow pass with a comparison-sampler map, exactly as in af_vk.cpp.
 *
 *  Differences from the Vulkan path, all of them API-mandated and commented where they matter:
 *      * objects are bound to the context instead of recorded into command buffers
 *      * multisampling is a framebuffer property here, so the HDR attachment is a multisampled
 *        renderbuffer that is blitted into a texture (GLES has no resolve-at-end-of-pass)
 *      * the Y direction is not flipped: GL's clip space already matches the shader's expectation
 *        (AF_CLIP_Y is +1 in the ES dialect), and the depth range is [-1,1] rather than [0,1]
 *      * glGenerateMipmap is used instead of a blit chain
 *
 *  The backend refuses to start below OpenGL ES 3.1 rather than silently degrading: the app's
 *  contract says the renderer needs a 3.1-class GPU, and pretending otherwise would ship a
 *  different (worse) image than the one the user was promised.
 */
#include "af_render.h"
#include "af_avatar_culler.h"
#include "af_avatar_lod.h"
#include "af_render_common.h"
#include "af_environment.h"
#include "af_shaders.h"
#include "af_texture_compression.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>            /* EGL_OPENGL_ES3_BIT_KHR */
#include <GLES3/gl31.h>
#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <android/log.h>

#ifndef GL_COMPRESSED_RGBA_ASTC_6x6_KHR
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#endif
#ifndef GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR 0x93D4
#endif

namespace af {
namespace {

bool glHasExtension(const char* wanted) {
    if (!wanted || !*wanted) return false;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    if (count > 0) {
        for (GLint i = 0; i < count; ++i) {
            const char* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
            if (extension && std::strcmp(extension, wanted) == 0) return true;
        }
        return false;
    }
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!extensions) return false;
    const size_t length = std::strlen(wanted);
    for (const char* at = extensions; (at = std::strstr(at, wanted)) != nullptr; ++at) {
        const bool left = at == extensions || at[-1] == ' ';
        const bool right = at[length] == '\0' || at[length] == ' ';
        if (left && right) return true;
    }
    return false;
}

bool glHasCompressedFormat(GLenum wanted) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
    if (count <= 0 || count > 4096) return false;
    std::vector<GLint> formats(static_cast<size_t>(count));
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats.data());
    return std::find(formats.begin(), formats.end(), static_cast<GLint>(wanted)) != formats.end();
}

uint64_t uncompressedAtlasFootprint(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) return 0;
    uint64_t total = 0;
    int w = width, h = height;
    for (int level = 0; level < 12; ++level) {
        const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
        if (pixels > (UINT64_MAX - total) / static_cast<uint64_t>(channels)) return UINT64_MAX;
        total += pixels * static_cast<uint64_t>(channels);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

bool compileShader(GLenum type, const char* source, const char* label, GLuint* out, std::string* error) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        if (error) *error = std::string("glCreateShader failed for ") + label;
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
        if (error) *error = std::string(label) + ": " + log;
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
    void attachEngine(af_engine* engine) override;
    void invalidateMesh() override;
    void setCamera(const CameraState& camera) override { camera_ = camera; }
    void setOptions(const RendererOptions& options) override;
    RendererOptions options() const override { return options_; }
    GpuProfile gpuProfile() const override { return gpuProfile_; }
    void renderFrame(float deltaSeconds) override;
    void setSkeletonVisible(bool visible) override { skeletonVisible_ = visible; }
    void setFlatShading(bool flat) override { flatShading_ = flat; }
    void setHighlightRegion(int region) override { highlightRegion_ = region; }
    void setClip(int clip, float speed, bool loop) override;
    int currentClip() const override { return clip_; }
    float clipTime() const override { return clipTime_; }
    void waitIdle() override { glFinish(); }
    RendererStats stats() const override { return stats_; }
    RendererInfo info() const override { return info_; }

private:
    bool createContext(ANativeWindow* window, std::string* error);
    bool createPrograms(std::string* error);
    bool createTargets(std::string* error);
    bool createAtlas(std::string* error);
    void uploadAtlas(std::string* error);
    void startAtlasCompression();
    void pollAtlasCompression();
    void uploadCompressedAtlas(const EncodedAtlas& atlas);
    void waitAtlasCompression();
    void ensureGeometryBuffers();
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
                                    GLenum format, GLenum type, bool depth);

    GLuint linkProgram(const char* vsName, const char* fsName, std::string* error);

    /* ---- EGL / context ---- */
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    uint32_t uboAlignment_ = 256;

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
    GpuProfile gpuProfile_{};
    AsyncAtlasCompression atlasCompressionJob_;
    bool atlasCompressionAttempted_ = false;
    uint64_t atlasGpuBytes_ = 0;
    af_engine* engine_ = nullptr;
    CameraState camera_{};
    AvatarCuller avatarCuller_;
    AvatarLodSet avatarLods_;
    int activeLod_ = 0;
    bool indexDirty_ = true;
    bool avatarVisible_ = false;
    FrameScene scene_{};
    EnvironmentParams env_{};
    ShGlobal sh_{};
    bool meshDirty_ = true;
    int highlightRegion_ = -1;
    bool skeletonVisible_ = false;
    bool flatShading_ = false;
    int clip_ = -1;
    float clipSpeed_ = 1.0f;
    bool clipLoop_ = true;
    float clipTime_ = 0.0f;
    int32_t vertexCount_ = 0;
    int32_t indexCount_ = 0;
    uint32_t materialStride_ = 256;
    int materialCount_ = 0;
    uint32_t lineVertexCount_ = 0;
    int drawCalls_ = 0;
    int triangles_ = 0;
    bool targetsDirty_ = false;
    bool contextLost_ = false;
};

/* ============================================================================================
 *  Context
 * ============================================================================================ */
bool GlRenderer::createContext(ANativeWindow* window, std::string* error) {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) { *error = "eglGetDisplay failed"; return false; }
    if (!eglInitialize(display_, nullptr, nullptr)) { *error = "eglInitialize failed"; return false; }

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
    if (!eglChooseConfig(display_, configAttribs, &config_, 1, &configCount) || configCount == 0) {
        *error = "no EGL config with an ES3 renderable type and a 24-bit depth buffer";
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) { *error = "eglCreateContext(ES3) failed"; return false; }

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) { *error = "eglCreateWindowSurface failed"; return false; }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        *error = "eglMakeCurrent failed";
        return false;
    }
    eglQuerySurface(display_, surface_, EGL_WIDTH, &surfaceWidth_);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfaceHeight_);

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

    /* Floating-point colour attachments: ES 3.2 has them in core, ES 3.0/3.1 need the extension. */
    bool floatColour = (major > 3 || (major == 3 && minor >= 2));
    if (!floatColour) {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        floatColour = ext && std::strstr(ext, "GL_EXT_color_buffer_float") != nullptr;
    }
    hdrInternalFormat_ = floatColour ? GL_RGBA16F : GL_RGBA8;

    GLint maxTextureSize = 0, maxSamples = 1;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    const bool astcExtension = glHasExtension("GL_KHR_texture_compression_astc_ldr") ||
                               glHasExtension("GL_OES_texture_compression_astc");
    const bool hasEtc2 = glHasCompressedFormat(GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC) &&
                         glHasCompressedFormat(GL_COMPRESSED_RGBA8_ETC2_EAC) &&
                         glHasCompressedFormat(GL_COMPRESSED_R11_EAC);
    const bool hasAstc6x6 = astcExtension &&
                            glHasCompressedFormat(GL_COMPRESSED_RGBA_ASTC_6x6_KHR) &&
                            glHasCompressedFormat(GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR);

    GpuDeviceCaps caps;
    caps.backend = "gles31";
    caps.vendorName = vendor ? vendor : "unknown vendor";
    caps.deviceName = rendererName ? rendererName : "unknown GPU";
    caps.driver = version;
    caps.maxTextureDimension2D = static_cast<uint32_t>(std::max(0, maxTextureSize));
    caps.maxColorSamples = static_cast<uint32_t>(std::max(1, maxSamples));
    caps.supportsAstcLdr = hasAstc6x6;
    caps.supportsEtc2 = hasEtc2;
    caps.supportsFloatHdr = floatColour;
    caps.softwareRenderer = (rendererName && (std::strstr(rendererName, "SwiftShader") ||
                                               std::strstr(rendererName, "llvmpipe") ||
                                               std::strstr(rendererName, "Software")));
    gpuProfile_ = makeGpuProfile(caps);

    info_.backend = caps.backend;
    info_.deviceName = caps.deviceName;
    info_.apiVersion = version;
    info_.driver = caps.vendorName;
    info_.gpuProfile = gpuProfile_.name;
    info_.textureCompression = atlasCompressionName(AtlasCompression::NONE);
    info_.supportsFloatHdr = floatColour;
    info_.supportsMsaa = maxSamples > 1;
    info_.supportsShadows = maxTextureSize >= 1024;
    info_.supportsAnisotropy = glHasExtension("GL_EXT_texture_filter_anisotropic");
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

    /* Sampler units. The GLES dialect of common.glsl declares the samplers without a binding
       qualifier, so the units are assigned here, next to the code that binds them. */
    auto unit = [](GLuint p, const char* name, GLint value) {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform1i(loc, value);
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
                                            bool depth) {
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
    if (depth) {
        GLuint depthRb = 0;
        glGenRenderbuffers(1, &depthRb);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return status == GL_FRAMEBUFFER_COMPLETE;
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
                                   false)) {
        *error = "HDR framebuffer incomplete (float colour buffers unavailable)";
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
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        /* The device refused the requested sample count; drop both attachments to 1x and try once
           more. The old fallback accidentally allocated the depth renderbuffer with the HDR colour
           format, guaranteeing an incomplete FBO exactly when auto quality changed tier. */
        glBindRenderbuffer(GL_RENDERBUFFER, msaaDepthRb_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msaaDepthRb_);
        glDeleteRenderbuffers(1, &msaaColorRb_);
        glGenRenderbuffers(1, &msaaColorRb_);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaColorRb_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, hdrInternalFormat_, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColorRb_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            *error = "MSAA framebuffer incomplete even at 1x";
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
    glDrawBuffers(0, nullptr);                  /* depth-only: no colour attachment at all */
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        *error = "shadow framebuffer incomplete";
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
                                        false)) {
            *error = "bloom framebuffer incomplete";
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
    return true;
}

/* ============================================================================================
 *  Setup
 * ============================================================================================ */
bool GlRenderer::init(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    options_ = options;
    if (!createContext(window, error)) return false;
    options_.msaaSamples = std::min(std::max(1, options_.msaaSamples),
                                    static_cast<int>(gpuProfile_.maxMsaaSamples));
    options_.shadowMapSize = std::max(512, std::min(options_.shadowMapSize, gpuProfile_.maxShadowMapSize));
    options_.bloomLevels = std::max(0, std::min(options_.bloomLevels, gpuProfile_.maxBloomLevels));
    options_.effectiveQualityLevel = std::max(0, std::min(options_.effectiveQualityLevel,
                                                         gpuProfile_.maxQualityLevel));
    if (!createPrograms(error)) return false;

    glGenVertexArrays(1, &avatarVao_);
    glGenVertexArrays(1, &groundVao_);
    glGenVertexArrays(1, &emptyVao_);
    glGenVertexArrays(1, &lineVao_);

    /* Avatar VAO: the engine's interleaved af_vertex, stride 88. */
    glBindVertexArray(avatarVao_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexVbo_);
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

    /* Ground VAO: positions only. */
    glBindVertexArray(groundVao_);
    glBindBuffer(GL_ARRAY_BUFFER, groundVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
    glBindVertexArray(0);

    /* Skeleton VAO: position + colour. */
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 7, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 7,
                          reinterpret_cast<const void*>(sizeof(float) * 3));
    glBindVertexArray(0);

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

    glGenBuffers(1, &cameraUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(uboAlignment_) * 2, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &postUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, postUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(uboAlignment_) * 16, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &materialUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, materialUbo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(uboAlignment_) * 32, nullptr, GL_DYNAMIC_DRAW);
    materialCapacity_ = GLint64(uboAlignment_) * 32;
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    /* Atlas placeholders: 1x1 white, so the sampler bindings are always valid (the shaders gate
       every lookup on the material's has_*_tex flags). */
    const GLubyte white[4] = {255, 255, 255, 255};
    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        glGenTextures(1, &atlasTexture_[i]);
        glBindTexture(GL_TEXTURE_2D, atlasTexture_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (!createTargets(error)) return false;
    activeTargetOptions_ = options_;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    if (engine_) {
        uploadAtlas(error);
        rebuildMaterialBlocks();
    }
    return true;
}

/* ============================================================================================
 *  Geometry and atlas
 * ============================================================================================ */
void GlRenderer::ensureGeometryBuffers() {
    if (!engine_) return;
    const int32_t vcount = af_engine_vertex_count(engine_);
    const af_vertex* verts = af_engine_vertices(engine_);
    const int32_t sourceIndexCount = af_engine_index_count(engine_);
    const uint32_t* sourceIndices = af_engine_indices(engine_);
    if (vcount <= 0 || !verts) return;

    const GLint64 vbytes = GLint64(vcount) * VertexLayout::stride;
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

    const std::vector<uint32_t>& lodIndices = avatarLods_.indices();
    const int32_t icount = lodIndices.empty() ? sourceIndexCount : static_cast<int32_t>(lodIndices.size());
    const uint32_t* indices = lodIndices.empty() ? sourceIndices : lodIndices.data();
    if (icount > 0 && indices) {
        const GLint64 ibytes = GLint64(icount) * sizeof(uint32_t);
        if (indexIbo_ == 0) glGenBuffers(1, &indexIbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexIbo_);
        if (ibytes > indexCapacity_) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(ibytes + ibytes / 4), nullptr, GL_STATIC_DRAW);
            indexCapacity_ = ibytes + ibytes / 4;
            indexCount_ = 0;
        }
        if (indexDirty_ || indexCount_ != icount) {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(ibytes), indices);
            indexCount_ = icount;
            indexDirty_ = false;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    meshDirty_ = false;
}

void GlRenderer::uploadAtlas(std::string* error) {
    if (!engine_) return;
    atlasGpuBytes_ = 0;
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
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        int levels = 1, mw = tex.width, mh = tex.height;
        while ((mw > 1 || mh > 1) && levels < 12) {
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            ++levels;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        if (info_.supportsAnisotropy) {
            GLfloat maxAniso = 1.0f;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &maxAniso);
            glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */,
                            std::min(4.0f, maxAniso));
        }
        const uint64_t bytes = uncompressedAtlasFootprint(tex.width, tex.height, tex.channels);
        atlasGpuBytes_ = atlasGpuBytes_ > UINT64_MAX - bytes ? UINT64_MAX : atlasGpuBytes_ + bytes;
    }
    atlasUploaded_ = true;
    stats_.textureBytes = atlasGpuBytes_;
    startAtlasCompression();
    (void)error;
}

void GlRenderer::startAtlasCompression() {
    if (!engine_ || atlasCompressionAttempted_ || gpuProfile_.atlasCompression == AtlasCompression::NONE)
        return;
    atlasCompressionAttempted_ = true;
    std::array<af_texture, AF_TEX_COUNT> sources{};
    for (int slot = 0; slot < AF_TEX_COUNT; ++slot)
        (void)af_engine_texture(engine_, slot, &sources[static_cast<size_t>(slot)]);
    const bool started = atlasCompressionJob_.start(sources, gpuProfile_);
    info_.textureCompression = started
        ? std::string(atlasCompressionName(gpuProfile_.atlasCompression)) + " (pending)"
        : atlasCompressionName(AtlasCompression::NONE);
}

void GlRenderer::pollAtlasCompression() {
    EncodedAtlas atlas;
    if (!atlasCompressionJob_.poll(&atlas)) return;
    if (!atlas.anyValid()) {
        info_.textureCompression = atlasCompressionName(AtlasCompression::NONE);
        return;
    }
    uploadCompressedAtlas(atlas);
}

void GlRenderer::uploadCompressedAtlas(const EncodedAtlas& atlas) {
    if (!engine_ || context_ == EGL_NO_CONTEXT) return;
    bool uploadedAny = false;
    bool usedFallback = false;
    for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
        const EncodedTexture& texture = atlas.textures[static_cast<size_t>(slot)];
        if (!texture.valid()) continue;
        GLenum internal = 0;
        switch (texture.encoding) {
            case AtlasTextureEncoding::ASTC_6X6_UNORM: internal = GL_COMPRESSED_RGBA_ASTC_6x6_KHR; break;
            case AtlasTextureEncoding::ASTC_6X6_SRGB: internal = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR; break;
            case AtlasTextureEncoding::ETC2_RGBA8_SRGB: internal = GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC; break;
            case AtlasTextureEncoding::ETC2_RGBA8_UNORM: internal = GL_COMPRESSED_RGBA8_ETC2_EAC; break;
            case AtlasTextureEncoding::ETC2_R11_UNORM: internal = GL_COMPRESSED_R11_EAC; break;
        }
        bool ok = internal != 0;
        glBindTexture(GL_TEXTURE_2D, atlasTexture_[slot]);
        for (int attempt = 0; attempt < 16 && glGetError() != GL_NO_ERROR; ++attempt) {}
        if (ok) {
            for (size_t level = 0; level < texture.levels.size(); ++level) {
                const EncodedMip& mip = texture.levels[level];
                if (mip.bytes.empty() || mip.bytes.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max())) {
                    ok = false;
                    break;
                }
                glCompressedTexImage2D(GL_TEXTURE_2D, GLint(level), internal, mip.width, mip.height, 0,
                                       GLsizei(mip.bytes.size()), mip.bytes.data());
                if (glGetError() != GL_NO_ERROR) { ok = false; break; }
            }
        }
        if (!ok) {
            /* A driver-side compressed upload refusal is non-fatal: restore the raw base level and
               let GLES rebuild the mip chain exactly as it did before this optimization. */
            af_texture raw{};
            if (af_engine_texture(engine_, slot, &raw) == AF_OK && raw.data && raw.width > 0 && raw.height > 0) {
                const GLenum rawInternal = (raw.channels == 1) ? GL_R8
                    : (slot == AF_TEX_BASE_COLOR) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
                const GLenum rawFormat = (raw.channels == 1) ? GL_RED : GL_RGBA;
                glTexImage2D(GL_TEXTURE_2D, 0, GLint(rawInternal), raw.width, raw.height, 0, rawFormat,
                             GL_UNSIGNED_BYTE, raw.data);
                glGenerateMipmap(GL_TEXTURE_2D);
                int levels = 1, mw = raw.width, mh = raw.height;
                while ((mw > 1 || mh > 1) && levels < 12) {
                    mw = mw > 1 ? mw / 2 : 1;
                    mh = mh > 1 ? mh / 2 : 1;
                    ++levels;
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
                usedFallback = true;
            }
            continue;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(texture.levels.size() - 1));
        const uint64_t compressedBytes = texture.byteSize();
        const uint64_t rawBytes = [&]() {
            af_texture raw{};
            return af_engine_texture(engine_, slot, &raw) == AF_OK
                ? uncompressedAtlasFootprint(raw.width, raw.height, raw.channels) : uint64_t(0);
        }();
        if (atlasGpuBytes_ >= rawBytes) atlasGpuBytes_ -= rawBytes;
        atlasGpuBytes_ = atlasGpuBytes_ > UINT64_MAX - compressedBytes ? UINT64_MAX
                                                                       : atlasGpuBytes_ + compressedBytes;
        uploadedAny = true;
    }
    stats_.textureBytes = atlasGpuBytes_;
    if (uploadedAny) {
        info_.textureCompression = usedFallback ? "mixed compressed/raw fallback"
                                                : atlasCompressionName(gpuProfile_.atlasCompression);
    } else {
        info_.textureCompression = atlasCompressionName(AtlasCompression::NONE);
    }
}

void GlRenderer::waitAtlasCompression() { atlasCompressionJob_.waitDiscard(); }

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
        const af_material& m = (materials && sm.material_index >= 0 && sm.material_index < materialCount)
                                   ? materials[sm.material_index] : af_material{};
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
    (void)deltaSeconds;
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
    buildCameraUbo(&scene_, camera_, options_, env_, sh_, surfaceWidth_, surfaceHeight_, false);
    int lodIndexCounts[kAvatarLodLevels] = {};
    int lodCount = avatarLods_.levelCount();
    if (lodCount > 0) {
        for (int level = 0; level < lodCount; ++level)
            lodIndexCounts[level] = static_cast<int>(avatarLods_.level(level).indexCount);
    } else {
        lodCount = 1;
        lodIndexCounts[0] = engine_ ? std::max(0, af_engine_index_count(engine_)) : 0;
    }
    const float quality = static_cast<float>(std::max(0, std::min(3, options_.effectiveQualityLevel))) / 3.0f;
    const AvatarCullResult cull = avatarCuller_.update(scene_.modelMin, scene_.modelMax, scene_.hasModel,
                                                         lodIndexCounts, lodCount, camera_, surfaceWidth_,
                                                         surfaceHeight_, quality, gpuProfile_.lodDistanceScale);
    avatarVisible_ = cull.visible;
    activeLod_ = std::max(0, cull.lod);
    triangles_ = cull.triangleCount;
    scene_.camera.envFlags[3] = flatShading_ ? 1.0f : 0.0f;
    scene_.camera.uiParams[0] = float(highlightRegion_);
    scene_.camera.uiParams[1] = highlightRegion_ >= 0 ? 1.0f : 0.0f;
    scene_.camera.uiParams[2] = float(options_.shadowSoftness) /
                                float(options_.shadowMapSize ? options_.shadowMapSize : 2048);
    scene_.camera.uiParams[3] = 1.0f / float(surfaceHeight_ ? surfaceHeight_ : 1);

    glBindBuffer(GL_UNIFORM_BUFFER, cameraUbo_);
    const int frame = int(stats_.frames % 2);
    glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(frame) * uboAlignment_, sizeof(CameraUbo), &scene_.camera);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    /* Post slices, laid out exactly like the Vulkan path so the two cannot drift. */
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

/* ============================================================================================
 *  Draw
 * ============================================================================================ */
void GlRenderer::drawShadowPass() {
    if (!options_.enableShadows || !engine_ || indexCount_ == 0) return;
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
    const bool haveLods = avatarLods_.levelCount() > 0;
    const AvatarLodLevel& lod = avatarLods_.level(activeLod_);
    const GLsizei drawCount = haveLods ? static_cast<GLsizei>(lod.indexCount)
                                        : std::max(0, af_engine_index_count(engine_));
    const size_t first = haveLods ? lod.firstIndex : 0;
    glDrawElements(GL_TRIANGLES, drawCount, GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(first * sizeof(uint32_t)));
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

    glBindBufferRange(GL_UNIFORM_BUFFER, 0, cameraUbo_, GLintptr(frame) * uboAlignment_, sizeof(CameraUbo));

    /* ---- studio ground ---- */
    if (options_.groundPlane && engine_ && vertexCount_ > 0) {
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
        ++drawCalls_;
    }

    /* ---- the avatar: one draw per submesh so each gets its own material block ---- */
    const bool hasEngine = engine_ != nullptr;
    const bool hasVertices = vertexCount_ > 0;
    const bool hasIndices = indexCount_ > 0;
    if ((!avatarVisible_ || !hasEngine || !hasVertices || !hasIndices) && stats_.frames % 120 == 0) {
        __android_log_print(ANDROID_LOG_WARN, "AvatarForgeRender",
                            "Avatar draw skipped: visible=%d engine=%d uploadedVerts=%d uploadedIndices=%d "
                            "engineVerts=%d engineIndices=%d lod=%d lodLevels=%d sceneHasModel=%d",
                            avatarVisible_ ? 1 : 0, hasEngine ? 1 : 0, vertexCount_, indexCount_,
                            hasEngine ? af_engine_vertex_count(engine_) : 0,
                            hasEngine ? af_engine_index_count(engine_) : 0,
                            activeLod_, avatarLods_.levelCount(), scene_.hasModel ? 1 : 0);
    }
    if (avatarVisible_ && engine_ && vertexCount_ > 0 && indexCount_ > 0) {
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
        const bool haveLods = avatarLods_.levelCount() > 0;
        const AvatarLodLevel& lod = avatarLods_.level(activeLod_);
        for (int i = 0; i < n; ++i) {
            glBindBufferRange(GL_UNIFORM_BUFFER, 1, materialUbo_, GLintptr(i) * materialStride_,
                              sizeof(MaterialUbo));
            const bool haveRange = haveLods && static_cast<size_t>(i) < lod.submeshes.size();
            const uint32_t offset = haveRange ? lod.submeshes[static_cast<size_t>(i)].firstIndex
                                              : static_cast<uint32_t>(std::max(0, submeshes[i].index_offset));
            const GLsizei count = haveRange ? static_cast<GLsizei>(lod.submeshes[static_cast<size_t>(i)].indexCount)
                                            : std::max(0, submeshes[i].index_count);
            if (count > 0) {
                glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(size_t(offset) * sizeof(uint32_t)));
                const GLenum drawError = glGetError();
                if (drawError != GL_NO_ERROR) {
                    GLint boundVao = 0, boundElementBuffer = 0;
                    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &boundVao);
                    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &boundElementBuffer);
                    __android_log_print(ANDROID_LOG_ERROR, "AvatarForgeRender",
                                        "glDrawElements failed: error=0x%x count=%d offset=%u vao=%d ebo=%d",
                                        static_cast<unsigned int>(drawError), count, offset,
                                        boundVao, boundElementBuffer);
                }
                ++drawCalls_;
            } else if (stats_.frames % 120 == 0) {
                __android_log_print(ANDROID_LOG_WARN, "AvatarForgeRender",
                                    "Avatar submesh has no drawable indices: slot=%d count=%d offset=%u "
                                    "lod=%d lodIndexCount=%zu submeshCount=%d",
                                    i, static_cast<int>(count), offset, activeLod_,
                                    haveLods ? lod.indexCount : size_t(0), n);
            }
        }
        triangles_ = haveLods ? static_cast<int>(lod.indexCount / 3)
                              : std::max(0, af_engine_index_count(engine_)) / 3;
    }

    /* ---- skeleton overlay ---- */
    if (skeletonVisible_ && lineVertexCount_ >= 2) {
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
    if (contextLost_ || display_ == EGL_NO_DISPLAY) return;
    pollAtlasCompression();
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

    stats_.frames++;
    if (deltaSeconds > 0.0f && deltaSeconds < 1.0f) {
        const float alpha = 0.06f;
        stats_.frameMs = stats_.frameMs * (1.0f - alpha) + deltaSeconds * 1000.0f * alpha;
        stats_.fps = stats_.frameMs > 0.01f ? 1000.0f / stats_.frameMs : 0.0f;
    }
    drawCalls_ = 0;
    triangles_ = 0;

    /* A resize between frames walks the whole target set; the window surface itself is resized by
       the compositor and picked up through eglQuerySurface. */
    int w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    if (w > 0 && h > 0 && (w != surfaceWidth_ || h != surfaceHeight_)) {
        if (!resize(w, h)) return;
    }

    updateScene(deltaSeconds);
    updateAnimation(deltaSeconds);
    ensureGeometryBuffers();

    drawShadowPass();
    drawForwardPass();
    drawBloom();
    drawComposite();

    if (!eglSwapBuffers(display_, surface_)) {
        /* A lost context cannot be recovered by this backend: report it and stop drawing rather
           than rendering into nothing. AvatarEngine.health() surfaces the flag. */
        const EGLint err = eglGetError();
        if (err == EGL_CONTEXT_LOST || err == EGL_BAD_SURFACE) contextLost_ = true;
    }

    stats_.drawCalls = drawCalls_;
    stats_.triangles = triangles_;
    stats_.vertices = vertexCount_;
}

/* ============================================================================================
 *  Interface plumbing and teardown
 * ============================================================================================ */
void GlRenderer::setOptions(const RendererOptions& options) {
    RendererOptions normalized = options;
    normalized.msaaSamples = std::min(std::max(1, normalized.msaaSamples),
                                     static_cast<int>(gpuProfile_.maxMsaaSamples));
    normalized.shadowMapSize = std::max(512, std::min(normalized.shadowMapSize, gpuProfile_.maxShadowMapSize));
    normalized.bloomLevels = std::max(0, std::min(normalized.bloomLevels, gpuProfile_.maxBloomLevels));
    normalized.effectiveQualityLevel = std::max(0, std::min(normalized.effectiveQualityLevel,
                                                            gpuProfile_.maxQualityLevel));
    const bool needsRebuild =
        normalized.msaaSamples != options_.msaaSamples ||
        normalized.enableShadows != options_.enableShadows ||
        (normalized.enableShadows && normalized.shadowMapSize != options_.shadowMapSize) ||
        normalized.enableBloom != options_.enableBloom ||
        (normalized.enableBloom && normalized.bloomLevels != options_.bloomLevels);
    options_ = normalized;
    if (needsRebuild && context_ != EGL_NO_CONTEXT) targetsDirty_ = true;
}

void GlRenderer::attachEngine(af_engine* engine) {
    waitAtlasCompression();
    engine_ = engine;
    atlasCompressionAttempted_ = false;
    atlasGpuBytes_ = 0;
    stats_.textureBytes = 0;
    info_.textureCompression = atlasCompressionName(AtlasCompression::NONE);
    meshDirty_ = true;
    indexDirty_ = true;
    activeLod_ = 0;
    atlasUploaded_ = false;
    avatarLods_.clear();
    if (engine_) {
        const int32_t vertices = af_engine_vertex_count(engine_);
        const int32_t indices = af_engine_index_count(engine_);
        const af_vertex* rest = af_engine_vertices_rest(engine_);
        if (!rest) rest = af_engine_vertices(engine_);
        avatarLods_.build(rest, static_cast<size_t>(std::max(0, vertices)),
                          af_engine_indices(engine_), static_cast<size_t>(std::max(0, indices)),
                          af_engine_submeshes(engine_),
                          static_cast<size_t>(std::max(0, af_engine_submesh_count(engine_))));
    }
    if (engine_ && context_ != EGL_NO_CONTEXT) {
        std::string error;
        uploadAtlas(&error);
        rebuildMaterialBlocks();
    }
}

void GlRenderer::invalidateMesh() { meshDirty_ = true; }

bool GlRenderer::resize(int width, int height) {
    if (context_ == EGL_NO_CONTEXT || width <= 0 || height <= 0) return false;
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    destroyTargets();
    std::string error;
    if (!createTargets(&error)) return false;
    activeTargetOptions_ = options_;
    return true;
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
    waitAtlasCompression();
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(display_, surface_, surface_, context_);
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
