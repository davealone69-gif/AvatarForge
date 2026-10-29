/*
 * ============================================================================================
 *  AvatarForge :: af_jni.cpp  --  the JNI bridge between the Kotlin SDK and libafcore/libafrender
 * ============================================================================================
 *
 *  This is the only file in the project that includes <jni.h>. It is deliberately a thin,
 *  mechanical translation layer:
 *
 *      * no policy, no defaults, no hidden behaviour - every method forwards to exactly one
 *        engine or renderer entry point and reports its af_result unchanged;
 *      * every failure is reported to Kotlin as an exception (IllegalStateException /
 *        IllegalArgumentException with the kernel's own message), never as a silent no-op;
 *      * no global state: engine and renderer handles are allocated per call and owned by Kotlin,
 *        and the callback trampoline is attached to the engine handle so it dies with it.
 *
 *  Threading: the engine is thread-affine (one thread at a time, see afcore.h). Kotlin calls
 *  these functions from its engine thread and, once the viewer is open, from the render thread -
 *  the SDK enforces that by transferring ownership, not by locking here.
 *
 *  The method names match com.avatarforge.sdk.internal.NativeBridge; if you rename one, rename it
 *  there too or the SDK stops linking (which is the point: the bridge has no dynamic dispatch).
 */
#include <jni.h>

#include <android/native_window_jni.h>

#include <cstring>
#include <string>
#include <vector>

#include "../../include/afcore.h"
#include "../af_image_pack.h"
#include "../render/af_render.h"
#include "../render/af_render_common.h"   /* screenRay / pickRegion */

