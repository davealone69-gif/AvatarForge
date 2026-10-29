# ==============================================================================================
#  AvatarForge :: sdk/consumer-rules.pro
# ==============================================================================================
#
#  Rules any app that shrinks its code needs when it uses this SDK.
#
#  Why they exist: the kernel finds the bridge by class name. libavatarforge.so exports
#  Java_com_avatarforge_sdk_internal_NativeBridge_* symbols, so R8 must not rename that class or its
#  methods, and the callback object the JNI layer looks up by GetMethodID must keep its method names
#  and signatures. Without these rules the app would build fine and fail on the first generation
#  with UnsatisfiedLinkError - which is exactly the kind of failure that should be caught by the
#  build, not by a user.
# ==============================================================================================

-keep class com.avatarforge.sdk.internal.NativeBridge { *; }
-keepclasseswithmembers class * {
    native <methods>;
}
# The kernel resolves these three members with GetMethodID on the object it was handed. The class
# itself may be renamed (the JNI code asks the JVM for the runtime class), the member names and
# signatures may not. `AvatarEngine$KernelCallbacks` is a private *inner class*, so an
# `implements`-based rule can never match it; this rule names it directly.
-keepclassmembernames class com.avatarforge.sdk.AvatarEngine$KernelCallbacks {
    public void onProgress(int, float, java.lang.String);
    public void onLog(int, java.lang.String);
    public boolean shouldCancel();
}
# The FileProvider authority is built from the application id at runtime, so no rule is needed for it,
# but the engine's export path must not be renamed because the kernel writes files to it.
-keep class com.avatarforge.sdk.AvatarForge { *; }
