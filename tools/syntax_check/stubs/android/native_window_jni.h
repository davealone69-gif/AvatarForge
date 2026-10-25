/* ANativeWindow-from-Java stub for desktop syntax-checking (real one: NDK). */
#ifndef AF_STUB_ANDROID_NATIVE_WINDOW_JNI_H
#define AF_STUB_ANDROID_NATIVE_WINDOW_JNI_H

#include "native_window.h"

struct _jobject;

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow* ANativeWindow_fromSurface(void* env, void* surface);
void ANativeWindow_release(struct ANativeWindow* window);

#ifdef __cplusplus
}
#endif

#endif /* AF_STUB_ANDROID_NATIVE_WINDOW_JNI_H */
