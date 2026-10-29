import java.util.Properties

/*
 * AvatarForge :: :app
 *
 * The app module is UI and nothing else: Jetpack Compose screens, one Activity, and the SDK.
 * Every button calls into :sdk, which calls into the C++ kernel through JNI - there is no second
 * implementation of anything in Kotlin, and no feature in the UI without a real engine call behind
 * it.
 */
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.avatarforge.app"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.avatarforge"
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        versionName = "0.10.1"

        /* Same ABIs as the SDK module; the native libraries arrive through the :sdk dependency. */
        /* Override the ABI list for a low-RAM build machine: -Paf.abIs=arm64-v8a builds one
           architecture instead of two, which halves both the native compile time and the peak
           memory. The default keeps both: arm64-v8a for devices, x86_64 for the emulator. */
        val abis = (project.findProperty("af.abIs") as String?)?.split(",")?.map { it.trim() }
            ?: listOf("arm64-v8a", "x86_64")
        ndk { abiFilters += abis }
    }

    /*
     * Release signing is fail-closed. A production release/AAB requires a real keystore supplied
     * through keystore.properties (ignored by git) or AF_RELEASE_* environment variables. CI may
     * compile an explicitly unsigned release artifact with -Paf.allowUnsignedRelease=true, but that
     * artifact is labelled UNSIGNED, is never published, and is not installable as an update.
     * There is no silent debug-keystore fallback for release.
     */
    val keystoreProps = rootProject.file("keystore.properties")
    val props = Properties().apply { if (keystoreProps.exists()) keystoreProps.inputStream().use { load(it) } }
    fun signingValue(propKey: String, envKey: String): String? =
        (props.getProperty(propKey) ?: System.getenv(envKey))?.takeIf { it.isNotBlank() }
    val releaseStorePath = signingValue("storeFile", "AF_RELEASE_STORE_FILE")
    val releaseStorePassword = signingValue("storePassword", "AF_RELEASE_STORE_PASSWORD")
    val releaseKeyAlias = signingValue("keyAlias", "AF_RELEASE_KEY_ALIAS")
    val releaseKeyPassword = signingValue("keyPassword", "AF_RELEASE_KEY_PASSWORD")
    val hasReleaseSigning = listOf(releaseStorePath, releaseStorePassword, releaseKeyAlias, releaseKeyPassword)
        .all { it != null } && file(releaseStorePath!!).isFile
    val allowUnsignedRelease = providers.gradleProperty("af.allowUnsignedRelease").orNull == "true"
    if (hasReleaseSigning) {
        signingConfigs.create("release") {
            storeFile = file(releaseStorePath!!)
            storePassword = releaseStorePassword!!
            keyAlias = releaseKeyAlias!!
            keyPassword = releaseKeyPassword!!
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            signingConfig = if (hasReleaseSigning) signingConfigs.getByName("release") else null
            /* R8 is off deliberately: the SDK is reached through JNI by name, and a shrinking pass
               that renamed the bridge class would fail at runtime rather than at build time. If
               shrinking is wanted later, the keep rules in consumer-rules.pro are already the
               complete set. */
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    buildFeatures {
        compose = true
    }
    composeOptions {
        /* Pinned to the Kotlin version in the root build file: the Compose compiler is a Kotlin
           compiler plugin, and a mismatch fails the build with a version warning that is easy to
           misread as a code error. */
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        resources.excludes += setOf("/META-INF/{AL2.0,LGPL2.1}")
    }
}

tasks.register("verifyReleaseSigning") {
    doLast {
        if (!hasReleaseSigning && !allowUnsignedRelease) {
            throw GradleException(
                "Release signing is BLOCKED: supply keystore.properties or AF_RELEASE_* credentials. " +
                "For compile-only CI artifacts use -Paf.allowUnsignedRelease=true; the result is UNSIGNED and must not be published."
            )
        }
        if (!hasReleaseSigning) {
            logger.warn("AvatarForge: building an explicitly UNSIGNED release artifact for compile verification only.")
        }
    }
}
tasks.matching { it.name == "preReleaseBuild" }.configureEach { dependsOn("verifyReleaseSigning") }

dependencies {
    implementation(project(":sdk"))

    implementation(platform("androidx.compose:compose-bom:2024.06.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.core:core-ktx:1.13.1")
    /* EXIF orientation: reference photos are routinely stored rotated, and the kernel expects an
       upright image (af_image in afcore.h). The app corrects it before handing pixels over. */
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
}
