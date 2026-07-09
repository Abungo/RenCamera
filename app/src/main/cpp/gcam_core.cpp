#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <any>
#include <fstream>
#include <cstdio>

#include "pipeline.h"
#include "frame_context.h"
#include "debug_utils.h"
#include "stages/align_stage.h"
#include "stages/fusion_stage.h"
#include "stages/debayer_stage.h"
#include "stages/tonemap_stage.h"
#include "stages/encode_stage.h"

#define LOG_TAG "RenCamera/JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// buildPipeline — assemble the default stage order
//
// Adding a new stage later:
//   1. Include its header here.
//   2. Add one addStage() call below (e.g., after tonemap, before encode).
//   3. Set its flag via setEnabled() if needed.
// ─────────────────────────────────────────────────────────────────────────────
static Pipeline buildPipeline(
    bool enableAlign,
    bool enableFuse,
    bool enableDebayer,
    bool enableTonemap,
    bool enableEncode)
{
    Pipeline p;
    p.addStage(std::make_unique<AlignStage>());
    p.addStage(std::make_unique<FusionStage>());
    p.addStage(std::make_unique<DebayerStage>());
    // ── Future stages inserted here, e.g.:
    // p.addStage(std::make_unique<DenoiseStage>());
    p.addStage(std::make_unique<ToneMapStage>());
    p.addStage(std::make_unique<EncodeStage>());

    p.setEnabled("align",   enableAlign);
    p.setEnabled("fuse",    enableFuse);
    p.setEnabled("debayer", enableDebayer);
    p.setEnabled("tonemap", enableTonemap);
    p.setEnabled("encode",  enableEncode);
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// JNI entry point
//   Kotlin signature:
//     external fun processBurst(
//         count: Int,
//         yPlanes:  Array<ByteBuffer>,
//         uPlanes:  Array<ByteBuffer>,
//         vPlanes:  Array<ByteBuffer>,
//         yRowStrides:    IntArray,
//         uvRowStrides:   IntArray,
//         uvPixelStrides: IntArray,
//         width: Int, height: Int,
//         jpegQuality: Int,
//         stageFlags: BooleanArray   // [align, fuse, debayer, tonemap, encode]
//     ): ByteArray?
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Native memory copying structures to support queueing multiple burst tasks
// without exhausting hardware ImageReader slots or Java direct memory limits.
// ─────────────────────────────────────────────────────────────────────────────
struct CopiedFrame {
    std::vector<uint8_t> yData;
    std::vector<uint8_t> uData;
    std::vector<uint8_t> vData;
    int yRowStride;
    int uvRowStride;
    int uvPixelStride;
    int width;
    int height;
};

struct CopiedBurst {
    std::vector<CopiedFrame> frames;
};

extern "C" JNIEXPORT jlong JNICALL
Java_com_renskylab_camera_NativeEngine_copyBurst(
    JNIEnv* env, jobject /* thiz */,
    jint    count,
    jobjectArray yPlanes,
    jobjectArray uPlanes,
    jobjectArray vPlanes,
    jintArray    yRowStrides,
    jintArray    uvRowStrides,
    jintArray    uvPixelStrides,
    jint width, jint height)
{
    if (count <= 0) {
        LOGE("copyBurst: count=%d invalid", count);
        return 0;
    }

    jint* yStrides  = env->GetIntArrayElements(yRowStrides,    nullptr);
    jint* uvRStride = env->GetIntArrayElements(uvRowStrides,   nullptr);
    jint* uvPStride = env->GetIntArrayElements(uvPixelStrides, nullptr);

    auto* burst = new CopiedBurst();
    burst->frames.resize(count);

    for (int i = 0; i < count; ++i) {
        jobject yBuf = env->GetObjectArrayElement(yPlanes, i);
        jobject uBuf = env->GetObjectArrayElement(uPlanes, i);
        jobject vBuf = env->GetObjectArrayElement(vPlanes, i);

        const uint8_t* yPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuf));
        const uint8_t* uPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(uBuf));
        const uint8_t* vPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(vBuf));

        if (!yPtr || !uPtr || !vPtr) {
            LOGE("copyBurst: Direct buffer pointer is NULL! Y=%p, U=%p, V=%p", yPtr, uPtr, vPtr);
            env->DeleteLocalRef(yBuf);
            env->DeleteLocalRef(uBuf);
            env->DeleteLocalRef(vBuf);
            return 0;
        }

        jlong yLength = env->GetDirectBufferCapacity(yBuf);
        if (yLength <= 0) {
            yLength = static_cast<jlong>(yStrides[i]) * height;
        }
        if (yLength < 0) yLength = 0;

        jlong uLength = env->GetDirectBufferCapacity(uBuf);
        if (uLength <= 0) {
            uLength = static_cast<jlong>(uvRStride[i]) * (height / 2);
        }
        if (uLength < 0) uLength = 0;

        jlong vLength = env->GetDirectBufferCapacity(vBuf);
        if (vLength <= 0) {
            vLength = static_cast<jlong>(uvRStride[i]) * (height / 2);
        }
        if (vLength < 0) vLength = 0;

        CopiedFrame& f = burst->frames[i];
        f.width = width;
        f.height = height;
        f.yRowStride = yStrides[i];
        f.uvRowStride = uvRStride[i];
        f.uvPixelStride = uvPStride[i];

        // Allocate and copy data
        f.yData.assign(yPtr, yPtr + yLength);
        f.uData.assign(uPtr, uPtr + uLength);
        f.vData.assign(vPtr, vPtr + vLength);

        env->DeleteLocalRef(yBuf);
        env->DeleteLocalRef(uBuf);
        env->DeleteLocalRef(vBuf);
    }

    env->ReleaseIntArrayElements(yRowStrides,    yStrides,  JNI_ABORT);
    env->ReleaseIntArrayElements(uvRowStrides,   uvRStride, JNI_ABORT);
    env->ReleaseIntArrayElements(uvPixelStrides, uvPStride, JNI_ABORT);

    LOGI("copyBurst: copied %d frames to native heap (handle = %p)", count, burst);
    return reinterpret_cast<jlong>(burst);
}

