/* Minimal EGL declarations for host fault injection; see GLES3/gl31.h. */
#ifndef AF_TEST_EGL_H
#define AF_TEST_EGL_H
#include <stdint.h>
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef void* EGLDisplay; typedef void* EGLSurface; typedef void* EGLContext;
typedef void* EGLConfig; typedef void* EGLNativeDisplayType;
typedef void* EGLNativeWindowType; typedef void* EGLNativePixmapType;
typedef intptr_t EGLAttrib;
typedef void (*EGLProc)(void);
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ACCESS 0x3002
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_WINDOW 0x300A
#define EGL_BAD_PARAMETER 0x300B
#define EGL_BAD_SURFACE 0x300D
#define EGL_CONTEXT_LOST 0x300E
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#ifdef __cplusplus
extern "C" {
#endif
EGLBoolean eglChooseConfig(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*);
EGLContext eglCreateContext(EGLDisplay,EGLConfig,EGLContext,const EGLint*);
EGLSurface eglCreateWindowSurface(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*);
EGLBoolean eglDestroyContext(EGLDisplay,EGLContext);
EGLBoolean eglDestroySurface(EGLDisplay,EGLSurface);
EGLDisplay eglGetDisplay(EGLNativeDisplayType);
EGLProc eglGetProcAddress(const char*);
EGLint eglGetError(void);
EGLBoolean eglInitialize(EGLDisplay,EGLint*,EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
EGLBoolean eglQuerySurface(EGLDisplay,EGLSurface,EGLint,EGLint*);
EGLBoolean eglSwapBuffers(EGLDisplay,EGLSurface);
EGLBoolean eglTerminate(EGLDisplay);
#ifdef __cplusplus
}
#endif
#endif
