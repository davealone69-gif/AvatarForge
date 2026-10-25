/*
 * AvatarForge :: :sdk
 *
 * The SDK module: the Kotlin façade, the JNI bridge declaration, the renderer wrapper and the
 * render surface view. It also builds the three native libraries through CMake, so a consumer that
 * takes this module as an AAR gets the kernel with it.
 *
 * Anything an app needs is public here: there is no "internal" API that the sample app uses and a
 * third party cannot. The `com.avatarforge.sdk.internal` package exists only to keep the raw JNI
 * declarations out of autocomplete.
 */
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
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

android {
    namespace = "com.avatarforge.sdk"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        minSdk = 26
        // A library does not set targetSdk: the consuming app's value is the one that applies.

        /* arm64-v8a is the primary target: every device that can hold this pipeline at 30 FPS is
           64-bit ARM. x86_64 is included so the app can be run (and profiled) in an emulator. */
        /* -Paf.abIs=arm64-v8a builds a single architecture: half the native compile time and half
           the peak memory, which is what a low-RAM build machine needs. The default keeps both. */
        val abis = (project.findProperty("af.abIs") as String?)?.split(",")?.map { it.trim() }
            ?: listOf("arm64-v8a", "x86_64")
        ndk {
            abiFilters += abis
        }
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                /*
                 * Passing the native build knobs through defaultConfig: this is the DSL node that
                 * owns `arguments` (the module-level externalNativeBuild block only carries the path
                 * and the CMake version).
                 *
                 * -Paf.lowRam=true serialises the native compile and drops it to -O1. On a 2 GB
                 * container an -O3 compile of af_vk.cpp plus the Gradle daemon exceeds RAM and the
                 * build daemon is killed; with the flag the same APK builds. Shipping builds do not
                 * use it (see BUILD.md).
                 */
                if (project.findProperty("af.lowRam") == "true") {
                    arguments.add("-DAF_OPT_FLAGS=-O1")
                    arguments.add("-DCMAKE_BUILD_PARALLEL_LEVEL=1")
                } else {
                    /* -Paf.parallel=N raises the native compile parallelism on machines that can
                       afford it (CI uses 4); the default of 2 keeps local low-RAM builds stable. */
                    val parallel = (project.findProperty("af.parallel") as String?)?.toIntOrNull() ?: 2
                    arguments.add("-DCMAKE_BUILD_PARALLEL_LEVEL=$parallel")
                }
            }
        }
    }

    /* Data-driven customization contracts are packaged into the AAR as offline assets. The raw
       user submissions remain under requirements/; only normalized, status-gated contracts ship. */
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
        release {
            isMinifyEnabled = false     // the SDK is a library; the app decides about shrinking
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            /* The native libraries are loaded with System.loadLibrary from the APK's lib/ dir, so
               they must stay uncompressed is FALSE and extractNativeLibs must be TRUE (the app
               manifest sets that): loadLibrary needs a real file on older devices. */
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    testImplementation("junit:junit:4.13.2")
}
