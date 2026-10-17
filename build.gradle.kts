/*
 * AvatarForge :: root build file.
 *
 * Versions are pinned in one place so the two modules can never disagree about the Kotlin or the
 * Android plugin, and so an APK built today behaves like an APK built next month.
 */
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("com.android.library") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
