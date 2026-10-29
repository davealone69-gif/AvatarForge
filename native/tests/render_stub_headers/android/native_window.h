/* The host tests use an opaque non-null window token. No Android window is created. */
#ifndef AF_TEST_NATIVE_WINDOW_H
#define AF_TEST_NATIVE_WINDOW_H
struct ANativeWindow;
#ifdef __cplusplus
extern "C" {
#endif
void ANativeWindow_acquire(ANativeWindow*);
void ANativeWindow_release(ANativeWindow*);
#ifdef __cplusplus
}
#endif
#endif