namespace {

constexpr int kSpecIntCount = 7;
constexpr int kSpecFloatCount = 33;

/* ------------------------------------------------------------------ exception helpers ------ */
void throwIllegalState(JNIEnv* env, const char* what, af_result result, const std::string& detail) {
    std::string message = what;
    message += " failed (";
    message += std::to_string(int(result));
    message += "): ";
    message += detail.empty() ? af_engine_version() : detail;
    jclass cls = env->FindClass("java/lang/IllegalStateException");
    if (cls) env->ThrowNew(cls, message.c_str());
}

void throwIllegalArgument(JNIEnv* env, const std::string& message) {
    jclass cls = env->FindClass("java/lang/IllegalArgumentException");
    if (cls) env->ThrowNew(cls, message.c_str());
}

/* ------------------------------------------------------------------ engine handle ---------- */
/*
 *  Kotlin sees a jlong. Internally that is this struct, which keeps the callback trampoline's
 *  global references alive for exactly as long as the engine lives.
 */
struct EngineHandle {
    af_engine* engine = nullptr;
    jobject callbackObject = nullptr;   /* global ref to NativeBridge.Callbacks, or null */
    jmethodID onProgress = nullptr;
    jmethodID onLog = nullptr;
    jmethodID shouldCancel = nullptr;
};

thread_local JNIEnv* g_tlsEnv = nullptr;   /* set for the duration of each bridged call */

void trampolineProgress(void* user, int stage, float fraction, const char* message) {
    EngineHandle* h = static_cast<EngineHandle*>(user);
    if (!h || !h->callbackObject || !h->onProgress || !g_tlsEnv) return;
    jstring text = message ? g_tlsEnv->NewStringUTF(message) : nullptr;
    g_tlsEnv->CallVoidMethod(h->callbackObject, h->onProgress, jint(stage), jfloat(fraction), text);
    if (text) g_tlsEnv->DeleteLocalRef(text);
}

void trampolineLog(void* user, int severity, const char* message) {
    EngineHandle* h = static_cast<EngineHandle*>(user);
    if (!h || !h->callbackObject || !h->onLog || !g_tlsEnv) return;
    jstring text = message ? g_tlsEnv->NewStringUTF(message) : nullptr;
    g_tlsEnv->CallVoidMethod(h->callbackObject, h->onLog, jint(severity), text);
    if (text) g_tlsEnv->DeleteLocalRef(text);
}

int trampolineShouldCancel(void* user) {
    EngineHandle* h = static_cast<EngineHandle*>(user);
    if (!h || !h->callbackObject || !h->shouldCancel || !g_tlsEnv) return 0;
    return g_tlsEnv->CallBooleanMethod(h->callbackObject, h->shouldCancel) == JNI_TRUE ? 1 : 0;
}

/* ------------------------------------------------------------------ spec marshalling ------- */
bool readSpec(JNIEnv* env, jintArray ints, jfloatArray floats, jstring prompt, af_spec* outSpec) {
    if (!ints || !floats || !outSpec) {
        throwIllegalArgument(env, "spec arrays must not be null");
        return false;
    }
    const jsize intCount = env->GetArrayLength(ints);
    const jsize floatCount = env->GetArrayLength(floats);
    if (intCount != kSpecIntCount || floatCount != kSpecFloatCount) {
        throwIllegalArgument(env, "spec array sizes are " + std::to_string(intCount) + " ints / " +
                                      std::to_string(floatCount) + " floats, expected " +
                                      std::to_string(kSpecIntCount) + " / " +
                                      std::to_string(kSpecFloatCount));
        return false;
    }
    jint ivals[kSpecIntCount];
    jfloat fvals[kSpecFloatCount];
    env->GetIntArrayRegion(ints, 0, intCount, ivals);
    env->GetFloatArrayRegion(floats, 0, floatCount, fvals);

    af_spec_init(outSpec);
    outSpec->sex = ivals[1];
    outSpec->build = ivals[2];
    outSpec->hair_style = ivals[3];
    outSpec->outfit = ivals[4];
    outSpec->footwear = ivals[5];
    outSpec->seed = ivals[6];

    int i = 0;
    outSpec->height_m = fvals[i++];
    outSpec->age_years = fvals[i++];
    outSpec->muscle = fvals[i++];
    outSpec->body_fat = fvals[i++];
    outSpec->bust = fvals[i++];
    outSpec->neck_cm = fvals[i++];
    outSpec->chest_cm = fvals[i++];
    outSpec->underbust_cm = fvals[i++];
    outSpec->waist_cm = fvals[i++];
    outSpec->hip_cm = fvals[i++];
    outSpec->thigh_cm = fvals[i++];
    outSpec->calf_cm = fvals[i++];
    outSpec->bicep_cm = fvals[i++];
    outSpec->forearm_cm = fvals[i++];
    outSpec->wrist_cm = fvals[i++];
    outSpec->shoulder_width_cm = fvals[i++];
    outSpec->inseam_cm = fvals[i++];
    outSpec->head_circumference_cm = fvals[i++];
    for (int c = 0; c < 3; ++c) outSpec->skin_tone[c] = fvals[i++];
    outSpec->skin_melanin = fvals[i++];
    for (int c = 0; c < 3; ++c) outSpec->hair_tone[c] = fvals[i++];
    for (int c = 0; c < 3; ++c) outSpec->cloth_tone[c] = fvals[i++];
    for (int c = 0; c < 3; ++c) outSpec->gear_tone[c] = fvals[i++];
    outSpec->cloth_roughness = fvals[i++];
    outSpec->gear_metalness = fvals[i++];

    if (prompt) {
        const char* text = env->GetStringUTFChars(prompt, nullptr);
        if (text) {
            std::snprintf(outSpec->prompt, sizeof(outSpec->prompt), "%s", text);
            env->ReleaseStringUTFChars(prompt, text);
        }
    }
    return true;
}

void writeSpec(JNIEnv* env, const af_spec& spec, jintArray outInts, jfloatArray outFloats,
               jobjectArray outPrompt) {
    if (outInts && env->GetArrayLength(outInts) >= kSpecIntCount) {
        jint ivals[kSpecIntCount] = {int32_t(sizeof(af_spec)), spec.sex, spec.build, spec.hair_style,
                                     spec.outfit, spec.footwear, spec.seed};
        env->SetIntArrayRegion(outInts, 0, kSpecIntCount, ivals);
    }
    if (outFloats && env->GetArrayLength(outFloats) >= kSpecFloatCount) {
        jfloat fvals[kSpecFloatCount];
        int i = 0;
        fvals[i++] = spec.height_m; fvals[i++] = spec.age_years; fvals[i++] = spec.muscle;
        fvals[i++] = spec.body_fat; fvals[i++] = spec.bust;
        fvals[i++] = spec.neck_cm; fvals[i++] = spec.chest_cm; fvals[i++] = spec.underbust_cm;
        fvals[i++] = spec.waist_cm; fvals[i++] = spec.hip_cm; fvals[i++] = spec.thigh_cm;
        fvals[i++] = spec.calf_cm; fvals[i++] = spec.bicep_cm; fvals[i++] = spec.forearm_cm;
        fvals[i++] = spec.wrist_cm; fvals[i++] = spec.shoulder_width_cm; fvals[i++] = spec.inseam_cm;
        fvals[i++] = spec.head_circumference_cm;
        for (int c = 0; c < 3; ++c) fvals[i++] = spec.skin_tone[c];
        fvals[i++] = spec.skin_melanin;
        for (int c = 0; c < 3; ++c) fvals[i++] = spec.hair_tone[c];
        for (int c = 0; c < 3; ++c) fvals[i++] = spec.cloth_tone[c];
        for (int c = 0; c < 3; ++c) fvals[i++] = spec.gear_tone[c];
        fvals[i++] = spec.cloth_roughness; fvals[i++] = spec.gear_metalness;
        env->SetFloatArrayRegion(outFloats, 0, kSpecFloatCount, fvals);
    }
    if (outPrompt && env->GetArrayLength(outPrompt) >= 1) {
        jstring text = env->NewStringUTF(spec.prompt);
        env->SetObjectArrayElement(outPrompt, 0, text);
        env->DeleteLocalRef(text);
    }
}

EngineHandle* asHandle(jlong value) { return reinterpret_cast<EngineHandle*>(value); }

/* Runs one engine call and converts a failure into a Java exception. */
template <typename Fn>
jint guarded(JNIEnv* env, EngineHandle* h, const char* what, Fn&& fn) {
    if (!h || !h->engine) {
        throwIllegalArgument(env, std::string(what) + ": engine handle is not open");
        return AF_ERR_INVALID_ARG;
    }
    g_tlsEnv = env;
    const int32_t rc = fn(h->engine);
    g_tlsEnv = nullptr;
    if (rc < 0) throwIllegalState(env, what, af_result(rc), af_engine_last_error(h->engine));
    return rc;
}

} /* namespace */

/* ============================================================================================
 *  Kernel ABI surface
 * ============================================================================================ */
