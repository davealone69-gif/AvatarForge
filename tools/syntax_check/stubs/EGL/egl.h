/*
 * Minimal EGL 1.5 stub for desktop syntax-checking of af_gl.cpp (real one: NDK).
 * Values match the EGL spec.
 */
#ifndef AF_STUB_EGL_EGL_H
#define AF_STUB_EGL_EGL_H

#include <KHR/khrplatform.h>

typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLConfig;
typedef void* EGLNativeDisplayType;
typedef int EGLint;
typedef khronos_uintptr_t EGLBoolean;
typedef unsigned int EGLBitfield;

#define EGL_NO_ERROR 0

#define EGL_NO_DISPLAY            ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY       ((EGLDisplay)0)
#define EGL_NO_CONTEXT            ((EGLContext)0)
#define EGL_NO_SURFACE            ((EGLSurface)0)
#define EGL_NONE                  0x3038
#define EGL_TRUE                  1
#define EGL_FALSE                 0

#define EGL_SUCCESS               0x3000
#define EGL_TRY_AGAIN             0x30A5
#define EGL_BAD_SURFACE           0x3009
#define EGL_CONTEXT_LOST          0x300E

#define EGL_RENDERABLE_TYPE       0x3040
#define EGL_SURFACE_TYPE          0x3033
#define EGL_RED_SIZE              0x3024
#define EGL_GREEN_SIZE            0x3025
#define EGL_BLUE_SIZE             0x3026
#define EGL_DEPTH_SIZE            0x3025
#define EGL_WINDOW_BIT            1u
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define EGL_WIDTH                 0x3057
#define EGL_HEIGHT                0x3056
#define EGL_VENDOR                0x3053

#ifdef __cplusplus
extern "C" {
#endif

EGLBoolean eglInitialize(EGLDisplay display, EGLint* major, EGLint* minor);
EGLBoolean eglTerminate(EGLDisplay display);
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint* attrib_list, EGLConfig* configs,
                           EGLint config_size, EGLint* num_config);
EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, void* native_window,
                                  const EGLint* attrib_list);
EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface);
EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context,
                            const EGLint* attrib_list);
EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context);
EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface surface, EGLSurface surface2,
                          EGLContext context);
EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value);
EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface);
EGLint eglGetError(void);
const char* eglQueryString(EGLDisplay display, EGLint name);

#ifdef __cplusplus
}
#endif

#endif /* AF_STUB_EGL_EGL_H */
