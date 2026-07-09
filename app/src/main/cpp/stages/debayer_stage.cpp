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

        // Adjust scaling factor based on calculated dynamic range
        float scale = 255.f / std::max(1.f, whiteLevel - blackLevel);

        // Sony sensor color channel gain defaults to neutralize green tint
        float rGain = 2.1f;
        float bGain = 1.9f;
        float gGain = 1.0f;

        // ── GPU HEADLESS COMPUTE SHADER DEMOSAICING ────────────────────────────
        bool success = false;
        std::string errorLog;
        
        EglHeadlessSetup egl;
        if (egl.init(errorLog)) {
            const char* COMPUTE_DEMOSAIC_SRC = R"glsl(
                #version 310 es
                layout(local_size_x = 16, local_size_y = 16) in;

                precision highp float;
                precision highp usampler2D;

                uniform usampler2D u_raw_texture;
                uniform int u_width;
                uniform int u_height;
                uniform float u_black_level;
                uniform float u_scale;
                uniform float u_r_gain;
                uniform float u_g_gain;
                uniform float u_b_gain;

                layout(std430, binding = 0) writeonly buffer OutputBuffer {
                    uint outRGB[];
                };

                float getRaw(int x, int y) {
                    int cx = clamp(x, 0, u_width - 1);
                    int cy = clamp(y, 0, u_height - 1);
                    uint val = texelFetch(u_raw_texture, ivec2(cx, cy), 0).r;
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

                void main() {
                    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
                    if (pos.x >= u_width || pos.y >= u_height) return;

                    int r = pos.y;
                    int c = pos.x;

                    float R = 0.0;
                    float G = 0.0;
                    float B = 0.0;

                    bool isEvenRow = (r % 2 == 0);
                    bool isEvenCol = (c % 2 == 0);

                    if (isEvenRow && isEvenCol) {
                        // Blue pixel
                        R = (getRaw(c-1, r-1) + getRaw(c+1, r-1) + getRaw(c-1, r+1) + getRaw(c+1, r+1)) * 0.25;
                        G = (getRaw(c, r-1) + getRaw(c, r+1) + getRaw(c-1, r) + getRaw(c+1, r)) * 0.25;
                        B = getRaw(c, r);
                    } else if (!isEvenRow && !isEvenCol) {
                        // Red pixel
                        R = getRaw(c, r);
                        G = (getRaw(c, r-1) + getRaw(c, r+1) + getRaw(c-1, r) + getRaw(c+1, r)) * 0.25;
                        B = (getRaw(c-1, r-1) + getRaw(c+1, r-1) + getRaw(c-1, r+1) + getRaw(c+1, r+1)) * 0.25;
                    } else if (isEvenRow && !isEvenCol) {
                        // Green pixel on Blue row
                        R = (getRaw(c, r-1) + getRaw(c, r+1)) * 0.5;
                        G = getRaw(c, r);
                        B = (getRaw(c-1, r) + getRaw(c+1, r)) * 0.5;
                    } else {
                        // Green pixel on Red row
                        R = (getRaw(c-1, r) + getRaw(c+1, r)) * 0.5;
                        G = getRaw(c, r);
                        B = (getRaw(c, r-1) + getRaw(c, r+1)) * 0.5;
                    }

                    uint uR = uint(clamp(R, 0.0, 255.0));
                    uint uG = uint(clamp(G, 0.0, 255.0));
                    uint uB = uint(clamp(B, 0.0, 255.0));

                    uint packedVal = uR | (uG << 8) | (uB << 16) | (255u << 24);
                    outRGB[r * u_width + c] = packedVal;
                }
            )glsl";

            GLuint program = createComputeProgram(COMPUTE_DEMOSAIC_SRC, errorLog);
            if (program != 0) {
                glUseProgram(program);

                // Upload 16-bit RAW buffer as R16UI Texture
                GLuint rawTexture;
                glGenTextures(1, &rawTexture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, rawTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, w, h);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT, fusedRaw.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glUniform1i(glGetUniformLocation(program, "u_raw_texture"), 0);
                glUniform1i(glGetUniformLocation(program, "u_width"), w);
                glUniform1i(glGetUniformLocation(program, "u_height"), h);
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
                glDeleteTextures(1, &rawTexture);
                glDeleteProgram(program);
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
