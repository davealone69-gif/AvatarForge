/*
 * AvatarForge :: :sdk
 *
 * The SDK module: Kotlin façade, JNI bridge declaration, renderer wrapper and render surface view.
 * It also builds the native libraries through CMake, so a consumer that takes this module as an AAR
 * gets the kernel with it.
 *
 * Anything an app needs is public here: there is no "internal" API that the sample app uses and a
 * third party cannot. The com.avatarforge.sdk.internal package keeps raw JNI declarations out of
 * autocomplete.
 */
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    id("maven-publish")
}

/*
 * Two ways to get the native libraries into the APK:
 *
 *   default                Gradle drives CMake (externalNativeBuild). One command, incremental,
 *                          and what a normal machine wants.
 *   -Paf.prebuiltJni=DIR   skip CMake and package the .so files already sitting in DIR/<abi>/.
 *
 * The second exists because the native build is the memory peak of the whole APK build: on a 2 GB
 * machine the Gradle daemon plus an -O3 clang of af_vk.cpp exceeds RAM and the daemon is killed.
 * Building the libraries with ninja first (which schedules them one at a time and finishes in
 * seconds) and then packaging them is the same artifact with a fraction of the peak. It is also how
 * CI caches the native layer between app-only changes.
 */
val prebuiltJni: String? = project.findProperty("af.prebuiltJni") as String?
val sdkPublicationVersion = providers.gradleProperty("af.sdkVersion").orElse("0.10.2-SNAPSHOT")

android {
    namespace = "com.avatarforge.sdk"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        minSdk = 26
        // A library does not set targetSdk: the consuming app's value is the one that applies.
        /* arm64-v8a is primary; x86_64 supports emulator builds. */
        val abis = (project.findProperty("af.abIs") as String?)?.split(",")?.map { it.trim() }
            ?: listOf("arm64-v8a", "x86_64")
        ndk { abiFilters += abis }
        consumerProguardFiles("consumer-rules.pro")
        externalNativeBuild {
            cmake {
                if (project.findProperty("af.lowRam") == "true") {
                    arguments.add("-DAF_OPT_FLAGS=-O1")
                    arguments.add("-DCMAKE_BUILD_PARALLEL_LEVEL=1")
                } else {
                    arguments.add("-DCMAKE_BUILD_PARALLEL_LEVEL=2")
                }
            }
        }
    }

    sourceSets.getByName("main").assets.srcDir("../config")

    if (prebuiltJni == null) {
        externalNativeBuild {
            cmake {
                path = file("../native/CMakeLists.txt")
                version = "3.22.1"
            }
        }
    } else {
        sourceSets.getByName("main").jniLibs.srcDir(prebuiltJni)
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    packaging {
        jniLibs { useLegacyPackaging = true }
    }

    publishing {
        singleVariant("release") { withSourcesJar() }
    }
}

dependencies {
    // Public API exposes StateFlow and suspend functions, so coroutines must be a compile dependency.
    api("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    testImplementation("junit:junit:4.13.2")
}

/*
 * Publish to a deterministic local Maven repository under sdk/build/repo. The generated POM and
 * Gradle module metadata carry dependency coordinates, unlike copying a raw AAR into libs/.
 * A remote Maven repository is deliberately not configured: publishing credentials must never be
 * embedded in this repository.
 */
afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                groupId = "com.avatarforge"
                artifactId = "avatarforge-sdk"
                version = sdkPublicationVersion.get()
                from(components["release"])
                pom {
                    name.set("AvatarForge Android SDK")
                    description.set("On-device avatar generation and native Vulkan/OpenGL ES renderer for Android.")
                    url.set("https://github.com/davealone69-gif/AvatarForge")
                    licenses {
                        license {
                            name.set("License not yet selected")
                            url.set("https://github.com/davealone69-gif/AvatarForge")
                        }
                    }
                    scm {
                        url.set("https://github.com/davealone69-gif/AvatarForge")
                        connection.set("scm:git:git://github.com/davealone69-gif/AvatarForge.git")
                        developerConnection.set("scm:git:ssh://git@github.com/davealone69-gif/AvatarForge.git")
                    }
                }
            }
        }
        repositories {
            maven {
                name = "localBuild"
                url = layout.buildDirectory.dir("repo").get().asFile.toURI()
            }
        }
    }
}