extern "C" {

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_abiVersion(JNIEnv*, jobject) {
    return af_engine_abi_version();
}

JNIEXPORT jstring JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineVersion(JNIEnv* env, jobject) {
    return env->NewStringUTF(af_engine_version());
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_imageAnalysisFloatCount(JNIEnv*, jobject) {
    return af::kImageAnalysisFloatCount;
}

JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineCreate(
    JNIEnv* env, jobject, jintArray ints, jfloatArray floats, jstring prompt, jobject callbacks) {
    af_spec spec{};
    if (!readSpec(env, ints, floats, prompt, &spec)) return 0;

    EngineHandle* handle = new EngineHandle();
    af_callbacks cb{};
    cb.user = handle;
    cb.on_progress = trampolineProgress;
    cb.on_log = trampolineLog;
    cb.should_cancel = trampolineShouldCancel;
    if (callbacks) {
        handle->callbackObject = env->NewGlobalRef(callbacks);
        jclass cls = env->GetObjectClass(callbacks);
        handle->onProgress = env->GetMethodID(cls, "onProgress", "(IFLjava/lang/String;)V");
        handle->onLog = env->GetMethodID(cls, "onLog", "(ILjava/lang/String;)V");
        handle->shouldCancel = env->GetMethodID(cls, "shouldCancel", "()Z");
        if (!handle->onProgress || !handle->onLog || !handle->shouldCancel) {
            env->DeleteGlobalRef(handle->callbackObject);
            handle->callbackObject = nullptr;
            delete handle;
            throwIllegalArgument(env, "Callbacks does not implement onProgress/onLog/shouldCancel");
            return 0;
        }
    }

    g_tlsEnv = env;
    const af_result rc = af_result(af_engine_create(&spec, callbacks ? &cb : nullptr, &handle->engine));
    g_tlsEnv = nullptr;
    if (rc != AF_OK) {
        if (handle->callbackObject) env->DeleteGlobalRef(handle->callbackObject);
        delete handle;
        throwIllegalState(env, "af_engine_create", rc, "the kernel refused the specification");
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineDestroy(JNIEnv* env, jobject, jlong h) {
    EngineHandle* handle = asHandle(h);
    if (!handle) return;
    if (handle->engine) af_engine_destroy(handle->engine);
    if (handle->callbackObject) env->DeleteGlobalRef(handle->callbackObject);
    delete handle;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineReset(
    JNIEnv* env, jobject, jlong h, jintArray ints, jfloatArray floats, jstring prompt) {
    EngineHandle* handle = asHandle(h);
    af_spec spec{};
    if (!readSpec(env, ints, floats, prompt, &spec)) return AF_ERR_INVALID_ARG;
    return guarded(env, handle, "af_engine_reset", [&](af_engine* e) { return af_engine_reset(e, &spec); });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineSetMemoryBudget(
    JNIEnv* env, jobject, jlong h, jlong bytes) {
    EngineHandle* handle = asHandle(h);
    return guarded(env, handle, "af_engine_set_memory_budget",
                   [&](af_engine* e) { return af_engine_set_memory_budget(e, uint64_t(bytes)); });
}

JNIEXPORT jstring JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineLastError(JNIEnv* env, jobject, jlong h) {
    EngineHandle* handle = asHandle(h);
    const char* text = (handle && handle->engine) ? af_engine_last_error(handle->engine) : "";
    return env->NewStringUTF(text ? text : "");
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineAnalyzePrompt(
    JNIEnv* env, jobject, jlong h, jstring prompt, jintArray outInts, jfloatArray outFloats,
    jobjectArray outPrompt) {
    EngineHandle* handle = asHandle(h);
    if (!prompt) {
        throwIllegalArgument(env, "prompt is null");
        return AF_ERR_INVALID_ARG;
    }
    const char* text = env->GetStringUTFChars(prompt, nullptr);
    if (!text) return AF_ERR_INVALID_ARG;
    af_spec spec{};
    af_spec_init(&spec);
    g_tlsEnv = env;
    const int32_t matched = (handle && handle->engine)
                                ? af_engine_analyze_prompt(handle->engine, text, &spec)
                                : AF_ERR_INVALID_ARG;
    g_tlsEnv = nullptr;
    env->ReleaseStringUTFChars(prompt, text);
    if (matched < 0) {
        throwIllegalState(env, "af_engine_analyze_prompt", af_result(matched),
                          handle ? af_engine_last_error(handle->engine) : "");
        return matched;
    }
    writeSpec(env, spec, outInts, outFloats, outPrompt);
    return matched;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineAnalyzeImages(
    JNIEnv* env, jobject, jlong h, jintArray widths, jintArray heights, jintArray channels,
    jintArray roles, jobjectArray pixels, jfloatArray outAnalyses) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) {
        throwIllegalArgument(env, "engine handle is not open");
        return AF_ERR_INVALID_ARG;
    }
    if (!widths || !heights || !channels || !roles || !pixels || !outAnalyses) {
        throwIllegalArgument(env, "image metadata, pixels and analysis output must not be null");
        return AF_ERR_INVALID_ARG;
    }

    const jsize count = env->GetArrayLength(widths);
    if (count < 1 || count > 4) {
        throwIllegalArgument(env, "1 to 4 reference images are supported per call");
        return AF_ERR_INVALID_ARG;
    }
    if (env->GetArrayLength(heights) != count || env->GetArrayLength(channels) != count ||
        env->GetArrayLength(roles) != count || env->GetArrayLength(pixels) != count) {
        throwIllegalArgument(env, "all image metadata and pixel arrays must have the same length");
        return AF_ERR_INVALID_ARG;
    }

    constexpr int perImage = af::kImageAnalysisFloatCount;
    const jsize expectedFloats = count * perImage;
    const jsize actualFloats = env->GetArrayLength(outAnalyses);
    if (actualFloats != expectedFloats) {
        throwIllegalArgument(env, "analysis output has " + std::to_string(actualFloats) +
                                     " floats; expected exactly " + std::to_string(expectedFloats));
        return AF_ERR_INVALID_ARG;
    }

    std::vector<af_image> images(static_cast<size_t>(count));
    jint w[4]{}, ht[4]{}, ch[4]{}, rl[4]{};
    env->GetIntArrayRegion(widths, 0, count, w);
    env->GetIntArrayRegion(heights, 0, count, ht);
    env->GetIntArrayRegion(channels, 0, count, ch);
    env->GetIntArrayRegion(roles, 0, count, rl);
    for (jsize i = 0; i < count; ++i) {
        if (w[i] <= 0 || ht[i] <= 0 || (ch[i] != 3 && ch[i] != 4)) {
            throwIllegalArgument(env, "each image needs positive dimensions and 3 RGB or 4 RGBA channels");
            return AF_ERR_INVALID_ARG;
        }
        if (rl[i] < AF_IMAGE_IDENTITY || rl[i] > AF_IMAGE_EXTRA_VIEW) {
            throwIllegalArgument(env, "image role is outside the af_image_role range");
            return AF_ERR_INVALID_ARG;
        }

        jobject buffer = env->GetObjectArrayElement(pixels, i);
        void* address = buffer ? env->GetDirectBufferAddress(buffer) : nullptr;
        const jlong capacity = buffer ? env->GetDirectBufferCapacity(buffer) : -1;
        const uint64_t required = static_cast<uint64_t>(w[i]) * static_cast<uint64_t>(ht[i]) *
                                  static_cast<uint64_t>(ch[i]);
        if (!address || capacity < 0 || static_cast<uint64_t>(capacity) < required) {
            if (buffer) env->DeleteLocalRef(buffer);
            throwIllegalArgument(env, "each image must use a sufficiently large direct ByteBuffer");
            return AF_ERR_INVALID_ARG;
        }
        images[size_t(i)].width = w[i];
        images[size_t(i)].height = ht[i];
        images[size_t(i)].channels = ch[i];
        images[size_t(i)].role = rl[i];
        images[size_t(i)].pixels = static_cast<const uint8_t*>(address);
        env->DeleteLocalRef(buffer);
    }

    std::vector<af_image_analysis> analyses(static_cast<size_t>(count));
    g_tlsEnv = env;
    const int32_t rc = af_engine_analyze_images(handle->engine, images.data(), int32_t(count),
                                                analyses.data(), int32_t(count));
    g_tlsEnv = nullptr;
    if (rc < 0) {
        throwIllegalState(env, "af_engine_analyze_images", af_result(rc), af_engine_last_error(handle->engine));
        return rc;
    }
    if (rc != count) {
        throwIllegalState(env, "af_engine_analyze_images", AF_ERR_INVALID_ARG,
                          "not every reference image produced a valid analysis");
        return AF_ERR_INVALID_ARG;
    }

    /* Exact 28-float records: every Lab value, including all three background components, is
       transferred. packImageAnalysis is also exercised by the host harness with guard sentinels. */
    std::vector<jfloat> packed(static_cast<size_t>(expectedFloats));
    for (jsize i = 0; i < count; ++i) {
        jfloat* record = &packed[size_t(i) * perImage];
        if (af::packImageAnalysis(analyses[size_t(i)], record, perImage) != perImage) {
            throwIllegalState(env, "packImageAnalysis", AF_ERR_INTERNAL,
                              "native image-analysis schema did not fill its declared stride");
            return AF_ERR_INTERNAL;
        }
    }
    env->SetFloatArrayRegion(outAnalyses, 0, expectedFloats, packed.data());
    return env->ExceptionCheck() ? AF_ERR_INTERNAL : rc;
}

/* Stages 2..8 are exposed through one entry point so Kotlin's JobHandle can drive the pipeline
   with a single enum rather than eight trampolines. */
JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineStage(
    JNIEnv* env, jobject, jlong h, jint stage) {
    EngineHandle* handle = asHandle(h);
    return guarded(env, handle, "stage", [&](af_engine* e) -> int32_t {
        switch (stage) {
            case AF_STAGE_FIT_ANATOMY:  return af_engine_fit_anatomy(e);
            case AF_STAGE_GEOMETRY:     return af_engine_build_geometry(e);
            case AF_STAGE_UV_UNWRAP:    return af_engine_unwrap_uv(e);
            case AF_STAGE_BAKE_TEXTURES:return af_engine_bake_textures(e);
            case AF_STAGE_RIG:          return af_engine_rig(e);
            case AF_STAGE_BLENDSHAPES:  return af_engine_bake_blendshapes(e);
            case AF_STAGE_ANIMATION:    return af_engine_bake_animation(e);
            default:                    return AF_ERR_INVALID_ARG;
        }
    });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineRunAll(JNIEnv* env, jobject, jlong h) {
    EngineHandle* handle = asHandle(h);
    return guarded(env, handle, "af_engine_run_all", [](af_engine* e) { return af_engine_run_all(e); });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineDirtyMask(JNIEnv* env, jobject, jlong h) {
    EngineHandle* handle = asHandle(h);
    return guarded(env, handle, "af_engine_dirty_mask", [](af_engine* e) { return af_engine_dirty_mask(e); });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineApplyPose(
    JNIEnv* env, jobject, jlong h, jfloatArray quats) {
    EngineHandle* handle = asHandle(h);
    if (!quats) {
        throwIllegalArgument(env, "pose is null");
        return AF_ERR_INVALID_ARG;
    }
    const jsize count = env->GetArrayLength(quats);
    std::vector<float> values;
    values.resize(size_t(count));
    env->GetFloatArrayRegion(quats, 0, count, values.data());
    af_pose pose{values.data(), count / 4};
    return guarded(env, handle, "af_engine_apply_pose",
                   [&](af_engine* e) { return af_engine_apply_pose(e, &pose); });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineSampleClip(
    JNIEnv* env, jobject, jlong h, jint clip, jfloat time, jfloatArray outQuats) {
    EngineHandle* handle = asHandle(h);
    if (!outQuats) {
        throwIllegalArgument(env, "outQuats is null");
        return AF_ERR_INVALID_ARG;
    }
    const jsize count = env->GetArrayLength(outQuats);
    std::vector<float> values;
    values.resize(size_t(count));
    af_pose pose{values.data(), count / 4};
    const jint rc = guarded(env, handle, "af_engine_sample_clip",
                            [&](af_engine* e) { return af_engine_sample_clip(e, clip, time, &pose); });
    if (rc >= 0) env->SetFloatArrayRegion(outQuats, 0, count, values.data());
    return rc;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineApplyClip(
    JNIEnv* env, jobject, jlong h, jint clip, jfloat time) {
    EngineHandle* handle = asHandle(h);
    return guarded(env, handle, "af_engine_apply_clip",
                   [&](af_engine* e) { return af_engine_apply_clip(e, clip, time); });
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineApplyBlendshapes(
    JNIEnv* env, jobject, jlong h, jfloatArray weights) {
    EngineHandle* handle = asHandle(h);
    if (!weights) {
        throwIllegalArgument(env, "weights is null");
        return AF_ERR_INVALID_ARG;
    }
    const jsize count = env->GetArrayLength(weights);
    std::vector<float> values;
    values.resize(size_t(count));
    env->GetFloatArrayRegion(weights, 0, count, values.data());
    return guarded(env, handle, "af_engine_apply_blendshapes",
                   [&](af_engine* e) { return af_engine_apply_blendshapes(e, values.data(), int32_t(count)); });
}

JNIEXPORT jobjectArray JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineBlendshapeNames(
    JNIEnv* env, jobject) {
    char names[64][64];
    const int count = af_engine_blendshape_names(names, 64);
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray(count > 0 ? count : 0, stringClass, nullptr);
    for (int i = 0; i < count; ++i) {
        jstring text = env->NewStringUTF(names[i]);
        env->SetObjectArrayElement(array, i, text);
        env->DeleteLocalRef(text);
    }
    return array;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineClipCount(JNIEnv*, jobject) {
    return int(AF_CLIP_COUNT);
}

JNIEXPORT jstring JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineClipName(JNIEnv* env, jobject, jint clip) {
    char buffer[64] = {0};
    const int32_t len = af_engine_clip_name(clip, buffer, int32_t(sizeof(buffer)));
    if (len <= 0) return env->NewStringUTF("rest");
    return env->NewStringUTF(buffer);
}

JNIEXPORT jfloat JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineClipDuration(
    JNIEnv*, jobject, jint clip) {
    return af_engine_clip_duration(nullptr, clip);
}

JNIEXPORT jstring JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineRegionName(JNIEnv* env, jobject, jint region) {
    return env->NewStringUTF(af_engine_region_name(region));
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineCounts(
    JNIEnv* env, jobject, jlong h, jintArray out) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine || !out || env->GetArrayLength(out) < 5) {
        throwIllegalArgument(env, "engineCounts needs an open engine and a 5-element array");
        return AF_ERR_INVALID_ARG;
    }
    const jint values[5] = {af_engine_vertex_count(handle->engine), af_engine_index_count(handle->engine),
                            af_engine_submesh_count(handle->engine), af_engine_joint_count(handle->engine),
                            af_engine_material_count(handle->engine)};
    env->SetIntArrayRegion(out, 0, 5, values);
    return AF_OK;
}

JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineVertexBytes(
    JNIEnv*, jobject, jlong h) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return 0;
    return jlong(af_engine_vertex_count(handle->engine)) * jlong(sizeof(af_vertex));
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineCopyVertices(
    JNIEnv* env, jobject, jlong h, jobject dst, jboolean rest) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine || !dst) {
        throwIllegalArgument(env, "copyVertices needs an open engine and a direct ByteBuffer");
        return AF_ERR_INVALID_ARG;
    }
    void* target = env->GetDirectBufferAddress(dst);
    const int32_t count = af_engine_vertex_count(handle->engine);
    const af_vertex* source = rest ? af_engine_vertices_rest(handle->engine) : af_engine_vertices(handle->engine);
    if (!target || !source || count <= 0) return 0;
    const jlong capacity = env->GetDirectBufferCapacity(dst);
    const jlong needed = jlong(count) * jlong(sizeof(af_vertex));
    if (capacity < needed) {
        throwIllegalArgument(env, "destination buffer is smaller than " + std::to_string(needed) + " bytes");
        return AF_ERR_INVALID_ARG;
    }
    std::memcpy(target, source, size_t(needed));
    return count;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineCopyIndices(
    JNIEnv* env, jobject, jlong h, jobject dst) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine || !dst) {
        throwIllegalArgument(env, "copyIndices needs an open engine and a direct ByteBuffer");
        return AF_ERR_INVALID_ARG;
    }
    void* target = env->GetDirectBufferAddress(dst);
    const int32_t count = af_engine_index_count(handle->engine);
    const uint32_t* source = af_engine_indices(handle->engine);
    if (!target || !source || count <= 0) return 0;
    const jlong needed = jlong(count) * jlong(sizeof(uint32_t));
    if (env->GetDirectBufferCapacity(dst) < needed) {
        throwIllegalArgument(env, "destination buffer is smaller than " + std::to_string(needed) + " bytes");
        return AF_ERR_INVALID_ARG;
    }
    std::memcpy(target, source, size_t(needed));
    return count;
}

/* Submeshes are packed as ints per element:
   {region, vertex_offset, vertex_count, index_offset, index_count, material_index}
   followed by 6 floats {bounds_min[3], bounds_max[3]}. */
JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineSubmeshInfo(
    JNIEnv* env, jobject, jlong h, jintArray outInts, jfloatArray outFloats) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return AF_ERR_INVALID_ARG;
    const int count = af_engine_submesh_count(handle->engine);
    const af_submesh* list = af_engine_submeshes(handle->engine);
    if (count <= 0 || !list) return 0;
    if (outInts && env->GetArrayLength(outInts) >= count * 6) {
        std::vector<jint> packed(size_t(count) * 6);
        for (int i = 0; i < count; ++i) {
            packed[size_t(i) * 6 + 0] = list[i].region;
            packed[size_t(i) * 6 + 1] = list[i].vertex_offset;
            packed[size_t(i) * 6 + 2] = list[i].vertex_count;
            packed[size_t(i) * 6 + 3] = list[i].index_offset;
            packed[size_t(i) * 6 + 4] = list[i].index_count;
            packed[size_t(i) * 6 + 5] = list[i].material_index;
        }
        env->SetIntArrayRegion(outInts, 0, count * 6, packed.data());
    }
    if (outFloats && env->GetArrayLength(outFloats) >= count * 6) {
        std::vector<jfloat> packed(size_t(count) * 6);
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                packed[size_t(i) * 6 + k] = list[i].bounds_min[k];
                packed[size_t(i) * 6 + 3 + k] = list[i].bounds_max[k];
            }
        }
        env->SetFloatArrayRegion(outFloats, 0, count * 6, packed.data());
    }
    return count;
}

/* Materials: 9 floats each (base colour rgba, metallic, roughness, ior, sheen, subsurface) plus
   4 texture flags packed into the tail. */
JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineMaterialInfo(
    JNIEnv* env, jobject, jlong h, jobjectArray outNames, jfloatArray outFloats) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return AF_ERR_INVALID_ARG;
    const int count = af_engine_material_count(handle->engine);
    const af_material* list = af_engine_materials(handle->engine);
    if (count <= 0 || !list) return 0;
    if (outNames && env->GetArrayLength(outNames) >= count) {
        for (int i = 0; i < count; ++i) {
            jstring text = env->NewStringUTF(list[i].name);
            env->SetObjectArrayElement(outNames, i, text);
            env->DeleteLocalRef(text);
        }
    }
    if (outFloats && env->GetArrayLength(outFloats) >= count * 13) {
        std::vector<jfloat> packed(size_t(count) * 13);
        for (int i = 0; i < count; ++i) {
            jfloat* p = &packed[size_t(i) * 13];
            for (int k = 0; k < 4; ++k) p[k] = list[i].base_color_factor[k];
            p[4] = list[i].metallic;
            p[5] = list[i].roughness;
            p[6] = list[i].ior;
            p[7] = list[i].sheen;
            p[8] = list[i].subsurface;
            p[9] = list[i].anisotropic;
            p[10] = float(list[i].has_base_color_tex);
            p[11] = float(list[i].has_normal_tex + list[i].has_mr_tex * 2 + list[i].has_ao_tex * 4);
            p[12] = list[i].sheen_tint;
        }
        env->SetFloatArrayRegion(outFloats, 0, count * 13, packed.data());
    }
    return count;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineJointInfo(
    JNIEnv* env, jobject, jlong h, jint joint, jintArray outInts, jfloatArray outFloats, jobjectArray outName) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return AF_ERR_INVALID_ARG;
    af_joint_info info{};
    const int32_t rc = af_engine_joint_info(handle->engine, joint, &info);
    if (rc != AF_OK) {
        throwIllegalState(env, "af_engine_joint_info", af_result(rc), af_engine_last_error(handle->engine));
        return rc;
    }
    if (outInts && env->GetArrayLength(outInts) >= 2) {
        const jint values[2] = {info.parent, info.flags};
        env->SetIntArrayRegion(outInts, 0, 2, values);
    }
    if (outFloats && env->GetArrayLength(outFloats) >= 7) {
        jfloat values[7];
        for (int i = 0; i < 3; ++i) values[i] = info.rest_translation[i];
        for (int i = 0; i < 4; ++i) values[3 + i] = info.rest_rotation[i];
        env->SetFloatArrayRegion(outFloats, 0, 7, values);
    }
    if (outName && env->GetArrayLength(outName) >= 1) {
        jstring text = env->NewStringUTF(info.name);
        env->SetObjectArrayElement(outName, 0, text);
        env->DeleteLocalRef(text);
    }
    return AF_OK;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineJointMatrices(
    JNIEnv* env, jobject, jlong h, jfloatArray out) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine || !out) {
        throwIllegalArgument(env, "jointMatrices needs an open engine and a float array");
        return AF_ERR_INVALID_ARG;
    }
    const int joints = af_engine_joint_count(handle->engine);
    if (env->GetArrayLength(out) < joints * 16) {
        throwIllegalArgument(env, "joint matrix array is too small");
        return AF_ERR_INVALID_ARG;
    }
    std::vector<float> values(size_t(joints) * 16);
    const int32_t rc = af_engine_joint_matrices(handle->engine, values.data(), joints);
    if (rc >= 0) env->SetFloatArrayRegion(out, 0, joints * 16, values.data());
    return rc;
}

JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineTextureInfo(
    JNIEnv* env, jobject, jlong h, jint slot, jintArray out) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return 0;
    af_texture tex{};
    const int32_t rc = af_engine_texture(handle->engine, slot, &tex);
    if (rc != AF_OK) return 0;
    if (out && env->GetArrayLength(out) >= 3) {
        const jint values[3] = {tex.width, tex.height, tex.channels};
        env->SetIntArrayRegion(out, 0, 3, values);
    }
    return jlong(tex.byte_size);
}

JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineCopyTexture(
    JNIEnv* env, jobject, jlong h, jint slot, jobject dst) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine || !dst) {
        throwIllegalArgument(env, "copyTexture needs an open engine and a direct ByteBuffer");
        return 0;
    }
    af_texture tex{};
    const int32_t rc = af_engine_texture(handle->engine, slot, &tex);
    if (rc != AF_OK) {
        throwIllegalState(env, "af_engine_texture", af_result(rc), af_engine_last_error(handle->engine));
        return 0;
    }
    void* target = env->GetDirectBufferAddress(dst);
    if (!target || env->GetDirectBufferCapacity(dst) < jlong(tex.byte_size)) {
        throwIllegalArgument(env, "destination buffer is too small for the texture");
        return 0;
    }
    std::memcpy(target, tex.data, size_t(tex.byte_size));
    return jlong(tex.byte_size);
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineExportGlb(
    JNIEnv* env, jobject, jlong h, jstring path) {
    EngineHandle* handle = asHandle(h);
    if (!path) {
        throwIllegalArgument(env, "path is null");
        return AF_ERR_INVALID_ARG;
    }
    const char* text = env->GetStringUTFChars(path, nullptr);
    if (!text) return AF_ERR_INVALID_ARG;
    const jint rc = guarded(env, handle, "af_engine_export_glb",
                            [&](af_engine* e) { return af_engine_export_glb(e, text); });
    env->ReleaseStringUTFChars(path, text);
    return rc;
}

/* Two-call protocol: with a null buffer it returns the size, otherwise it copies and returns the
   number of bytes written. The SDK caches the export bytes after the first call. */
JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineExportGlbBytes(
    JNIEnv* env, jobject, jlong h, jobject dst) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return 0;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    g_tlsEnv = env;
    const af_result rc = af_result(af_engine_export_glb_to_memory(handle->engine, &bytes, &size));
    g_tlsEnv = nullptr;
    if (rc != AF_OK) {
        throwIllegalState(env, "af_engine_export_glb_to_memory", rc, af_engine_last_error(handle->engine));
        return 0;
    }
    if (!dst) return jlong(size);
    void* target = env->GetDirectBufferAddress(dst);
    if (!target || env->GetDirectBufferCapacity(dst) < jlong(size)) {
        throwIllegalArgument(env, "destination buffer is too small for the .glb");
        return 0;
    }
    std::memcpy(target, bytes, size);
    af_arena_free(bytes);
    return jlong(size);
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_engineStats(
    JNIEnv* env, jobject, jlong h, jfloatArray outFloats, jlongArray outLongs) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return AF_ERR_INVALID_ARG;
    af_stats stats{};
    const int32_t rc = af_engine_stats(handle->engine, &stats);
    if (rc != AF_OK) return rc;
    if (outFloats && env->GetArrayLength(outFloats) >= 9) {
        const jfloat values[9] = {stats.fit_anatomy_ms, stats.field_ms, stats.isosurface_ms, stats.garment_ms,
                                  stats.hair_ms, stats.uv_ms, stats.texture_ms, stats.rig_ms, stats.export_ms};
        env->SetFloatArrayRegion(outFloats, 0, 9, values);
    }
    if (outLongs && env->GetArrayLength(outLongs) >= 11) {
        const jlong values[11] = {stats.vertex_count, stats.index_count, stats.triangle_count,
                                  stats.submesh_count, stats.joint_count, stats.blendshape_count,
                                  jlong(stats.texture_bytes), jlong(stats.geometry_bytes),
                                  jlong(stats.peak_field_bytes), 0, 0};
        env->SetLongArrayRegion(outLongs, 0, 11, values);
    }
    return AF_OK;
}

/* Region names are exposed to the UI through af_engine_region_name(); the picker below is what
   turns a tap into one of them. */
JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_enginePickRegion(
    JNIEnv* env, jobject, jlong h, jfloat yawDeg, jfloat pitchDeg, jfloat distance, jfloat targetY,
    jfloat fovDeg, jfloat ndcX, jfloat ndcY, jint width, jint height, jfloatArray outPoint) {
    EngineHandle* handle = asHandle(h);
    if (!handle || !handle->engine) return -1;
    af::CameraState camera{};
    camera.yawDeg = yawDeg;
    camera.pitchDeg = pitchDeg;
    camera.distance = distance;
    camera.targetY = targetY;
    camera.fovDeg = fovDeg;
    float origin[3], direction[3];
    af::screenRay(camera, ndcX, ndcY, width, height, origin, direction);
    float distanceAlongRay = 0.0f;
    float point[3] = {0, 0, 0};
    const int region = af::pickRegion(handle->engine, origin, direction, &distanceAlongRay, point);
    if (outPoint && env->GetArrayLength(outPoint) >= 4) {
        const jfloat values[4] = {point[0], point[1], point[2], distanceAlongRay};
        env->SetFloatArrayRegion(outPoint, 0, 4, values);
    }
    return region;
}

} /* extern "C" */

