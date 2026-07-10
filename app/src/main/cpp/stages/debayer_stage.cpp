#include "debayer_stage.h"
#include "../debug_utils.h"
#include "../gl_helpers.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <any>
#include <future>
#include <vector>

// Highway SIMD — vectorised YUV→RGB inner loop
#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

namespace {

// BT.601 full-range coefficients scaled to 16-bit fixed point (×128)
// R = Y + 1.402*(V-128)       → 1.402 ≈ 179/128
// G = Y - 0.344*(U-128) - 0.714*(V-128)
// B = Y + 1.772*(U-128)       → 1.772 ≈ 227/128
static constexpr int CR_R  =  179; //  1.402 * 128
static constexpr int CU_G  =   44; //  0.344 * 128
static constexpr int CV_G  =   91; //  0.714 * 128
static constexpr int CB_B  =  227; //  1.772 * 128

/// Convert a row of I420 to RGB24 using scalar BT.601.
/// fusedU/fusedV are packed planes (pixel stride = 1).
static void convertRowScalar(
    const uint8_t* __restrict__ yRow,
    const uint8_t* __restrict__ uRow,   // half-width
    const uint8_t* __restrict__ vRow,
    uint8_t* __restrict__ rgbRow,
    int width)
{
    for (int x = 0; x < width; ++x) {
        int Y = static_cast<int>(yRow[x]);
        int U = static_cast<int>(uRow[x >> 1]) - 128;
        int V = static_cast<int>(vRow[x >> 1]) - 128;

        int R = Y + (CR_R * V + 64) / 128;
        int G = Y - (CU_G * U + CV_G * V + 64) / 128;
        int B = Y + (CB_B * U + 64) / 128;

        rgbRow[x*3 + 0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
        rgbRow[x*3 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
        rgbRow[x*3 + 2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// DebayerStage::process
// ─────────────────────────────────────────────────────────────────────────────
bool DebayerStage::process(FrameContext& ctx) {
    bool useRaw = false;
    if (ctx.metadata.count("use_raw_capture")) {
        try {
            useRaw = std::any_cast<bool>(ctx.metadata.at("use_raw_capture"));
        } catch (...) {}
    }

    int w = ctx.fusedWidth;
    int h = ctx.fusedHeight;
    ctx.colorImage.resize(w, h);

    if (useRaw) {
        if (!ctx.metadata.count("fused_raw")) {
            LOGE("DebayerStage: no fused RAW frame in metadata");
            return false;
        }
        const auto& fusedRaw = std::any_cast<const std::vector<uint16_t>&>(ctx.metadata.at("fused_raw"));
        
        // Dynamically detect bit depth and black level from fusedRaw buffer
        float blackLevel = 1024.f;
        float whiteLevel = 4095.f;
        
        uint16_t minVal = 65535;
        uint16_t maxVal = 0;
        for (uint16_t v : fusedRaw) {
            if (v < minVal) minVal = v;
            if (v > maxVal) maxVal = v;
        }
        LOGI("DebayerStage: RAW range detection — min = %d, max = %d", minVal, maxVal);

        if (maxVal <= 1023) {
            // 10-bit RAW (e.g. standard 10-bit Bayer)
            blackLevel = 64.f;
            whiteLevel = 1023.f;
        } else if (maxVal <= 4095) {
            // 12-bit RAW
            blackLevel = 1024.f;
            if (minVal < 300) blackLevel = 256.f;
            if (minVal < 100) blackLevel = 64.f;
            whiteLevel = 4095.f;
        } else {
            // 14-bit or 16-bit RAW
            blackLevel = 1024.f;
            if (minVal < 500) blackLevel = 256.f;
            whiteLevel = static_cast<float>(maxVal);
        }

        float digitalGain = 1.0f;
        if (ctx.metadata.count("digital_gain")) {
            try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
        }
        LOGI("DebayerStage: applying digital gain = %.3fx", digitalGain);

        // Adjust scaling factor based on calculated dynamic range and digital exposure matching gain
        float scale = (255.f / std::max(1.f, whiteLevel - blackLevel)) * digitalGain;

        // Sony sensor color channel gain defaults to neutralize green tint
        float rGain = 2.1f;
        float bGain = 1.9f;
        float gGain = 1.0f;

        // ── GPU HEADLESS COMPUTE SHADER SABRE MULTI-FRAME DEMOSAICING ────────────────────────────
        bool success = false;
        std::string errorLog;
        
        int numFrames = ctx.inputFrames.size();

        if (numFrames == 1) {
            LOGI("DebayerStage: single frame detected, bypassing Sabre GPU compute. Using CPU Bilinear demosaicing fallback.");
        } else {
            EglHeadlessSetup egl;
            if (egl.init(errorLog)) {
            const char* COMPUTE_SABRE_SRC = R"glsl(
                #version 310 es
                layout(local_size_x = 16, local_size_y = 16) in;

                precision highp float;
                precision highp usampler2DArray;
                precision highp sampler2DArray;

                uniform usampler2DArray u_raw_frames;
                uniform sampler2DArray u_motion_fields;

                uniform int u_num_frames;
                uniform int u_width;
                uniform int u_height;
                uniform int u_blocks_wide;
                uniform int u_blocks_tall;
                uniform float u_block_size;

                uniform float u_black_level;
                uniform float u_scale;
                uniform float u_r_gain;
                uniform float u_g_gain;
                uniform float u_b_gain;

                layout(std430, binding = 0) writeonly buffer OutputBuffer {
                    uint outRGB[];
                };

                float getRawCorrected(ivec2 p, int frameIdx) {
                    int cx = clamp(p.x, 0, u_width - 1);
                    int cy = clamp(p.y, 0, u_height - 1);
                    uint val = texelFetch(u_raw_frames, ivec3(cx, cy, frameIdx), 0).r;
                    float cleanVal = max(0.0, (float(val) - u_black_level) * u_scale);

                    bool isEvenRow = (cy % 2 == 0);
                    bool isEvenCol = (cx % 2 == 0);
                    if (isEvenRow && isEvenCol) {
                        return cleanVal * u_b_gain; // Blue (BGGR)
                    } else if (!isEvenRow && !isEvenCol) {
                        return cleanVal * u_r_gain; // Red
                    } else {
                        return cleanVal * u_g_gain; // Green
                    }
                }

                int getPixelColor(ivec2 p) {
                    bool isEvenRow = (p.y % 2 == 0);
                    bool isEvenCol = (p.x % 2 == 0);
                    if (isEvenRow && isEvenCol) return 2; // Blue
                    if (!isEvenRow && !isEvenCol) return 0; // Red
                    return 1; // Green
                }

                void main() {
                    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
                    if (pos.x >= u_width || pos.y >= u_height) return;

                    // Edge-adaptive kernel: compute horizontal and vertical green gradients on reference frame
                    float gLeft = getRawCorrected(pos + ivec2(-1, 0), 0);
                    float gRight = getRawCorrected(pos + ivec2(1, 0), 0);
                    float gUp = getRawCorrected(pos + ivec2(0, -1), 0);
                    float gDown = getRawCorrected(pos + ivec2(0, 1), 0);
                    float edgeStrength = abs(gLeft - gRight) + abs(gUp - gDown);
                    // Ramps from 3.5 (flat regions, strong denoising) to 8.0 (sharp edges, detail preservation)
                    float kExponent = mix(3.5, 8.0, clamp(edgeStrength / 15.0, 0.0, 1.0));

                    float sumR = 0.0, weightR = 0.0;
                    float sumG = 0.0, weightG = 0.0;
                    float sumB = 0.0, weightB = 0.0;

                    // 1. Reference frame (Frame 0, pre-fused) local neighborhood contribution
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            ivec2 samplePos = pos + ivec2(dx, dy);
                            if (samplePos.x >= 0 && samplePos.x < u_width && samplePos.y >= 0 && samplePos.y < u_height) {
                                float val = getRawCorrected(samplePos, 0);
                                int color = getPixelColor(samplePos);
                                
                                float dist2 = float(dx * dx + dy * dy);
                                float spatialW = exp(-dist2 * kExponent);

                                if (color == 0) {
                                    sumR += val * spatialW;
                                    weightR += spatialW;
                                } else if (color == 1) {
                                    sumG += val * spatialW;
                                    weightG += spatialW;
                                } else {
                                    sumB += val * spatialW;
                                    weightB += spatialW;
                                }
                            }
                        }
                    }

                    // 2. Subpixel multi-frame accumulation (with noise weight filtering)
                    float noiseTolerance = 12.f / u_scale; // scaled to RAW pixel domain (12.f out of 255.f)
                    for (int f = 1; f < u_num_frames; ++f) {
                        float bx = (float(pos.x) - u_block_size * 0.5) / u_block_size;
                        float by = (float(pos.y) - u_block_size * 0.5) / u_block_size;
                        vec2 mv_uv = vec2((bx + 0.5) / float(u_blocks_wide), (by + 0.5) / float(u_blocks_tall));

                        vec2 mv = texture(u_motion_fields, vec3(mv_uv, float(f - 1))).rg;
                        vec2 targetPos = vec2(pos) + mv;
                        ivec2 centerIdx = ivec2(round(targetPos.x), round(targetPos.y));

                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                ivec2 samplePos = centerIdx + ivec2(dx, dy);
                                if (samplePos.x >= 0 && samplePos.x < u_width && samplePos.y >= 0 && samplePos.y < u_height) {
                                    float val = getRawCorrected(samplePos, f);
                                    
                                    // Compare against reference pixel to reject noise and motion mismatches
                                    ivec2 refSamplePos = pos + ivec2(dx, dy);
                                    float refVal = getRawCorrected(clamp(refSamplePos, ivec2(0), ivec2(u_width - 1, u_height - 1)), 0);
                                    float diff = abs(val - refVal);
                                    
                                    // Smoothly reject unaligned/noisy pixels
                                    float weightFactor = exp(-diff * diff / (2.f * noiseTolerance * noiseTolerance));
                                    if (weightFactor < 0.2) continue; // Tightened threshold from 0.1 to 0.2

                                    int color = getPixelColor(samplePos);

                                    vec2 delta = vec2(samplePos) - targetPos;
                                    float dist2 = dot(delta, delta);
                                    float spatialW = exp(-dist2 * kExponent) * weightFactor;

                                    if (color == 0) {
                                        sumR += val * spatialW;
                                        weightR += spatialW;
                                    } else if (color == 1) {
                                        sumG += val * spatialW;
                                        weightG += spatialW;
                                    } else {
                                        sumB += val * spatialW;
                                        weightB += spatialW;
                                    }
                                }
                            }
                        }
                    }

                    float finalR = (weightR > 0.01) ? (sumR / weightR) : 0.0;
                    float finalG = (weightG > 0.01) ? (sumG / weightG) : 0.0;
                    float finalB = (weightB > 0.01) ? (sumB / weightB) : 0.0;

                    // Fallback to high-quality local demosaicing if any channel lacks enough samples
                    bool needR = (weightR <= 0.2);
                    bool needG = (weightG <= 0.2);
                    bool needB = (weightB <= 0.2);

                    if (needR || needG || needB) {
                        int r = pos.y;
                        int c = pos.x;
                        bool isEvenRow = (r % 2 == 0);
                        bool isEvenCol = (c % 2 == 0);

                        float fallbackR = 0.0;
                        float fallbackG = 0.0;
                        float fallbackB = 0.0;

                        if (isEvenRow && isEvenCol) {
                            fallbackB = getRawCorrected(pos, 0);
                            fallbackG = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0) + getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.25 + 
                                        (4.0 * fallbackB - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0) - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
                            fallbackR = (getRawCorrected(ivec2(c-1, r-1), 0) + getRawCorrected(ivec2(c+1, r-1), 0) + getRawCorrected(ivec2(c-1, r+1), 0) + getRawCorrected(ivec2(c+1, r+1), 0)) * 0.25 + 
                                        (6.0 * fallbackB - 1.5 * (getRawCorrected(ivec2(c-2, r), 0) + getRawCorrected(ivec2(c+2, r), 0) + getRawCorrected(ivec2(c, r-2), 0) + getRawCorrected(ivec2(c, r+2), 0))) * 0.0625;
                        } else if (!isEvenRow && !isEvenCol) {
                            fallbackR = getRawCorrected(pos, 0);
                            fallbackG = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0) + getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.25 + 
                                        (4.0 * fallbackR - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0) - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
                            fallbackB = (getRawCorrected(ivec2(c-1, r-1), 0) + getRawCorrected(ivec2(c+1, r-1), 0) + getRawCorrected(ivec2(c-1, r+1), 0) + getRawCorrected(ivec2(c+1, r+1), 0)) * 0.25 + 
                                        (6.0 * fallbackR - 1.5 * (getRawCorrected(ivec2(c-2, r), 0) + getRawCorrected(ivec2(c+2, r), 0) + getRawCorrected(ivec2(c, r-2), 0) + getRawCorrected(ivec2(c, r+2), 0))) * 0.0625;
                        } else if (isEvenRow && !isEvenCol) {
                            fallbackG = getRawCorrected(pos, 0);
                            fallbackB = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0)) * 0.5 + 
                                        (2.0 * fallbackG - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0)) * 0.125;
                            fallbackR = (getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.5 + 
                                        (2.0 * fallbackG - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
                        } else {
                            fallbackG = getRawCorrected(pos, 0);
                            fallbackR = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0)) * 0.5 + 
                                        (2.0 * fallbackG - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0)) * 0.125;
                            fallbackB = (getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.5 + 
                                        (2.0 * fallbackG - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
                        }

                        if (needR) finalR = fallbackR;
                        if (needG) finalG = fallbackG;
                        if (needB) finalB = fallbackB;
                    }

                    uint uR = uint(clamp(finalR, 0.0, 255.0));
                    uint uG = uint(clamp(finalG, 0.0, 255.0));
                    uint uB = uint(clamp(finalB, 0.0, 255.0));

                    uint packedVal = uR | (uG << 8) | (uB << 16) | (255u << 24);
                    outRGB[pos.y * u_width + pos.x] = packedVal;
                }
            )glsl";

