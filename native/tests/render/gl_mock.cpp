#include "gl_mock.h"
#include "afcore.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <sstream>

namespace {
GLuint nextName = 1;
GLint nextUniform = 10000;
void error(GLenum code) { af_test::state.glErrors.push_back(code); }
void names(GLsizei n, GLuint* out, int& live) { for (GLsizei i = 0; i < n; ++i) { out[i] = nextName++; ++live; } }
void drop(GLsizei n, const GLuint* out, int& live) { live -= n; (void)out; }
std::vector<std::string> extensionNames() {
    std::istringstream input(af_test::state.extensions);
    std::vector<std::string> names;
    for (std::string name; input >> name;) names.push_back(name);
    return names;
}
}
namespace af_test {
MockState state;
void reset() { state = MockState{}; nextName = 1; nextUniform = 10000; }
void raiseGlError(GLenum code) { error(code); }
}
using af_test::state;
using af_test::raiseGlError;

extern "C" {
EGLDisplay eglGetDisplay(EGLNativeDisplayType) { return reinterpret_cast<EGLDisplay>(uintptr_t(1)); }
EGLProc eglGetProcAddress(const char* name) {
    if (!name || !state.timerFunctionsAvailable) return nullptr;
    if (!std::strcmp(name, "glGenQueriesEXT")) return reinterpret_cast<EGLProc>(glGenQueriesEXT);
    if (!std::strcmp(name, "glDeleteQueriesEXT")) return reinterpret_cast<EGLProc>(glDeleteQueriesEXT);
    if (!std::strcmp(name, "glBeginQueryEXT")) return reinterpret_cast<EGLProc>(glBeginQueryEXT);
    if (!std::strcmp(name, "glEndQueryEXT")) return reinterpret_cast<EGLProc>(glEndQueryEXT);
    if (!std::strcmp(name, "glGetQueryObjectuivEXT")) return reinterpret_cast<EGLProc>(glGetQueryObjectuivEXT);
    if (!std::strcmp(name, "glGetQueryObjectui64vEXT")) return reinterpret_cast<EGLProc>(glGetQueryObjectui64vEXT);
    return nullptr;
}
EGLint eglGetError(void) { const EGLint e = state.eglError; state.eglError = EGL_SUCCESS; return e; }
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*) { return EGL_TRUE; }
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig* out, EGLint, EGLint* count) {
    *out = reinterpret_cast<EGLConfig>(uintptr_t(2)); *count = 1; return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) {
    if (state.fault == af_test::Fault::EglContext) { state.eglError = EGL_BAD_CONFIG; return EGL_NO_CONTEXT; }
    ++state.contextsCreated; return reinterpret_cast<EGLContext>(uintptr_t(3));
}
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) {
    ++state.surfacesCreated; return reinterpret_cast<EGLSurface>(uintptr_t(4));
}
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext context) { state.currentContext = context; return EGL_TRUE; }
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint key, EGLint* value) {
    *value = key == EGL_WIDTH ? state.surfaceWidth : state.surfaceHeight;
    return EGL_TRUE;
}
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { ++state.surfacesDestroyed; return EGL_TRUE; }
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { ++state.contextsDestroyed; return EGL_TRUE; }
EGLBoolean eglTerminate(EGLDisplay) { return EGL_TRUE; }
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface) {
    if (state.fault == af_test::Fault::SwapBuffers) { state.eglError = EGL_BAD_SURFACE; return EGL_FALSE; }
    ++state.swapBuffers;
    return EGL_TRUE;
}

