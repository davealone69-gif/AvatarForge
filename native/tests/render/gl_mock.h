/* Stateful GLES/EGL test double. It checks API ownership rules, not rendering quality. */
#ifndef AF_TEST_GL_MOCK_H
#define AF_TEST_GL_MOCK_H
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <map>
#include <string>
#include <vector>

namespace af_test {
enum class Fault { None, EglContext, FragmentCompile, ProgramLink, VertexArray, MsaaStorage, HdrFramebuffer };
struct VertexAttribute { GLuint buffer = 0; };
struct VertexArray { GLuint elementBuffer = 0; std::map<GLuint, VertexAttribute> attributes; };
struct MockState {
    Fault fault = Fault::None;
    bool rejectFloatHdr = false;
    std::string version = "OpenGL ES 3.1 Mock";
    std::string extensions = "GL_EXT_color_buffer_float_suffix GL_EXT_texture_filter_anisotropic";
    GLuint currentProgram = 0, currentArrayBuffer = 0, currentVao = 0, currentFramebuffer = 0;
    GLuint currentRenderbuffer = 0, currentTexture = 0;
    EGLContext currentContext = EGL_NO_CONTEXT;
    EGLint eglError = EGL_SUCCESS;
    std::vector<GLenum> glErrors;
    std::map<GLuint, VertexArray> vaos;
    std::map<GLuint, GLenum> shaderTypes;
    std::map<GLuint, bool> programLinks;
    std::map<GLint, GLuint> uniformOwners;
    std::vector<std::pair<GLuint, GLint>> samplerWrites;
    std::vector<std::pair<GLuint, GLuint>> elementDraws;
    int contextsCreated = 0, contextsDestroyed = 0, surfacesCreated = 0, surfacesDestroyed = 0;
    int livePrograms = 0, liveShaders = 0, liveBuffers = 0, liveTextures = 0;
    int liveFramebuffers = 0, liveRenderbuffers = 0, liveVaos = 0;
    int msaaAttempts = 0, framebufferChecks = 0, draws = 0;
    bool failNextVertexArray = false;
    bool rejectNextMsaa = false;
    bool rejectSingleSample = false;
    bool incompleteNextTextureFramebuffer = false;
};
extern MockState state;
void reset();
void raiseGlError(GLenum error);
}  // namespace af_test
#endif