/* ============================================================================================
 *  Renderer surface
 * ============================================================================================ */
extern "C" {

JNIEXPORT jlong JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererCreate(
    JNIEnv* env, jobject, jobject surface, jint quality, jint msaa,
    jint shadowSize, jint bloomLevels) {
    if (!surface) {
        throwIllegalArgument(env, "rendererCreate needs a live Surface");
        return 0;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        throwIllegalState(env, "ANativeWindow_fromSurface", AF_ERR_INTERNAL,
                          "the Surface has no native window (is it already released?)");
        return 0;
    }
    af::RendererOptions options{};
    options.quality = af::QualityPreset(quality);
    options.msaaSamples = msaa;
    options.shadowMapSize = shadowSize;
    options.bloomLevels = bloomLevels;

    std::string error;
    af::Renderer* renderer = af::Renderer::create(window, options, &error);
    ANativeWindow_release(window);   /* the renderer keeps its own reference through the surface */
    if (!renderer) {
        throwIllegalState(env, "Renderer::create", AF_ERR_UNSUPPORTED, error);
        return 0;
    }
    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererDestroy(JNIEnv*, jobject, jlong r) {
    delete reinterpret_cast<af::Renderer*>(r);
}

JNIEXPORT jboolean JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererResize(
    JNIEnv* env, jobject, jlong r, jint width, jint height) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) {
        throwIllegalArgument(env, "renderer handle is not open");
        return JNI_FALSE;
    }
    return renderer->resize(width, height) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererAttachEngine(
    JNIEnv* env, jobject, jlong r, jlong h) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    EngineHandle* handle = asHandle(h);
    if (!renderer || !handle) {
        throwIllegalArgument(env, "rendererAttachEngine needs both an open renderer and an engine");
        return AF_ERR_INVALID_ARG;
    }
    renderer->attachEngine(handle->engine);
    return AF_OK;
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererSetCamera(
    JNIEnv*, jobject, jlong r, jfloat yaw, jfloat pitch, jfloat distance, jfloat targetY, jfloat fov) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return;
    af::CameraState camera{};
    camera.yawDeg = yaw;
    camera.pitchDeg = pitch;
    camera.distance = distance;
    camera.targetY = targetY;
    camera.fovDeg = fov;
    renderer->setCamera(camera);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererSetOptions(
    JNIEnv*, jobject, jlong r, jint quality, jboolean shadows, jboolean bloom, jboolean sss,
    jfloat exposure, jfloat sunAzimuth, jfloat sunElevation, jfloat sunIntensity, jfloat turbidity,
    jfloat environmentIntensity, jboolean groundPlane, jint msaa,
    jint shadowSize, jint bloomLevels, jfloat shadowSoftness) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return;
    af::RendererOptions options = renderer->options();
    options.quality = af::QualityPreset(quality);
    options.enableShadows = shadows == JNI_TRUE;
    options.enableBloom = bloom == JNI_TRUE;
    options.enableSss = sss == JNI_TRUE;
    options.exposure = exposure;
    options.sunAzimuthDeg = sunAzimuth;
    options.sunElevationDeg = sunElevation;
    options.sunIntensity = sunIntensity;
    options.turbidity = turbidity;
    options.environmentIntensity = environmentIntensity;
    options.groundPlane = groundPlane == JNI_TRUE;
    options.msaaSamples = msaa;
    options.shadowMapSize = shadowSize;
    options.bloomLevels = bloomLevels;
    options.shadowSoftness = shadowSoftness;
    renderer->setOptions(options);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererSetOverlays(
    JNIEnv*, jobject, jlong r, jboolean skeleton, jboolean flat, jint highlightRegion) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return;
    renderer->setSkeletonVisible(skeleton == JNI_TRUE);
    renderer->setFlatShading(flat == JNI_TRUE);
    renderer->setHighlightRegion(highlightRegion);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererSetClip(
    JNIEnv*, jobject, jlong r, jint clip, jfloat speed, jboolean loop) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return;
    renderer->setClip(clip, speed, loop == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererInvalidateMesh(
    JNIEnv*, jobject, jlong r) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (renderer) renderer->invalidateMesh();
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererFrame(
    JNIEnv*, jobject, jlong r, jfloat deltaSeconds) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (renderer) renderer->renderFrame(deltaSeconds);
}

JNIEXPORT void JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererWaitIdle(JNIEnv*, jobject, jlong r) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (renderer) renderer->waitIdle();
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererInfo(
    JNIEnv* env, jobject, jlong r, jobjectArray outStrings, jintArray outFlags) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return AF_ERR_INVALID_ARG;
    const af::RendererInfo info = renderer->info();
    std::string driverSummary = info.driver;
    if (!info.gpuProfile.empty()) {
        if (!driverSummary.empty()) driverSummary += " | ";
        driverSummary += info.gpuProfile;
        if (!info.textureCompression.empty()) driverSummary += " | " + info.textureCompression;
    }
    if (outStrings && env->GetArrayLength(outStrings) >= 4) {
        const char* texts[4] = {info.backend.c_str(), info.deviceName.c_str(), info.apiVersion.c_str(),
                                driverSummary.c_str()};
        for (int i = 0; i < 4; ++i) {
            jstring text = env->NewStringUTF(texts[i]);
            env->SetObjectArrayElement(outStrings, i, text);
            env->DeleteLocalRef(text);
        }
    }
    if (outFlags && env->GetArrayLength(outFlags) >= 4) {
        const jint flags[4] = {info.supportsShadows ? 1 : 0, info.supportsMsaa ? 1 : 0,
                               info.supportsFloatHdr ? 1 : 0, info.supportsAnisotropy ? 1 : 0};
        env->SetIntArrayRegion(outFlags, 0, 4, flags);
    }
    return AF_OK;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererStats(
    JNIEnv* env, jobject, jlong r, jfloatArray outFloats, jintArray outInts) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    if (!renderer) return AF_ERR_INVALID_ARG;
    const af::RendererStats stats = renderer->stats();
    if (outFloats && env->GetArrayLength(outFloats) >= 4) {
        const jfloat values[4] = {stats.frameMs, stats.cpuMs, stats.fps,
                                  float(stats.textureBytes) / (1024.0f * 1024.0f)};
        env->SetFloatArrayRegion(outFloats, 0, 4, values);
    }
    if (outInts && env->GetArrayLength(outInts) >= 9) {
        const jint values[9] = {stats.drawCalls, stats.triangles, stats.vertices, stats.shadowMapSize,
                                stats.msaaSamples, stats.bloomLevels, stats.qualityLevel,
                                stats.lastBackendError, stats.frameFatal ? 1 : 0};
        env->SetIntArrayRegion(outInts, 0, 9, values);
        // Optional tail preserves callers using the existing nine-int telemetry layout.
        if (env->GetArrayLength(outInts) >= 10) {
            const jint sequence = stats.frames;
            env->SetIntArrayRegion(outInts, 9, 1, &sequence);
        }
        if (env->GetArrayLength(outInts) >= 11) {
            const jint value = stats.activeLod;
            env->SetIntArrayRegion(outInts, 10, 1, &value);
        }
        if (env->GetArrayLength(outInts) >= 12) {
            const jint value = stats.availableLods;
            env->SetIntArrayRegion(outInts, 11, 1, &value);
        }
        if (env->GetArrayLength(outInts) >= 13) {
            const jint value = stats.triangleBudgetExceeded ? 1 : 0;
            env->SetIntArrayRegion(outInts, 12, 1, &value);
        }
    }
    return AF_OK;
}

JNIEXPORT jint JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererCurrentClip(JNIEnv*, jobject, jlong r) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    return renderer ? renderer->currentClip() : -1;
}

JNIEXPORT jfloat JNICALL Java_com_avatarforge_sdk_internal_NativeBridge_rendererClipTime(JNIEnv*, jobject, jlong r) {
    af::Renderer* renderer = reinterpret_cast<af::Renderer*>(r);
    return renderer ? renderer->clipTime() : 0.0f;
}

} /* extern "C" */
