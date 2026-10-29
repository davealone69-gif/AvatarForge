/* Host test stub for Android logging. */
#ifndef AF_TEST_ANDROID_LOG_H
#define AF_TEST_ANDROID_LOG_H
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#ifdef __cplusplus
extern "C" {
#endif
int __android_log_print(int prio, const char* tag, const char* fmt, ...);
#ifdef __cplusplus
}
#endif
#endif
