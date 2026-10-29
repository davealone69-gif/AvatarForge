/* Minimal GLES API surface for host fault-injection tests, NOT an OpenGL implementation.
 * Only the declarations/constants used by af_gl.cpp are needed; the mock validates call state.
 */
#ifndef AF_TEST_GL31_H
#define AF_TEST_GL31_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint;
typedef int GLsizei; typedef unsigned char GLboolean; typedef unsigned int GLbitfield;
typedef float GLfloat; typedef float GLclampf; typedef char GLchar;
typedef int64_t GLint64; typedef ptrdiff_t GLsizeiptr; typedef ptrdiff_t GLintptr;
typedef signed char GLbyte; typedef unsigned char GLubyte;
#define GL_NO_ERROR 0
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_ONE 1
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LINES 0x0001
#define GL_SRC_ALPHA 0x0302
#define GL_LEQUAL 0x0203
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_BACK 0x0405
#define GL_CCW 0x0901
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE5 0x84C5
#define GL_TEXTURE6 0x84C6
#define GL_TEXTURE7 0x84C7
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_RED 0x1903
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_R8 0x8229
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_MAX_SAMPLES 0x8D57
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00
#ifdef __cplusplus
extern "C" {
#endif
void glActiveTexture(GLenum); void glAttachShader(GLuint,GLuint);
void glBindBuffer(GLenum,GLuint); void glBindBufferBase(GLenum,GLuint,GLuint);
void glBindBufferRange(GLenum,GLuint,GLuint,GLintptr,GLsizeiptr);
void glBindFramebuffer(GLenum,GLuint); void glBindRenderbuffer(GLenum,GLuint);
void glBindTexture(GLenum,GLuint); void glBindVertexArray(GLuint);
void glBlendFunc(GLenum,GLenum);
void glBlitFramebuffer(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
void glBufferData(GLenum,GLsizeiptr,const void*,GLenum);
void glBufferSubData(GLenum,GLintptr,GLsizeiptr,const void*);
GLenum glCheckFramebufferStatus(GLenum); void glClear(GLbitfield); void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat);
void glCompileShader(GLuint); GLuint glCreateProgram(void); GLuint glCreateShader(GLenum);
void glCullFace(GLenum); void glDeleteBuffers(GLsizei,const GLuint*);
void glDeleteFramebuffers(GLsizei,const GLuint*); void glDeleteProgram(GLuint);
void glDeleteRenderbuffers(GLsizei,const GLuint*); void glDeleteShader(GLuint);
void glDeleteTextures(GLsizei,const GLuint*); void glDeleteVertexArrays(GLsizei,const GLuint*);
void glDepthFunc(GLenum); void glDepthMask(GLboolean); void glDisable(GLenum);
void glDrawArrays(GLenum,GLint,GLsizei); void glDrawBuffers(GLsizei,const GLenum*);
void glDrawElements(GLenum,GLsizei,GLenum,const void*);
void glEnable(GLenum); void glEnableVertexAttribArray(GLuint); void glFinish(void);
void glFramebufferRenderbuffer(GLenum,GLenum,GLenum,GLuint);
void glFramebufferTexture2D(GLenum,GLenum,GLenum,GLuint,GLint); void glFrontFace(GLenum);
void glGenBuffers(GLsizei,GLuint*); void glGenFramebuffers(GLsizei,GLuint*);
void glGenRenderbuffers(GLsizei,GLuint*); void glGenTextures(GLsizei,GLuint*);
void glGenVertexArrays(GLsizei,GLuint*); void glGenerateMipmap(GLenum);
GLenum glGetError(void); void glGetFloatv(GLenum,GLfloat*); void glGetIntegerv(GLenum,GLint*);
void glGetProgramInfoLog(GLuint,GLsizei,GLsizei*,GLchar*); void glGetProgramiv(GLuint,GLenum,GLint*);
void glGetShaderInfoLog(GLuint,GLsizei,GLsizei*,GLchar*); void glGetShaderiv(GLuint,GLenum,GLint*);
const GLubyte* glGetString(GLenum);
GLuint glGetUniformBlockIndex(GLuint,const GLchar*); GLint glGetUniformLocation(GLuint,const GLchar*);
void glLinkProgram(GLuint); void glPolygonOffset(GLfloat,GLfloat); void glReadBuffer(GLenum);
void glRenderbufferStorageMultisample(GLenum,GLsizei,GLenum,GLsizei,GLsizei);
void glShaderSource(GLuint,GLsizei,const GLchar* const*,const GLint*);
void glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
void glTexParameterf(GLenum,GLenum,GLfloat); void glTexParameteri(GLenum,GLenum,GLint);
void glUniform1i(GLint,GLint); void glUniformBlockBinding(GLuint,GLuint,GLuint);
void glUseProgram(GLuint);
void glVertexAttribPointer(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
void glViewport(GLint,GLint,GLsizei,GLsizei);
#ifdef __cplusplus
}
#endif
#endif
