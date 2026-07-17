#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <any>
#include <fstream>
#include <cstdio>
#include <thread>
#include <atomic>

#include "pipeline.h"
#include "frame_context.h"
#include "debug_utils.h"
#include "stages/defect_pixel_stage.h"
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
    p.addStage(std::make_unique<DefectPixelStage>());
    p.addStage(std::make_unique<AlignStage>());
    p.addStage(std::make_unique<FusionStage>());
    p.addStage(std::make_unique<DebayerStage>());
    // ── Future stages inserted here, e.g.:
    // p.addStage(std::make_unique<DenoiseStage>());
    p.addStage(std::make_unique<ToneMapStage>());
    p.addStage(std::make_unique<EncodeStage>());

    p.setEnabled("defect_pixel", true);
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
        jlong expectedYLength = static_cast<jlong>(yStrides[i]) * height;
        if (yLength <= 0) yLength = expectedYLength;
        jlong copyY = std::min(yLength, expectedYLength);
        if (copyY < 0) copyY = 0;

        jlong uLength = env->GetDirectBufferCapacity(uBuf);
        jlong expectedULength = static_cast<jlong>(uvRStride[i]) * (height / 2);
        if (uLength <= 0) uLength = expectedULength;
        jlong copyU = std::min(uLength, expectedULength);
        if (copyU < 0) copyU = 0;

        jlong vLength = env->GetDirectBufferCapacity(vBuf);
        jlong expectedVLength = static_cast<jlong>(uvRStride[i]) * (height / 2);
        if (vLength <= 0) vLength = expectedVLength;
        jlong copyV = std::min(vLength, expectedVLength);
        if (copyV < 0) copyV = 0;

        CopiedFrame& f = burst->frames[i];
        f.width = width;
        f.height = height;
        f.yRowStride = yStrides[i];
        f.uvRowStride = uvRStride[i];
        f.uvPixelStride = uvPStride[i];

        // Allocate and copy data safely, zero-padding if capacity is smaller than strides
        f.yData.assign(yPtr, yPtr + copyY);
        if (f.yData.size() < static_cast<size_t>(expectedYLength)) {
            f.yData.resize(expectedYLength, 0);
        }

        f.uData.assign(uPtr, uPtr + copyU);
        if (f.uData.size() < static_cast<size_t>(expectedULength)) {
            f.uData.resize(expectedULength, 0);
        }

        f.vData.assign(vPtr, vPtr + copyV);
        if (f.vData.size() < static_cast<size_t>(expectedVLength)) {
            f.vData.resize(expectedVLength, 0);
        }

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
    jboolean useRaw,
    jboolean debugRawDumps)
{
    auto* burst = reinterpret_cast<CopiedBurst*>(handle);
    if (!burst || burst->frames.empty()) {
        LOGE("saveRawBurst: invalid handle or empty burst");
        return JNI_FALSE;
    }

    const char* dirPath = env->GetStringUTFChars(dirPathStr, nullptr);
    std::string path(dirPath);
    env->ReleaseStringUTFChars(dirPathStr, dirPath);

    std::atomic<bool> success{true};
    std::vector<std::thread> threads;
    threads.reserve(burst->frames.size());

    for (size_t i = 0; i < burst->frames.size(); ++i) {
        threads.emplace_back([&path, useRaw, debugRawDumps, &success, burst, i]() {
            const CopiedFrame& f = burst->frames[i];
            
            if (useRaw) {
                char filename[128];
                snprintf(filename, sizeof(filename), "/frame_%02zu_%dx%d.raw", i, f.width, f.height);
                std::string filePath = path + filename;
                
                std::ofstream out(filePath, std::ios::binary);
                if (!out) {
                    LOGE("saveRawBurst: failed to open file %s for writing", filePath.c_str());
                    success = false;
                    return;
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

                uint8_t lut[65536];
                float scale = 255.f / std::max(1.f, whiteLevel - blackLevel);
                for (int val = 0; val < 65536; ++val) {
                    float cleanVal = (static_cast<float>(val) - blackLevel) * scale;
                    lut[val] = static_cast<uint8_t>(std::clamp(cleanVal, 0.f, 255.f));
                }

                int strideElements = f.yRowStride / 2;
                for (int r = 0; r < f.height; ++r) {
                    const uint16_t* rowStart = rawData + r * strideElements;
                    uint8_t* destStart = previewY.data() + r * f.width;
                    for (int c = 0; c < f.width; ++c) {
                        destStart[c] = lut[rowStart[c]];
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
                if (debugRawDumps) {
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
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    if (!success) {
        return JNI_FALSE;
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
    jlongArray frameExposures,
    jfloatArray frameNoiseProfiles,
    jfloatArray configParams,
    jfloatArray lscDataArr,
    jint lscMapWidth,
    jint lscMapHeight,
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

    std::vector<double> cFrameExposures(count, 33333333.0);
    if (frameExposures) {
        jlong* exposures = env->GetLongArrayElements(frameExposures, nullptr);
        jsize len = env->GetArrayLength(frameExposures);
        for (int i = 0; i < std::min(static_cast<int>(count), static_cast<int>(len)); ++i) {
            cFrameExposures[i] = static_cast<double>(exposures[i]);
        }
        env->ReleaseLongArrayElements(frameExposures, exposures, JNI_ABORT);
    }
    ctx.metadata["frame_exposures"] = cFrameExposures;

    std::vector<float> noiseProfiles;
    if (frameNoiseProfiles) {
        jfloat* profiles = env->GetFloatArrayElements(frameNoiseProfiles, nullptr);
        jsize len = env->GetArrayLength(frameNoiseProfiles);
        noiseProfiles.resize(len);
        for (int i = 0; i < len; ++i) {
            noiseProfiles[i] = profiles[i];
        }
        env->ReleaseFloatArrayElements(frameNoiseProfiles, profiles, JNI_ABORT);
    }
    ctx.metadata["noise_profiles"] = noiseProfiles;

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
        
        if (env->GetArrayLength(configParams) > 19) {
            ctx.metadata["denoise_pass1_enabled"] = static_cast<bool>(params[15] > 0.5f);
            ctx.metadata["denoise_pass1_strength"] = static_cast<float>(params[16]);
            ctx.metadata["denoise_pass2_enabled"] = static_cast<bool>(params[17] > 0.5f);
            ctx.metadata["denoise_pass2_strength"] = static_cast<float>(params[18]);
            ctx.metadata["tonemap_exposure_boost"] = static_cast<float>(params[19]);
        } else {
            ctx.metadata["denoise_pass1_enabled"] = true;
            ctx.metadata["denoise_pass1_strength"] = 1.0f;
            ctx.metadata["denoise_pass2_enabled"] = true;
            ctx.metadata["denoise_pass2_strength"] = 0.5f;
            ctx.metadata["tonemap_exposure_boost"] = 1.30f;
        }
        
        float digitalGain = 1.0f;
        if (env->GetArrayLength(configParams) > 20) {
            digitalGain = params[20];
        }
        ctx.metadata["digital_gain"] = digitalGain;
        LOGI("Extracted digital gain from JNI: %.3fx", digitalGain);

        float appliedEv = 0.0f;
        if (env->GetArrayLength(configParams) > 21) {
            appliedEv = params[21];
        }
        ctx.metadata["applied_ev_compensation"] = appliedEv;
        LOGI("Extracted applied EV compensation from JNI: %.3f EV", appliedEv);

        // Dynamic AWB Gains (Indices 22-24)
        std::vector<float> awbGains(3);
        if (env->GetArrayLength(configParams) > 24) {
            awbGains[0] = params[22];
            awbGains[1] = params[23];
            awbGains[2] = params[24];
        } else {
            awbGains = {2.1f, 1.0f, 1.9f};
        }
        ctx.metadata["awb_gains"] = awbGains;
        LOGI("Extracted dynamic AWB gains from JNI: R=%.3f, G=%.3f, B=%.3f", awbGains[0], awbGains[1], awbGains[2]);

        // Dynamic Color Correction Matrix (Indices 25-33)
        std::vector<float> ccm(9);
        if (env->GetArrayLength(configParams) > 33) {
            for (int i = 0; i < 9; ++i) {
                ccm[i] = params[25 + i];
            }
            // Transform Sensor-to-sRGB CCM into Sensor-to-Display P3 CCM
            // M_sRGB_to_P3 matrix coefficients (Row-major):
            // [ 0.8225, 0.1775, 0.0000 ]
            // [ 0.0332, 0.9668, 0.0000 ]
            // [ 0.0171, 0.0724, 0.9105 ]
            std::vector<float> sensorToXYZ = ccm;
            const float mP3[9] = {
                0.8225f, 0.1775f, 0.0000f,
                0.0332f, 0.9668f, 0.0000f,
                0.0171f, 0.0724f, 0.9105f
            };
            LOGI("Original dynamic CCM from JNI (Sensor-to-sRGB): [%.3f, %.3f, %.3f; %.3f, %.3f, %.3f; %.3f, %.3f, %.3f]",
                 sensorToXYZ[0], sensorToXYZ[1], sensorToXYZ[2],
                 sensorToXYZ[3], sensorToXYZ[4], sensorToXYZ[5],
                 sensorToXYZ[6], sensorToXYZ[7], sensorToXYZ[8]);
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    float sum = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        sum += mP3[row * 3 + k] * sensorToXYZ[k * 3 + col];
                    }
                    ccm[row * 3 + col] = sum;
                }
            }
        } else {
            ccm = {
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 1.0f
            };
        }
        ctx.metadata["color_correction_matrix"] = ccm;
        LOGI("Extracted dynamic Color Correction Matrix (CCM) transformed to DCI-P3: [%.3f, %.3f, %.3f; %.3f, %.3f, %.3f; %.3f, %.3f, %.3f]",
             ccm[0], ccm[1], ccm[2], ccm[3], ccm[4], ccm[5], ccm[6], ccm[7], ccm[8]);

        int cfaPattern = 3; // default BGGR
        if (env->GetArrayLength(configParams) > 34) {
            cfaPattern = static_cast<int>(params[34]);
        }
        ctx.metadata["cfa_pattern"] = cfaPattern;
        LOGI("Extracted CFA pattern arrangement from JNI: %d", cfaPattern);

        float blackLevel = 1024.f;
        if (env->GetArrayLength(configParams) > 35) {
            blackLevel = params[35];
        }
        ctx.metadata["black_level"] = blackLevel;
        LOGI("Extracted black level from JNI: %.1f", blackLevel);

        float whiteLevel = 4095.f;
        if (env->GetArrayLength(configParams) > 36) {
            whiteLevel = params[36];
        }
        ctx.metadata["white_level"] = whiteLevel;
        LOGI("Extracted white level from JNI: %.1f", whiteLevel);

        env->ReleaseFloatArrayElements(configParams, params, JNI_ABORT);
    }

    // ── Lens Shading Correction map ──────────────────────────────────────────
    if (lscDataArr != nullptr) {
        jsize lscLen = env->GetArrayLength(lscDataArr);
        jfloat* lscPtr = env->GetFloatArrayElements(lscDataArr, nullptr);
        std::vector<float> lscVec(lscPtr, lscPtr + lscLen);
        env->ReleaseFloatArrayElements(lscDataArr, lscPtr, JNI_ABORT);
        ctx.metadata["lsc_data"]        = lscVec;
        ctx.metadata["lsc_map_width"]   = static_cast<int>(lscMapWidth);
        ctx.metadata["lsc_map_height"]  = static_cast<int>(lscMapHeight);
        LOGI("processCopiedBurst: LSC map received — %d×%d (%zu floats)",
             static_cast<int>(lscMapWidth), static_cast<int>(lscMapHeight), lscVec.size());
    } else {
        // Unity map — no correction
        ctx.metadata["lsc_data"]        = std::vector<float>{1.f, 1.f, 1.f, 1.f};
        ctx.metadata["lsc_map_width"]   = 1;
        ctx.metadata["lsc_map_height"]  = 1;
        LOGI("processCopiedBurst: no LSC map provided — unity gain applied");
    }
    
    const char* debugDir = env->GetStringUTFChars(debugDirStr, nullptr);
    ctx.metadata["debug_dir"] = std::string(debugDir);
    env->ReleaseStringUTFChars(debugDirStr, debugDir);
    
    env->ReleaseBooleanArrayElements(stageFlags, flags, JNI_ABORT);

    // ── Reference Frame Selection (Sharpest frame as base canvas) ──────────────
    // To prevent handshake blur, we find the frame with the highest gradient variance.
    // In HDR+ Enhanced, we MUST only select from normal-exposure frames (even indices)
    // because odd frames are underexposed for highlights and cannot serve as the base canvas.
    if (count > 1) {
        size_t bestIdx = 0;
        float maxSharpness = -1.f;
        
        // Dynamically determine black level of the first frame to threshold noise
        float blackLevel = 64.f;
        {
            const YuvFrame& f0 = ctx.inputFrames[0];
            const uint16_t* rawData = reinterpret_cast<const uint16_t*>(f0.yPlane);
            uint16_t minVal = 65535, maxVal = 0;
            int testPixels = std::min(f0.width * f0.height, 10000);
            for (int p = 0; p < testPixels; ++p) {
                uint16_t v = rawData[p];
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
            }
            if (maxVal > 1023 && maxVal <= 4095) {
                blackLevel = 1024.f;
                if (minVal < 300) blackLevel = 256.f;
                if (minVal < 100) blackLevel = 64.f;
            } else if (maxVal > 4095) {
                blackLevel = 1024.f;
                if (minVal < 500) blackLevel = 256.f;
            }
        }

        // Noise threshold: ignore gradients smaller than 4% of sensor dynamic range
        float noiseFloor = (blackLevel > 500.f) ? 120.f : 30.f;

        for (size_t i = 0; i < count; i += 2) { // Increment by 2 to strictly check normal-exposure frames
            const YuvFrame& f = ctx.inputFrames[i];
            const uint16_t* rawBase = reinterpret_cast<const uint16_t*>(f.yPlane);
            int strideElements = f.yRowStride / 2;
            
            // Perform a fast sharpness evaluation on a central 512x512 region
            int cropSize = 512;
            int startX = std::max(0, (f.width - cropSize) / 2);
            int startY = std::max(0, (f.height - cropSize) / 2);
            
            double gradSum = 0;
            int pixelCount = 0;
            
            for (int r = startY; r < startY + cropSize - 1; r += 4) { // Subsampled for speed
                const uint16_t* row = rawBase + r * strideElements;
                const uint16_t* nextRow = rawBase + (r + 1) * strideElements;
                for (int c = startX; c < startX + cropSize - 1; c += 4) {
                    float val = std::max(0.f, static_cast<float>(row[c]) - blackLevel);
                    float right = std::max(0.f, static_cast<float>(row[c + 1]) - blackLevel);
                    float down = std::max(0.f, static_cast<float>(nextRow[c]) - blackLevel);
                    
                    float diffX = std::abs(val - right);
                    float diffY = std::abs(val - down);
                    
                    // Filter out noise fluctuations (noise floor thresholding)
                    if (diffX > noiseFloor) gradSum += diffX;
                    if (diffY > noiseFloor) gradSum += diffY;
                    pixelCount++;
                }
            }
            
            float s = pixelCount > 0 ? static_cast<float>(gradSum / pixelCount) : 0.f;
            if (s > maxSharpness) {
                maxSharpness = s;
                bestIdx = i;
            }
        }
        
        LOGI("Reference Frame Selection: Selected frame %zu as sharpest normal frame (score: %.3f)", bestIdx, maxSharpness);
        ctx.metadata["selected_base_frame_index"] = static_cast<int>(bestIdx);
        if (bestIdx > 0) {
            std::swap(ctx.inputFrames[0], ctx.inputFrames[bestIdx]);
            // Also swap corresponding metadata so exposures, ISOs, and noise profiles match the new frame 0
            if (ctx.metadata.count("frame_isos")) {
                try {
                    auto& isos = std::any_cast<std::vector<int>&>(ctx.metadata["frame_isos"]);
                    std::swap(isos[0], isos[bestIdx]);
                } catch(...) {}
            }
            if (ctx.metadata.count("frame_exposures")) {
                try {
                    auto& exposures = std::any_cast<std::vector<double>&>(ctx.metadata["frame_exposures"]);
                    std::swap(exposures[0], exposures[bestIdx]);
                } catch(...) {}
            }
        }
    } else {
        ctx.metadata["selected_base_frame_index"] = 0;
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
