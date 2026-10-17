/*
 * AvatarForge :: settings.gradle.kts
 *
 * Two modules, and the split is the architecture:
 *
 *   :sdk   the SDK: JNI bridge, Kotlin façade, renderer wrapper. Depends on the native libraries
 *          built by CMake from native/ (libafcore, libafrender, libavatarforge).
 *   :app   the Android app: Jetpack Compose UI only. Depends on :sdk and on nothing else.
 *
 * A third-party app can depend on :sdk (or on its AAR) and get exactly what our own UI gets - there
 * is no private API path.
 */
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "AvatarForge"
include(":sdk", ":app")