extern "C" JNIEXPORT void JNICALL
Java_com_renskylab_camera_NativeEngine_freeBurst(
    JNIEnv* env, jobject /* thiz */,
    jlong handle)
{
    if (handle == 0) return;
    auto* burst = reinterpret_cast<CopiedBurst*>(handle);
    delete burst;
    LOGI("freeBurst: released native burst memory (handle = %lld)", static_cast<long long>(handle));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_renskylab_camera_NativeEngine_saveRawBurst(
    JNIEnv* env, jobject /* thiz */,
    jlong handle,
    jstring dirPathStr,
    jboolean useRaw)
{
    auto* burst = reinterpret_cast<CopiedBurst*>(handle);
    if (!burst || burst->frames.empty()) {
        LOGE("saveRawBurst: invalid handle or empty burst");
        return JNI_FALSE;
    }

    const char* dirPath = env->GetStringUTFChars(dirPathStr, nullptr);
    std::string path(dirPath);
    env->ReleaseStringUTFChars(dirPathStr, dirPath);

    for (size_t i = 0; i < burst->frames.size(); ++i) {
        const CopiedFrame& f = burst->frames[i];
        
        if (useRaw) {
            char filename[128];
            snprintf(filename, sizeof(filename), "/frame_%02zu_%dx%d.raw", i, f.width, f.height);
            std::string filePath = path + filename;
            
            std::ofstream out(filePath, std::ios::binary);
            if (!out) {
                LOGE("saveRawBurst: failed to open file %s for writing", filePath.c_str());
                return JNI_FALSE;
            }
            out.write(reinterpret_cast<const char*>(f.yData.data()), f.yData.size());
            out.close();

            // Create a grayscale JPEG preview representing the RAW sensor frame
            char jpegFilename[128];
            snprintf(jpegFilename, sizeof(jpegFilename), "/frame_%02zu_%dx%d.jpg", i, f.width, f.height);
            std::string jpegFilePath = path + jpegFilename;

            // Compute normalized 8-bit Y plane from 16-bit raw data
            int pixelCount = f.width * f.height;
            std::vector<uint8_t> previewY(pixelCount);
            const uint16_t* rawData = reinterpret_cast<const uint16_t*>(f.yData.data());
            
            // Sony default: black = 1024, white = 4095
            float blackLevel = 1024.f;
            float whiteLevel = 4095.f;
            
            // Dynamically check range to support different bit depths in preview
            uint16_t maxVal = 0;
            for (int p = 0; p < std::min(pixelCount, 10000); ++p) {
                if (rawData[p] > maxVal) maxVal = rawData[p];
            }
            if (maxVal <= 1023) {
                blackLevel = 64.f;
                whiteLevel = 1023.f;
            }

            float scale = 255.f / std::max(1.f, whiteLevel - blackLevel);
            int strideElements = f.yRowStride / 2;
            for (int r = 0; r < f.height; ++r) {
                for (int c = 0; c < f.width; ++c) {
                    float cleanVal = (static_cast<float>(rawData[r * strideElements + c]) - blackLevel) * scale;
                    previewY[r * f.width + c] = static_cast<uint8_t>(std::clamp(cleanVal, 0.f, 255.f));
                }
            }

            std::vector<uint8_t> uvGray(pixelCount / 2, 128);
            saveYuvAsJpeg(previewY.data(), uvGray.data(), uvGray.data(), f.width, f.height, jpegFilePath);
        } else {
            // YUV frame: always save JPEG preview for inspection
            char jpegFilename[128];
            snprintf(jpegFilename, sizeof(jpegFilename), "/frame_%02zu_%dx%d.jpg", i, f.width, f.height);
            std::string jpegFilePath = path + jpegFilename;
            saveStridedYuvAsJpeg(
                f.yData.data(), f.yRowStride,
                f.uData.data(), f.uvRowStride,
                f.vData.data(), f.uvPixelStride,
                f.width, f.height,
                jpegFilePath);

            // Raw YUV binary — only written when debug_raw_dumps flag is active.
            // Note: saveRawBurst is currently called with useRaw determining RAW Bayer
            // vs YUV; raw dump behaviour is controlled by the pipeline ctx in runNativeEngine.
            // Keep false here unless saveRawBurst is extended with a debugRawDumps param.
            bool rawDumps = g_debug_raw_dumps;
            if (rawDumps) {
                char filename[128];
                snprintf(filename, sizeof(filename), "/frame_%02zu_%dx%d.yuv", i, f.width, f.height);
                std::string filePath = path + filename;

                std::ofstream out(filePath, std::ios::binary);
                if (out) {
                    // Write Y (planar, stride-free)
                    for (int r = 0; r < f.height; ++r) {
                        out.write(reinterpret_cast<const char*>(f.yData.data() + r * f.yRowStride), f.width);
                    }
                    // Write U
                    int uvW = f.width / 2;
                    int uvH = f.height / 2;
                    for (int r = 0; r < uvH; ++r) {
                        const uint8_t* row = f.uData.data() + r * f.uvRowStride;
                        for (int c = 0; c < uvW; ++c) {
                            uint8_t uVal = row[c * f.uvPixelStride];
                            out.write(reinterpret_cast<const char*>(&uVal), 1);
                        }
                    }
                    // Write V
                    for (int r = 0; r < uvH; ++r) {
                        const uint8_t* row = f.vData.data() + r * f.uvRowStride;
                        for (int c = 0; c < uvW; ++c) {
                            uint8_t vVal = row[c * f.uvPixelStride];
                            out.write(reinterpret_cast<const char*>(&vVal), 1);
                        }
                    }
                    out.close();
                }
            }
        }
    }
    LOGI("saveRawBurst: saved %zu frames to %s (useRaw = %d)", burst->frames.size(), path.c_str(), useRaw);
    return JNI_TRUE;
}

static float computeSharpness(const YuvFrame& f) {
    int step = 8;
    double gradSum = 0;
    int count = 0;
    for (int r = 0; r < f.height - 1; r += step) {
        const uint8_t* row = f.yPlane + r * f.yRowStride;
        const uint8_t* nextRow = row + f.yRowStride;
        for (int c = 0; c < f.width - 1; c += step) {
            float diffX = std::abs(static_cast<float>(row[c]) - static_cast<float>(row[c + 1]));
            float diffY = std::abs(static_cast<float>(row[c]) - static_cast<float>(nextRow[c]));
            gradSum += diffX + diffY;
            count++;
        }
    }
    return count > 0 ? static_cast<float>(gradSum / count) : 0.f;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_renskylab_camera_NativeEngine_processCopiedBurst(
    JNIEnv* env, jobject /* thiz */,
    jlong handle,
    jint jpegQuality,
    jbooleanArray stageFlags,
    jboolean nightMode,
    jint iso,
    jintArray frameIsos,
    jfloatArray configParams,
    jstring debugDirStr,
    jobject listener)
{
    auto* burst = reinterpret_cast<CopiedBurst*>(handle);
    if (!burst || burst->frames.empty()) {
        LOGE("processCopiedBurst: invalid handle or empty burst");
        return nullptr;
    }

    jboolean* flags = env->GetBooleanArrayElements(stageFlags, nullptr);

    FrameContext ctx;
    size_t count = burst->frames.size();
    ctx.inputFrames.resize(count);

    std::vector<int> cFrameIsos(count, iso);
    if (frameIsos) {
        jint* isos = env->GetIntArrayElements(frameIsos, nullptr);
        jsize len = env->GetArrayLength(frameIsos);
        for (int i = 0; i < std::min(static_cast<int>(count), static_cast<int>(len)); ++i) {
            cFrameIsos[i] = isos[i];
        }
        env->ReleaseIntArrayElements(frameIsos, isos, JNI_ABORT);
    }
    ctx.metadata["frame_isos"] = cFrameIsos;

    for (size_t i = 0; i < count; ++i) {
        const CopiedFrame& f = burst->frames[i];
        ctx.inputFrames[i] = {
            .yPlane        = f.yData.data(),
            .uPlane        = f.uData.data(),
            .vPlane        = f.vData.data(),
            .yRowStride    = f.yRowStride,
            .uvRowStride   = f.uvRowStride,
            .uvPixelStride = f.uvPixelStride,
            .width         = f.width,
            .height        = f.height,
        };
    }

    // Stage flags: [align, fuse, debayer, tonemap, encode]
    bool f_align   = count > 0 && flags[0];
    bool f_fuse    = count > 0 && flags[1];
    bool f_debayer = count > 0 && flags[2];
    bool f_tonemap = count > 0 && flags[3];
    bool f_encode  = count > 0 && flags[4];

    ctx.metadata["jpeg_quality"] = static_cast<int>(jpegQuality);
    ctx.metadata["night_mode"]   = static_cast<bool>(nightMode);
    ctx.metadata["iso"]          = static_cast<int>(iso);

    jfloat* params = env->GetFloatArrayElements(configParams, nullptr);
    if (params) {
        ctx.metadata["alignment_regularization"] = static_cast<float>(params[0]);
        ctx.metadata["fusion_noise_multiplier"]   = static_cast<float>(params[1]);
        ctx.metadata["chroma_denoise_enabled"]   = static_cast<bool>(params[2] > 0.5f);
        ctx.metadata["awb_softness_normal"]       = static_cast<float>(params[3]);
        ctx.metadata["awb_softness_night"]        = static_cast<float>(params[4]);
        ctx.metadata["detail_alpha"]             = static_cast<float>(params[5]);
        ctx.metadata["saturation_boost"]         = static_cast<float>(params[6]);
        ctx.metadata["black_point_clamp"]         = static_cast<float>(params[7]);
        ctx.metadata["spatial_denoise_strength"] = static_cast<int>(params[8]);
        // ISO override: if > 0, use instead of the actual capture ISO
        int isoOverride = static_cast<int>(params[9]);
        if (isoOverride > 0) {
            ctx.metadata["iso"] = isoOverride;
            LOGI("ISO override applied: %d", isoOverride);
        }
        ctx.metadata["use_raw_capture"] = static_cast<bool>(params[10] > 0.5f);
        // debug_raw_dumps: when true, each stage also writes raw binary blobs (YUV/PPM)
        // alongside JPEGs. Off by default (~94 MB extra I/O adds ~25 s).
        if (env->GetArrayLength(configParams) > 11) {
            ctx.metadata["debug_raw_dumps"] = static_cast<bool>(params[11] > 0.5f);
        } else {
            ctx.metadata["debug_raw_dumps"] = false;
        }
        // debug_images_enabled: master switch (handled in Kotlin by passing "" debug dir when off)
        // Stored here in case native code needs to check it directly in future.
        if (env->GetArrayLength(configParams) > 12) {
            ctx.metadata["debug_images_enabled"] = static_cast<bool>(params[12] > 0.5f);
        } else {
            ctx.metadata["debug_images_enabled"] = true;
        }
        env->ReleaseFloatArrayElements(configParams, params, JNI_ABORT);
    }
    
    const char* debugDir = env->GetStringUTFChars(debugDirStr, nullptr);
    ctx.metadata["debug_dir"] = std::string(debugDir);
    env->ReleaseStringUTFChars(debugDirStr, debugDir);
    
    env->ReleaseBooleanArrayElements(stageFlags, flags, JNI_ABORT);

    // ── Reference Frame Selection (Sharpest frame as base canvas) ──────────────
    if (count > 1) {
        size_t bestIdx = 0;
        float maxSharpness = -1.f;
        for (size_t i = 0; i < count; ++i) {
            float s = computeSharpness(ctx.inputFrames[i]);
            if (s > maxSharpness) {
                maxSharpness = s;
                bestIdx = i;
            }
        }
        LOGI("Reference Frame Selection: Selected frame %zu as sharpest (score: %.3f)", bestIdx, maxSharpness);
        if (bestIdx > 0) {
            std::swap(ctx.inputFrames[0], ctx.inputFrames[bestIdx]);
        }
    }

    // ── Run pipeline ──────────────────────────────────────────────────────────
    Pipeline pipeline = buildPipeline(f_align, f_fuse, f_debayer, f_tonemap, f_encode);

    std::function<void(const std::string&, int)> progressCb = nullptr;
    if (listener != nullptr) {
        jclass listenerClass = env->GetObjectClass(listener);
        jmethodID onProgressMethod = env->GetMethodID(listenerClass, "onProgress", "(Ljava/lang/String;I)V");
        if (onProgressMethod != nullptr) {
            progressCb = [env, listener, onProgressMethod](const std::string& step, int percent) {
                jstring jStep = env->NewStringUTF(step.c_str());
                env->CallVoidMethod(listener, onProgressMethod, jStep, percent);
                env->DeleteLocalRef(jStep);
            };
        }
    }

    bool ok = pipeline.run(ctx, progressCb);
    if (!ok || ctx.jpegOutput.empty()) {
        LOGE("processCopiedBurst: pipeline failed");
        return nullptr;
    }

    jbyteArray result = env->NewByteArray(static_cast<jsize>(ctx.jpegOutput.size()));
    if (!result) return nullptr;
    env->SetByteArrayRegion(
        result, 0,
        static_cast<jsize>(ctx.jpegOutput.size()),
        reinterpret_cast<const jbyte*>(ctx.jpegOutput.data()));

    return result;
}