            GLuint program = createComputeProgram(COMPUTE_SABRE_SRC, errorLog);
            if (program != 0) {
                glUseProgram(program);

                // 1. Upload all RAW burst frames to a 2D Texture Array (Frame 0 = fusedRaw)
                GLuint rawTextureArray;
                glGenTextures(1, &rawTextureArray);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, rawTextureArray);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R16UI, w, h, numFrames);
                for (int f = 0; f < numFrames; ++f) {
                    const uint16_t* frameRaw = (f == 0) ? fusedRaw.data() : reinterpret_cast<const uint16_t*>(ctx.inputFrames[f].yPlane);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, ctx.inputFrames[f].yRowStride / 2);
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, f, w, h, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, frameRaw);
                }
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                // 2. Upload motion fields
                int blocksWide = ctx.motionFields[0].blocksWide;
                int blocksTall = ctx.motionFields[0].blocksTall;
                int blockSize = ctx.motionFields[0].blockSize;
                int numFields = ctx.motionFields.size();

                std::vector<float> contiguousMVs(2 * blocksWide * blocksTall * numFields);
                for (int f = 0; f < numFields; ++f) {
                    const auto& mf = ctx.motionFields[f];
                    for (int y = 0; y < blocksTall; ++y) {
                        for (int x = 0; x < blocksWide; ++x) {
                            auto mv = mf.vectors[y * blocksWide + x];
                            contiguousMVs[0 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dx;
                            contiguousMVs[1 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dy;
                        }
                    }
                }

                GLuint motionFieldsTex;
                glGenTextures(1, &motionFieldsTex);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D_ARRAY, motionFieldsTex);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RG32F, blocksWide, blocksTall, numFields);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, blocksWide, blocksTall, numFields, GL_RG, GL_FLOAT, contiguousMVs.data());
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                // 3. Set Uniforms
                glUniform1i(glGetUniformLocation(program, "u_raw_frames"), 0);
                glUniform1i(glGetUniformLocation(program, "u_motion_fields"), 1);
                glUniform1i(glGetUniformLocation(program, "u_num_frames"), numFrames);
                glUniform1i(glGetUniformLocation(program, "u_width"), w);
                glUniform1i(glGetUniformLocation(program, "u_height"), h);
                glUniform1i(glGetUniformLocation(program, "u_blocks_wide"), blocksWide);
                glUniform1i(glGetUniformLocation(program, "u_blocks_tall"), blocksTall);
                glUniform1f(glGetUniformLocation(program, "u_block_size"), static_cast<float>(blockSize));

                glUniform1f(glGetUniformLocation(program, "u_black_level"), blackLevel);
                glUniform1f(glGetUniformLocation(program, "u_scale"), scale);
                glUniform1f(glGetUniformLocation(program, "u_r_gain"), rGain);
                glUniform1f(glGetUniformLocation(program, "u_g_gain"), gGain);
                glUniform1f(glGetUniformLocation(program, "u_b_gain"), bGain);

                // Create output SSBO for packed RGB
                GLuint outBuffer;
                glGenBuffers(1, &outBuffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);

                // Dispatch compute shader
                glDispatchCompute(static_cast<GLuint>((w + 15) / 16), static_cast<GLuint>((h + 15) / 16), 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

                // Read back output buffer
                void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), GL_MAP_READ_BIT);
                if (ptr != nullptr) {
                    ctx.colorImage.resize(w, h);
                    uint8_t* dst = ctx.colorImage.data.data();
                    const uint32_t* src = static_cast<const uint32_t*>(ptr);
                    for (int i = 0; i < w * h; ++i) {
                        uint32_t val = src[i];
                        dst[i * 3 + 0] = val & 0xFF;
                        dst[i * 3 + 1] = (val >> 8) & 0xFF;
                        dst[i * 3 + 2] = (val >> 16) & 0xFF;
                    }
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                    success = true;
                } else {
                    errorLog += "GL: failed to map demosaic output buffer\n";
                }

                glDeleteBuffers(1, &outBuffer);
                glDeleteTextures(1, &rawTextureArray);
                glDeleteTextures(1, &motionFieldsTex);
            }
        }
    }

    if (!success) {
            LOGE("GL Demosaicing failed, falling back to CPU multi-threaded Bilinear demosaicing. GL Errors:\n%s", errorLog.c_str());
            // Bilinear Demosaicing (BGGR layout) - Multi-threaded CPU Fallback
            int numThreads = 8;
            int rowsPerThread = h / numThreads;
            std::vector<std::future<void>> futures;
            futures.reserve(numThreads);

            for (int t = 0; t < numThreads; ++t) {
                int rStart = t * rowsPerThread;
                int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

                futures.push_back(std::async(std::launch::async, [&ctx, &fusedRaw, rStart, rEnd, w, h, blackLevel, scale, rGain, gGain, bGain]() {
                    auto getRaw = [&](int r, int cc) -> float {
                        int cr = std::clamp(r, 0, h - 1);
                        int c_clamped = std::clamp(cc, 0, w - 1);
                        float val = static_cast<float>(fusedRaw[cr * w + c_clamped]);
                        float cleanVal = std::max(0.f, (val - blackLevel) * scale);

                        bool isEvenRow = (cr % 2 == 0);
                        bool isEvenCol = (c_clamped % 2 == 0);
                        if (isEvenRow && isEvenCol) {
                            return cleanVal * bGain; // Blue
                        } else if (!isEvenRow && !isEvenCol) {
                            return cleanVal * rGain; // Red
                        } else {
                            return cleanVal * gGain; // Green
                        }
                    };

                    for (int r = rStart; r < rEnd; ++r) {
                        uint8_t* rgbRow = ctx.colorImage.rowPtr(r);
                        for (int c = 0; c < w; ++c) {
                            float R = 0.f, G = 0.f, B = 0.f;
                            bool isEvenRow = (r % 2 == 0);
                            bool isEvenCol = (c % 2 == 0);

                            if (isEvenRow && isEvenCol) {
                                // Blue pixel
                                R = (getRaw(r-1, c-1) + getRaw(r-1, c+1) + getRaw(r+1, c-1) + getRaw(r+1, c+1)) * 0.25f;
                                G = (getRaw(r-1, c) + getRaw(r+1, c) + getRaw(r, c-1) + getRaw(r, c+1)) * 0.25f;
                                B = getRaw(r, c);
                            } else if (!isEvenRow && !isEvenCol) {
                                // Red pixel
                                R = getRaw(r, c);
                                G = (getRaw(r-1, c) + getRaw(r+1, c) + getRaw(r, c-1) + getRaw(r, c+1)) * 0.25f;
                                B = (getRaw(r-1, c-1) + getRaw(r-1, c+1) + getRaw(r+1, c-1) + getRaw(r+1, c+1)) * 0.25f;
                            } else if (isEvenRow && !isEvenCol) {
                                // Green pixel on Blue row
                                R = (getRaw(r-1, c) + getRaw(r+1, c)) * 0.5f;
                                G = getRaw(r, c);
                                B = (getRaw(r, c-1) + getRaw(r, c+1)) * 0.5f;
                            } else {
                                // Green pixel on Red row
                                R = (getRaw(r, c-1) + getRaw(r, c+1)) * 0.5f;
                                G = getRaw(r, c);
                                B = (getRaw(r-1, c) + getRaw(r+1, c)) * 0.5f;
                            }

                            rgbRow[c * 3 + 0] = static_cast<uint8_t>(std::clamp(R, 0.f, 255.f));
                            rgbRow[c * 3 + 1] = static_cast<uint8_t>(std::clamp(G, 0.f, 255.f));
                            rgbRow[c * 3 + 2] = static_cast<uint8_t>(std::clamp(B, 0.f, 255.f));
                        }
                    }
                }));
            }
            for (auto& fut : futures) {
                fut.get();
            }
        }
    } else {
        if (ctx.fusedY.empty()) {
            LOGE("DebayerStage: no fused Y plane (FusionStage must run first)");
            return false;
        }

        const uint8_t* Y = ctx.fusedY.data();
        const uint8_t* U = ctx.fusedU.data();
        const uint8_t* V = ctx.fusedV.data();

        int uvW = w / 2;

        int numThreads = 8;
        int rowsPerThread = h / numThreads;
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);

        for (int t = 0; t < numThreads; ++t) {
            int rStart = t * rowsPerThread;
            int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

            futures.push_back(std::async(std::launch::async, [&ctx, Y, U, V, rStart, rEnd, w, uvW]() {
                for (int r = rStart; r < rEnd; ++r) {
                    convertRowScalar(
                        Y + r * w,
                        U + (r / 2) * uvW,
                        V + (r / 2) * uvW,
                        ctx.colorImage.rowPtr(r),
                        w);
                }
            }));
        }
        for (auto& fut : futures) {
            fut.get();
        }
    }

    // Save intermediate debayered RGB + JPEG
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            
            // Raw PPM — only written when debug_raw_dumps is enabled
            bool rawDumps = false;
            if (ctx.metadata.count("debug_raw_dumps")) {
                try { rawDumps = std::any_cast<bool>(ctx.metadata.at("debug_raw_dumps")); } catch (...) {}
            }
            if (rawDumps) {
                std::string ppmPath = debugDir + "/stage_2_debayer/debayered.ppm";
                std::ofstream out(ppmPath, std::ios::binary);
                if (out) {
                    out << "P6\n" << w << " " << h << "\n255\n";
                    out.write(reinterpret_cast<const char*>(ctx.colorImage.data.data()), ctx.colorImage.data.size());
                    out.close();
                }
            }
            
            // JPEG preview
            saveRgbAsJpeg(
                ctx.colorImage.data.data(), w, h,
                debugDir + "/stage_2_debayer/debayered.jpg");
        } catch (...) {}
    }

    LOGI("DebayerStage: converted %d×%d YUV → RGB", w, h);
    return true;
}