GLenum glGetError(void) {
    if (state.glErrors.empty()) return GL_NO_ERROR;
    const GLenum e = state.glErrors.front(); state.glErrors.erase(state.glErrors.begin()); return e;
}
void glActiveTexture(GLenum) {}
void glAttachShader(GLuint, GLuint) {}
void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) state.currentArrayBuffer = buffer;
    if (target == GL_UNIFORM_BUFFER) state.currentUniformBuffer = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER && state.currentVao) state.vaos[state.currentVao].elementBuffer = buffer;
}
void glBindBufferBase(GLenum, GLuint, GLuint) {}
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if (target == GL_UNIFORM_BUFFER && index == 0) state.cameraBuffer = buffer;
    const auto it = state.bufferSizes.find(buffer);
    if (it == state.bufferSizes.end() || offset < 0 || size < 0 || offset + size > it->second || offset % 256 != 0) {
        raiseGlError(GL_INVALID_VALUE);
    }
}
void glBindFramebuffer(GLenum target, GLuint name) {
    state.currentFramebuffer = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) state.currentReadFramebuffer = name;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) state.currentDrawFramebuffer = name;
}
void glBindRenderbuffer(GLenum, GLuint name) { state.currentRenderbuffer = name; }
void glBindTexture(GLenum, GLuint name) { state.currentTexture = name; }
void glBindVertexArray(GLuint name) { state.currentVao = name; }
void glBlendFunc(GLenum, GLenum) {}
void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {}
void glBufferData(GLenum target, GLsizeiptr size, const void*, GLenum) {
    GLuint buffer = target == GL_ARRAY_BUFFER ? state.currentArrayBuffer :
                    target == GL_UNIFORM_BUFFER ? state.currentUniformBuffer :
                    target == GL_ELEMENT_ARRAY_BUFFER && state.currentVao ? state.vaos[state.currentVao].elementBuffer : 0;
    if (!buffer) raiseGlError(GL_INVALID_OPERATION);
    else state.bufferSizes[buffer] = size;
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void*) {
    GLuint buffer = target == GL_ARRAY_BUFFER ? state.currentArrayBuffer :
                    target == GL_UNIFORM_BUFFER ? state.currentUniformBuffer :
                    target == GL_ELEMENT_ARRAY_BUFFER && state.currentVao ? state.vaos[state.currentVao].elementBuffer : 0;
    const auto it = state.bufferSizes.find(buffer);
    if (it == state.bufferSizes.end() || offset < 0 || size < 0 || offset + size > it->second) raiseGlError(GL_INVALID_VALUE);
}
GLenum glCheckFramebufferStatus(GLenum) {
    ++state.framebufferChecks;
    if (state.incompleteNextTextureFramebuffer) { state.incompleteNextTextureFramebuffer = false; return 0x8CD6; }
    return GL_FRAMEBUFFER_COMPLETE;
}
void glClear(GLbitfield) {}
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
void glCompileShader(GLuint) {}
GLuint glCreateProgram(void) { ++state.livePrograms; const GLuint p = nextName++; state.programLinks[p] = true; return p; }
GLuint glCreateShader(GLenum type) {
    ++state.liveShaders; const GLuint s = nextName++; state.shaderTypes[s] = type;
    return s;
}
void glCullFace(GLenum) {}
void glDeleteBuffers(GLsizei n, const GLuint* p) { drop(n, p, state.liveBuffers); }
void glDeleteFramebuffers(GLsizei n, const GLuint* p) { drop(n, p, state.liveFramebuffers); }
void glDeleteProgram(GLuint) { --state.livePrograms; }
void glDeleteRenderbuffers(GLsizei n, const GLuint* p) { drop(n, p, state.liveRenderbuffers); }
void glDeleteShader(GLuint p) { --state.liveShaders; state.shaderTypes.erase(p); }
void glDeleteTextures(GLsizei n, const GLuint* p) { drop(n, p, state.liveTextures); }
void glDeleteVertexArrays(GLsizei n, const GLuint* p) {
    drop(n, p, state.liveVaos); for (GLsizei i = 0; i < n; ++i) state.vaos.erase(p[i]);
}
void glDepthFunc(GLenum) {}
void glDepthMask(GLboolean) {}
void glDisable(GLenum) {}
void glDrawArrays(GLenum, GLint, GLsizei) { ++state.draws; }
void glDrawBuffers(GLsizei n, const GLenum*) { if (n == 0) raiseGlError(GL_INVALID_VALUE); }
void glDrawElements(GLenum, GLsizei, GLenum, const void*) {
    ++state.draws;
    const GLuint element = state.vaos[state.currentVao].elementBuffer;
    state.elementDraws.emplace_back(state.currentVao, element);
    if (!state.currentVao || !element) raiseGlError(GL_INVALID_OPERATION);
}
void glEnable(GLenum) {}
void glEnableVertexAttribArray(GLuint) {}
void glFinish(void) { ++state.glFinishCalls; }
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
void glFrontFace(GLenum) {}
void glGenBuffers(GLsizei n, GLuint* p) { names(n, p, state.liveBuffers); }
void glGenFramebuffers(GLsizei n, GLuint* p) { names(n, p, state.liveFramebuffers); }
void glGenRenderbuffers(GLsizei n, GLuint* p) { names(n, p, state.liveRenderbuffers); }
void glGenTextures(GLsizei n, GLuint* p) { names(n, p, state.liveTextures); }
void glGenVertexArrays(GLsizei n, GLuint* p) {
    names(n, p, state.liveVaos);
    if (state.fault == af_test::Fault::VertexArray) { raiseGlError(GL_INVALID_OPERATION); }
}
void glGenQueriesEXT(GLsizei n, GLuint* p) {
    names(n, p, state.liveQueries);
    state.timerQueriesGenerated += n;
}
void glDeleteQueriesEXT(GLsizei n, const GLuint* p) {
    for (GLsizei i = 0; i < n; ++i) {
        if (!p[i]) continue;
        --state.liveQueries;
        ++state.timerQueriesDeleted;
    }
}
void glBeginQueryEXT(GLenum target, GLuint query) {
    if (target != 0x88BF || state.activeTimerQuery != 0 || !query) {
        raiseGlError(GL_INVALID_OPERATION);
        return;
    }
    state.activeTimerQuery = query;
    ++state.timerQueryBeginCalls;
}
void glEndQueryEXT(GLenum target) {
    if (target != 0x88BF || state.activeTimerQuery == 0) {
        raiseGlError(GL_INVALID_OPERATION);
        return;
    }
    state.activeTimerQuery = 0;
    ++state.timerQueryEndCalls;
}
void glGetQueryObjectuivEXT(GLuint query, GLenum pname, GLuint* value) {
    (void)query;
    if (pname != 0x8867) { raiseGlError(GL_INVALID_ENUM); return; }
    ++state.timerQueryResultPolls;
    *value = state.timerResultAvailable ? GL_TRUE : GL_FALSE;
}
void glGetQueryObjectui64vEXT(GLuint query, GLenum pname, unsigned long long* value) {
    (void)query;
    if (pname != 0x8866) { raiseGlError(GL_INVALID_ENUM); return; }
    ++state.timerQueryValueReads;
    *value = state.timerResultNs;
}
void glGenerateMipmap(GLenum) {}
void glGetFloatv(GLenum, GLfloat* value) { *value = 4.0f; }
void glGetIntegerv(GLenum key, GLint* value) {
    switch (key) {
        case GL_CURRENT_PROGRAM: *value = GLint(state.currentProgram); break;
        case GL_READ_FRAMEBUFFER_BINDING: *value = GLint(state.currentReadFramebuffer); break;
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *value = 256; break;
        case GL_MAX_SAMPLES: *value = 4; break;
        case GL_NUM_EXTENSIONS: *value = GLint(extensionNames().size()); break;
        case 0x8FBB: *value = state.gpuDisjoint ? GL_TRUE : GL_FALSE; break;
        default: *value = 1; break;
    }
}
void glGetProgramInfoLog(GLuint, GLsizei len, GLsizei*, GLchar* out) { std::snprintf(out, size_t(len), "injected link failure"); }
void glGetProgramiv(GLuint p, GLenum key, GLint* value) { *value = key == GL_LINK_STATUS ? (state.programLinks[p] ? 1 : 0) : 64; }
void glGetShaderInfoLog(GLuint, GLsizei len, GLsizei*, GLchar* out) { std::snprintf(out, size_t(len), "injected compile failure"); }
void glGetShaderiv(GLuint p, GLenum key, GLint* value) {
    *value = key == GL_COMPILE_STATUS ? (state.fault == af_test::Fault::FragmentCompile && state.shaderTypes[p] == GL_FRAGMENT_SHADER ? 0 : 1) : 64;
}
const GLubyte* glGetString(GLenum key) {
    if (key == GL_EXTENSIONS) {
        ++state.legacyExtensionQueries;
        raiseGlError(GL_INVALID_ENUM);  /* ES 3.x requires glGetStringi. */
        return nullptr;
    }
    const char* result = "";
    if (key == GL_VERSION) result = state.version.c_str();
    if (key == GL_RENDERER) result = state.rendererName.c_str();
    if (key == GL_VENDOR) result = state.vendorName.c_str();
    if (key == GL_SHADING_LANGUAGE_VERSION) result = state.shadingLanguageVersion.c_str();
    return reinterpret_cast<const GLubyte*>(result);
}
const GLubyte* glGetStringi(GLenum key, GLuint index) {
    ++state.indexedExtensionQueries;
    const auto names = extensionNames();
    if (key != GL_EXTENSIONS || index >= names.size()) {
        raiseGlError(key == GL_EXTENSIONS ? GL_INVALID_VALUE : GL_INVALID_ENUM);
        return nullptr;
    }
    static std::string name;
    name = names[index];
    return reinterpret_cast<const GLubyte*>(name.c_str());
}
GLuint glGetUniformBlockIndex(GLuint, const GLchar* name) {
    if (!std::strcmp(name, "CameraBlock")) return 0;
    if (!std::strcmp(name, "MaterialBlock")) return 1;
    if (!std::strcmp(name, "PostBlock")) return 2;
    return GL_INVALID_INDEX;
}
GLint glGetUniformLocation(GLuint p, const GLchar*) { const GLint location = nextUniform++; state.uniformOwners[location] = p; return location; }
void glLinkProgram(GLuint p) {
    if (state.fault == af_test::Fault::ProgramLink && state.livePrograms == 1) state.programLinks[p] = false;
}
void glPolygonOffset(GLfloat, GLfloat) {}
void glReadBuffer(GLenum value) { state.currentReadBuffer = value; }
void glReadPixels(GLint, GLint, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    ++state.readPixelsCalls;
    state.readPixelsWidth = width;
    state.readPixelsHeight = height;
    state.lastReadFramebuffer = state.currentReadFramebuffer;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0 || !pixels) {
        raiseGlError(GL_INVALID_OPERATION);
        return;
    }
    auto* rgba = static_cast<GLubyte*>(pixels);
    const size_t count = size_t(width) * size_t(height);
    for (size_t i = 0; i < count; ++i) {
        rgba[i * 4 + 0] = 14;
        rgba[i * 4 + 1] = 19;
        rgba[i * 4 + 2] = 28;
        rgba[i * 4 + 3] = 255;
    }
    if (state.readPixelsHasAvatarPixel) {
        rgba[0] = 220;
        rgba[1] = 80;
        rgba[2] = 40;
        rgba[3] = 255;
    }
    if (state.readPixelsFail) raiseGlError(GL_INVALID_OPERATION);
}
void glRenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum, GLsizei, GLsizei) {
    ++state.msaaAttempts;
    if (samples > 1 && state.rejectNextMsaa) { state.rejectNextMsaa = false; raiseGlError(GL_INVALID_VALUE); }
    if (samples == 1 && state.rejectSingleSample) raiseGlError(GL_INVALID_VALUE);
}
void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
void glTexImage2D(GLenum, GLint, GLint internal, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {
    if (internal == GL_RGBA16F && state.rejectFloatHdr) {
        state.incompleteNextTextureFramebuffer = true;
    }
}
void glTexParameterf(GLenum, GLenum name, GLfloat value) {
    if (name == GL_TEXTURE_MAX_ANISOTROPY_EXT) {
        ++state.anisotropyWrites;
        state.lastAnisotropy = value;
    }
}
void glTexParameteri(GLenum, GLenum, GLint) {}
void glUniform1i(GLint location, GLint) {
    const GLuint owner = state.uniformOwners[location];
    if (!state.currentProgram || state.currentProgram != owner) raiseGlError(GL_INVALID_OPERATION);
    else state.samplerWrites.emplace_back(state.currentProgram, location);
}
void glUniformBlockBinding(GLuint, GLuint, GLuint) {}
void glUseProgram(GLuint p) { state.currentProgram = p; }
void glVertexAttribPointer(GLuint index, GLint, GLenum, GLboolean, GLsizei, const void*) {
    if (!state.currentVao || !state.currentArrayBuffer) raiseGlError(GL_INVALID_OPERATION);
    else state.vaos[state.currentVao].attributes[index].buffer = state.currentArrayBuffer;
}
void glViewport(GLint, GLint, GLsizei, GLsizei) {}
int __android_log_print(int, const char* tag, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (tag && !std::strcmp(tag, "AvatarForgeRenderer") &&
        std::strstr(message, "AF_GLES_CONTEXT")) state.lastContextLog = message;
    return written;
}

/* Small engine fixture: the renderer under test is real, but no geometry generation is needed. */
int32_t af_engine_vertex_count(const af_engine*) { return 3; }
int32_t af_engine_index_count(const af_engine*) { return 3; }
const af_vertex* af_engine_vertices(const af_engine*) { static const af_vertex v[3]{}; return v; }
const uint32_t* af_engine_indices(const af_engine*) { static const uint32_t i[3] = {0, 1, 2}; return i; }
int32_t af_engine_submesh_count(const af_engine*) { return 1; }
const af_submesh* af_engine_submeshes(const af_engine*) { static af_submesh s{}; s.vertex_count = 3; s.index_count = 3; return &s; }
int32_t af_engine_material_count(const af_engine*) { return 1; }
const af_material* af_engine_materials(const af_engine*) { static const af_material m{}; return &m; }
int32_t af_engine_texture(const af_engine*, int32_t slot, af_texture* out) {
    if (!out || slot < 0 || slot >= AF_TEX_COUNT) return AF_ERR_INVALID_ARG;
    static const uint8_t pixels[16] = {255, 255, 255, 255, 255, 255, 255, 255,
                                       255, 255, 255, 255, 255, 255, 255, 255};
    out->width = 2; out->height = 2; out->channels = 4; out->mip_levels = 1;
    out->byte_size = sizeof(pixels); out->data = pixels;
    return AF_OK;
}
int32_t af_engine_joint_count(const af_engine*) { return 0; }
int32_t af_engine_joint_info(const af_engine*, int32_t, af_joint_info*) { return AF_ERR_INVALID_ARG; }
int32_t af_engine_joint_matrices(const af_engine*, float*, int32_t) { return AF_ERR_INVALID_ARG; }
float af_engine_clip_duration(const af_engine*, int32_t) { return 1.0f; }
int32_t af_engine_apply_clip(af_engine*, int32_t, float) { return AF_OK; }
}
