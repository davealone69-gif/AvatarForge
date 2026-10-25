/*
 * AvatarForge :: tools/syntax_check/stubs/GLES3/gl31.h
 *
 * Minimal OpenGL ES 3.1 declarations for desktop syntax-checking of the renderer
 * (g++ -fsyntax-only). The real NDK provides the full header on Android; this stub
 * declares exactly the surface af_gl.cpp uses, with the real GL constant values, so
 * a type error, a typo, or a bad constant use is caught without an NDK.
 */
#ifndef AF_STUB_GLES3_GL31_H
#define AF_STUB_GLES3_GL31_H

#include <KHR/khrplatform.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef khronos_int8_t GLbyte;
typedef khronos_uint8_t GLubyte;
typedef khronos_int16_t GLshort;
typedef khronos_uint16_t GLushort;
typedef khronos_int32_t GLint;
typedef int GLfixed;
typedef khronos_intptr_t GLintptr;
typedef khronos_intptr_t GLint64;
typedef khronos_uintptr_t GLuint64;
typedef khronos_float_t GLfloat;
typedef khronos_double_t GLdouble;
typedef khronos_size_t GLsizeiptr;
typedef char GLchar;

#ifdef __cplusplus
extern "C" {
#endif

/* types */
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_INFO_LOG_LENGTH          0x8B84
#define GL_LINK_STATUS              0x8B82
#define GL_ARRAY_BUFFER             0x8892
#define GL_ELEMENT_ARRAY_BUFFER     0x8893
#define GL_UNIFORM_BUFFER           0x8A11
#define GL_STATIC_DRAW              0x88E4
#define GL_DYNAMIC_DRAW             0x88E8
#define GL_FALSE                    0
#define GL_TRUE                     1
#define GL_NO_ERROR                 0x0000
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_LINES                    0x0001
#define GL_FLOAT                    0x1406
#define GL_UNSIGNED_BYTE            0x1401
#define GL_UNSIGNED_INT             0x1405
#define GL_TEXTURE_2D               0x0DE1
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE5                 (GL_TEXTURE0 + 5)
#define GL_TEXTURE6                 (GL_TEXTURE0 + 6)
#define GL_TEXTURE7                 (GL_TEXTURE0 + 7)
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_TEXTURE_MAX_LEVEL        0x813D
#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_LINEAR_MIPMAP_LINEAR     0x2703
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_R8                       0x8229
#define GL_RGBA8                    0x8058
#define GL_SRGB8_ALPHA8             0x8C43
#define GL_RGBA16F                  0x881A
#define GL_RED                      0x1903
#define GL_RGBA                     0x1908
#define GL_HALF_FLOAT               0x140B
#define GL_DEPTH_COMPONENT          0x1902
#define GL_DEPTH_COMPONENT24        0x81A6
#define GL_FRAMEBUFFER              0x8D40
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_DEPTH_ATTACHMENT         0x8D00
#define GL_RENDERBUFFER             0x8D41
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_NONE                     0
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_DEPTH_TEST               0x0B71
#define GL_CULL_FACE                0x0B44
#define GL_BLEND                    0x0BE2
#define GL_POLYGON_OFFSET_FILL      0x8037
#define GL_BACK                     0x0405
#define GL_CCW                      0x0901
#define GL_LEQUAL                   0x0203
#define GL_VENDOR                   0x1F00
#define GL_VERSION                  0x1F01
#define GL_EXTENSIONS               0x1F03
#define GL_RENDERER                 0x1F01
#define GL_MAX_SAMPLES              0x8D57
#define GL_TEXTURE_COMPARE_MODE     0x884C
#define GL_TEXTURE_COMPARE_FUNC     0x884D
#define GL_COMPARE_REF_TO_TEXTURE   0x884E
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A64
#define GL_INVALID_INDEX            0xFFFFFFFFu
#define GL_SRC_ALPHA                0x0302
#define GL_ONE                      1

void glActiveTexture(GLenum texture);
void glAttachShader(GLuint program, GLuint shader);
void glBindBuffer(GLenum target, GLuint buffer);
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glBindTexture(GLenum target, GLuint texture);
void glBindVertexArray(GLuint array);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
GLenum glCheckFramebufferStatus(GLenum target);
void glClear(GLbitfield mask);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glCompileShader(GLuint shader);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum type);
void glCullFace(GLenum mode);
void glDeleteBuffers(GLsizei n, const GLuint* buffers);
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
void glDeleteProgram(GLuint program);
void glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers);
void glDeleteShader(GLuint shader);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDisable(GLenum cap);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawBuffers(GLsizei n, const GLenum* bufs);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void glEnable(GLenum cap);
void glEnableVertexAttribArray(GLuint index);
void glFinish(void);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glFrontFace(GLenum mode);
void glGenBuffers(GLsizei n, GLuint* buffers);
void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers);
void glGenTextures(GLsizei n, GLuint* textures);
void glGenVertexArrays(GLsizei n, GLuint* arrays);
void glGenerateMipmap(GLenum target);
GLenum glGetError(void);
void glGetFloatv(GLenum pname, GLfloat* data);
void glGetIntegerv(GLenum pname, GLint* data);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
const GLubyte* glGetString(GLenum name);
GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName);
GLint glGetUniformLocation(GLuint program, const GLchar* name);
void glLinkProgram(GLuint program);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glReadBuffer(GLenum mode);
void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void* pixels);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glUniform1i(GLint location, GLint v0);
void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
void glUseProgram(GLuint program);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#ifdef __cplusplus
}
#endif

#endif /* AF_STUB_GLES3_GL31_H */
