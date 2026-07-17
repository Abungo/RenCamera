#include "debayer_stage.h"
#include "../debug_utils.h"
#include "../gl_helpers.h"

#include "../shader_loader.h"
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

static inline int getPixelColorPattern(int r, int c, int cfaPattern) {
    int row = r % 2;
    int col = c % 2;
    if (cfaPattern == 0) { // RGGB
        if (row == 0 && col == 0) return 0; // Red
        if (row == 1 && col == 1) return 2; // Blue
        return 1; // Green
    } else if (cfaPattern == 1) { // GRBG
        if (row == 0 && col == 1) return 0; // Red
        if (row == 1 && col == 0) return 2; // Blue
        return 1; // Green
    } else if (cfaPattern == 2) { // GBRG
        if (row == 1 && col == 0) return 0; // Red
        if (row == 0 && col == 1) return 2; // Blue
        return 1; // Green
    } else { // BGGR (3)
        if (row == 1 && col == 1) return 0; // Red
        if (row == 0 && col == 0) return 2; // Blue
        return 1; // Green
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

    float rGain = 1.0f;
    float gGain = 1.0f;
    float bGain = 1.0f;
    std::vector<float> ccm = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    uint16_t minVal = 65535;
    uint16_t maxVal = 0;
    float blackLevel = 1024.f;
    float whiteLevel = 4095.f;

    int cfaPattern = 3; // default BGGR
    if (ctx.metadata.count("cfa_pattern")) {
        try { cfaPattern = std::any_cast<int>(ctx.metadata.at("cfa_pattern")); } catch (...) {}
    }

    if (useRaw) {
        if (!ctx.metadata.count("fused_raw")) {
            LOGE("DebayerStage: no fused RAW frame in metadata");
            return false;
        }
        const auto& fusedRaw = std::any_cast<const std::vector<uint16_t>&>(ctx.metadata.at("fused_raw"));
        
        bool hasMetadataRange = false;
        if (ctx.metadata.count("black_level") && ctx.metadata.count("white_level")) {
            try {
                blackLevel = std::any_cast<float>(ctx.metadata.at("black_level"));
                whiteLevel = std::any_cast<float>(ctx.metadata.at("white_level"));
                hasMetadataRange = true;
                LOGI("DebayerStage: Using JNI metadata black level = %.1f, white level = %.1f", blackLevel, whiteLevel);
            } catch (...) {}
        }

        if (!hasMetadataRange) {
            // Dynamically detect bit depth and black level from fusedRaw buffer
            blackLevel = 1024.f;
            whiteLevel = 4095.f;
            
            minVal = 65535;
            maxVal = 0;
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
        }

        float digitalGain = 1.0f;
        if (ctx.metadata.count("digital_gain")) {
            try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
        }
        LOGI("DebayerStage: applying digital gain = %.3fx", digitalGain);

        // Adjust scaling factor based on calculated dynamic range to preserve highlights from clipping
        float scale = 255.f / std::max(1.f, whiteLevel - blackLevel);

        // Sony sensor color channel gain defaults to neutralize green tint
        rGain = 2.1f;
        bGain = 1.9f;
        gGain = 1.0f;

        if (ctx.metadata.count("awb_gains")) {
            try {
                auto gains = std::any_cast<std::vector<float>>(ctx.metadata.at("awb_gains"));
                if (gains.size() >= 3) {
                    rGain = gains[0];
                    gGain = gains[1];
                    bGain = gains[2];
                }
            } catch (...) {}
        }

        ccm = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f
        };
        if (ctx.metadata.count("color_correction_matrix")) {
            try {
                ccm = std::any_cast<std::vector<float>>(ctx.metadata.at("color_correction_matrix"));
            } catch (...) {}
        }

        // ── Lens Shading Correction (LSC) applied in-place to fused RAW before debayering
        // The LSC gain map corrects vignetting: each pixel's raw value is divided by its
        // channel-specific shading gain (which is >= 1.0, highest in corners).
        std::vector<float> lscData;
        int lscMapW = 0, lscMapH = 0;
        if (ctx.metadata.count("lsc_data") && ctx.metadata.count("lsc_map_width") && ctx.metadata.count("lsc_map_height")) {
            try {
                lscData   = std::any_cast<std::vector<float>>(ctx.metadata.at("lsc_data"));
                lscMapW   = std::any_cast<int>(ctx.metadata.at("lsc_map_width"));
                lscMapH   = std::any_cast<int>(ctx.metadata.at("lsc_map_height"));
            } catch (...) {}
        }

        const bool hasLsc = lscMapW > 1 && lscMapH > 1 &&
                            static_cast<int>(lscData.size()) == 4 * lscMapW * lscMapH;
        if (hasLsc) {
            LOGI("DebayerStage: applying LSC map %d×%d to %d×%d fused RAW", lscMapW, lscMapH, w, h);
            // The gain map channels: 0=R, 1=Gr, 2=Gb, 3=B
            // For RGGB CFA: ch 0=R, ch 1=Gr, ch 2=Gb, ch 3=B
            // We bilinearly interpolate from the low-resolution LSC map to full image coordinates.
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    // Map pixel (col, row) to LSC map coordinates
                    float mapX = (col + 0.5f) * lscMapW / w - 0.5f;
                    float mapY = (row + 0.5f) * lscMapH / h - 0.5f;
                    mapX = std::clamp(mapX, 0.f, static_cast<float>(lscMapW - 1));
                    mapY = std::clamp(mapY, 0.f, static_cast<float>(lscMapH - 1));

                    int mx0 = static_cast<int>(mapX);
                    int my0 = static_cast<int>(mapY);
                    int mx1 = std::min(mx0 + 1, lscMapW - 1);
                    int my1 = std::min(my0 + 1, lscMapH - 1);
                    float fx = mapX - mx0;
                    float fy = mapY - my0;

                    // Determine Bayer channel (0=R, 1=Gr, 2=Gb, 3=B)
                    int bayerCh;
                    int pr = row % 2, pc = col % 2;
                    if (cfaPattern == 0) {      // RGGB
                        if (pr == 0 && pc == 0)      bayerCh = 0; // R
                        else if (pr == 0 && pc == 1) bayerCh = 1; // Gr
                        else if (pr == 1 && pc == 0) bayerCh = 2; // Gb
                        else                         bayerCh = 3; // B
                    } else if (cfaPattern == 1) { // GRBG
                        if (pr == 0 && pc == 0)      bayerCh = 1; // Gr
                        else if (pr == 0 && pc == 1) bayerCh = 0; // R
                        else if (pr == 1 && pc == 0) bayerCh = 3; // B
                        else                         bayerCh = 2; // Gb
                    } else if (cfaPattern == 2) { // GBRG
                        if (pr == 0 && pc == 0)      bayerCh = 2; // Gb
                        else if (pr == 0 && pc == 1) bayerCh = 3; // B
                        else if (pr == 1 && pc == 0) bayerCh = 0; // R
                        else                         bayerCh = 1; // Gr
                    } else {                    // BGGR (3)
                        if (pr == 0 && pc == 0)      bayerCh = 3; // B
                        else if (pr == 0 && pc == 1) bayerCh = 2; // Gb
                        else if (pr == 1 && pc == 0) bayerCh = 1; // Gr
                        else                         bayerCh = 0; // R
                    }

                    // Bilinear interpolation of the gain from the 4 surrounding map cells
                    auto getGain = [&](int my, int mx) -> float {
                        int base = (my * lscMapW + mx) * 4;
                        return lscData[base + bayerCh];
                    };
                    float gain = (1 - fy) * ((1 - fx) * getGain(my0, mx0) + fx * getGain(my0, mx1))
                               +      fy  * ((1 - fx) * getGain(my1, mx0) + fx * getGain(my1, mx1));
                    gain = std::max(1.0f, gain); // gains are always >= 1.0 by definition

                    // Multiply raw pixel (pedestal-subtracted) by gain to normalize shading (LSC correction)
                    auto& pixel = const_cast<std::vector<uint16_t>&>(fusedRaw)[row * w + col];
                    float val = static_cast<float>(pixel) - blackLevel;
                    float corrected = std::max(0.f, val) * gain + blackLevel;
                    pixel = static_cast<uint16_t>(std::clamp(corrected, 0.f, static_cast<float>(whiteLevel)));
                }
            }
            LOGI("DebayerStage: LSC applied successfully");
        }

        // ── GPU HEADLESS COMPUTE SHADER SABRE MULTI-FRAME DEMOSAICING ────────────────────────────
        bool success = false;
        std::string errorLog;
        
        int numFrames = ctx.inputFrames.size();

        if (numFrames == 1) {
            LOGI("DebayerStage: single frame detected, bypassing Sabre GPU compute. Using CPU Bilinear demosaicing fallback.");
        } else {
            EglHeadlessSetup egl;
            if (egl.init(errorLog)) {
            const char* COMPUTE_SABRE_SRC = BuiltInShaders::SABRE_DEBAYER.c_str();
            GLuint program = createComputeProgram(COMPUTE_SABRE_SRC, errorLog);
            if (program != 0) {
                glUseProgram(program);

                // 1. Upload the temporally fused RAW frame to slice 0 of the texture array
                GLuint rawTextureArray;
                glGenTextures(1, &rawTextureArray);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, rawTextureArray);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R16UI, w, h, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, w, h, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, fusedRaw.data());
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
                glUniform1i(glGetUniformLocation(program, "u_num_frames"), 1);
                glUniform1i(glGetUniformLocation(program, "u_width"), w);
                glUniform1i(glGetUniformLocation(program, "u_height"), h);
                glUniform1i(glGetUniformLocation(program, "u_blocks_wide"), blocksWide);
                glUniform1i(glGetUniformLocation(program, "u_blocks_tall"), blocksTall);
                glUniform1f(glGetUniformLocation(program, "u_block_size"), static_cast<float>(blockSize));

                int iso = 100;
                if (ctx.metadata.count("iso")) {
                    try { iso = std::any_cast<int>(ctx.metadata.at("iso")); } catch (...) {}
                }
                float digitalGain = 1.0f;
                if (ctx.metadata.count("digital_gain")) {
                    try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
                }
                float effectiveIso = iso * digitalGain;
                float rawRangeSigma = 0.015f + (effectiveIso / 100.0f) * 0.012f;
                rawRangeSigma = std::clamp(rawRangeSigma, 0.015f, 0.25f);

                glUniform1f(glGetUniformLocation(program, "u_black_level"), blackLevel);
                glUniform1f(glGetUniformLocation(program, "u_scale"), scale);
                glUniform1f(glGetUniformLocation(program, "u_raw_range_sigma"), rawRangeSigma);
                glUniform1f(glGetUniformLocation(program, "u_r_gain"), rGain);
                glUniform1f(glGetUniformLocation(program, "u_g_gain"), gGain);
                glUniform1f(glGetUniformLocation(program, "u_b_gain"), bGain);
                glUniform1i(glGetUniformLocation(program, "u_cfa_pattern"), cfaPattern);

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
            // Bilinear Demosaicing (Dynamic CFA pattern layout) - Multi-threaded CPU Fallback
            int numThreads = 8;
            int rowsPerThread = h / numThreads;
            std::vector<std::future<void>> futures;
            futures.reserve(numThreads);

            int iso = 100;
            if (ctx.metadata.count("iso")) {
                try { iso = std::any_cast<int>(ctx.metadata.at("iso")); } catch (...) {}
            }
            float digitalGain = 1.0f;
            if (ctx.metadata.count("digital_gain")) {
                try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
            }
            float effectiveIso = iso * digitalGain;
            float rawRangeSigma = 0.015f + (effectiveIso / 100.0f) * 0.012f;
            rawRangeSigma = std::clamp(rawRangeSigma, 0.015f, 0.25f);

            for (int t = 0; t < numThreads; ++t) {
                int rStart = t * rowsPerThread;
                int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

                futures.push_back(std::async(std::launch::async, [&ctx, &fusedRaw, rStart, rEnd, w, h, blackLevel, scale, rGain, gGain, bGain, ccm, cfaPattern, rawRangeSigma, numFrames]() {
                    auto getRaw = [&fusedRaw, w, h, blackLevel, scale, rGain, gGain, bGain, cfaPattern, rawRangeSigma, numFrames](int r, int cc) -> float {
                        int cr = std::clamp(r, 0, h - 1);
                        int c_clamped = std::clamp(cc, 0, w - 1);
                        float centerVal = static_cast<float>(fusedRaw[cr * w + c_clamped]);
                        float centerClean = std::max(0.f, (centerVal - blackLevel) * scale);

                        float cleanVal = centerClean;

                        // CPU same-color Bayer bilateral filter ONLY for single noisy RAW input
                        if (numFrames == 1) {
                            float sumVal = 0.f;
                            float sumW = 0.f;
                            float rSigma2 = 2.f * rawRangeSigma * rawRangeSigma;
                            float sSigma2 = 2.f * 1.5f * 1.5f;

                            for (int dy = -2; dy <= 2; dy += 2) {
                                for (int dx = -2; dx <= 2; dx += 2) {
                                    int nr = std::clamp(cr + dy, 0, h - 1);
                                    int nc = std::clamp(c_clamped + dx, 0, w - 1);
                                    float val = static_cast<float>(fusedRaw[nr * w + nc]);
                                    float clean = std::max(0.f, (val - blackLevel) * scale);

                                    float diff = clean - centerClean;
                                    float dS2 = static_cast<float>((dx/2)*(dx/2) + (dy/2)*(dy/2));
                                    float weight = std::expf(-dS2 / sSigma2) * std::expf(-diff*diff / rSigma2);

                                    sumVal += weight * clean;
                                    sumW += weight;
                                }
                            }
                            if (sumW > 1e-4f) {
                                cleanVal = sumVal / sumW;
                            }
                        }

                        int color = getPixelColorPattern(cr, c_clamped, cfaPattern);
                        if (color == 0) {
                            return cleanVal * rGain; // Red
                        } else if (color == 2) {
                            return cleanVal * bGain; // Blue
                        } else {
                            return cleanVal * gGain; // Green
                        }
                    };

                    for (int r = rStart; r < rEnd; ++r) {
                        uint8_t* rgbRow = ctx.colorImage.rowPtr(r);
                        for (int c = 0; c < w; ++c) {
                            float R = 0.f, G = 0.f, B = 0.f;
                            int color = getPixelColorPattern(r, c, cfaPattern);

                            // Compute Horizontal and Vertical Gradients for Green channel selection
                            float hGrad = std::abs(getRaw(r, c - 2) - 2.0f * getRaw(r, c) + getRaw(r, c + 2)) +
                                          std::abs(getRaw(r, c - 1) - getRaw(r, c + 1));
                            float vGrad = std::abs(getRaw(r - 2, c) - 2.0f * getRaw(r, c) + getRaw(r + 2, c)) +
                                          std::abs(getRaw(r - 1, c) - getRaw(r + 1, c));

                            float greenH = 0.0f;
                            float greenV = 0.0f;

                            if (color == 1) { // Green site
                                greenH = getRaw(r, c);
                                greenV = greenH;
                            } else {
                                // Horizontal interpolation guided by Laplacian
                                greenH = (getRaw(r, c - 1) + getRaw(r, c + 1)) * 0.5f +
                                         (2.0f * getRaw(r, c) - getRaw(r, c - 2) - getRaw(r, c + 2)) * 0.25f;
                                // Vertical interpolation guided by Laplacian
                                greenV = (getRaw(r - 1, c) + getRaw(r + 1, c)) * 0.5f +
                                         (2.0f * getRaw(r, c) - getRaw(r - 2, c) - getRaw(r + 2, c)) * 0.25f;
                            }

                            // Choose green value along the edge with lower gradient
                            float G_val = (hGrad < vGrad) ? greenH : ((vGrad < hGrad) ? greenV : (greenH + greenV) * 0.5f);

                            if (color == 0) { // Red pixel
                                R = getRaw(r, c);
                                G = G_val;
                                
                                // Interpolate Blue at Red site: guided by Green differences
                                float b_g_topleft  = getRaw(r - 1, c - 1) - (getRaw(r - 2, c - 1) + getRaw(r, c - 1) + getRaw(r - 1, c - 2) + getRaw(r - 1, c)) * 0.25f;
                                float b_g_topright = getRaw(r - 1, c + 1) - (getRaw(r - 2, c + 1) + getRaw(r, c + 1) + getRaw(r - 1, c) + getRaw(r - 1, c + 2)) * 0.25f;
                                float b_g_botleft  = getRaw(r + 1, c - 1) - (getRaw(r, c - 1) + getRaw(r + 2, c - 1) + getRaw(r + 1, c - 2) + getRaw(r + 1, c)) * 0.25f;
                                float b_g_botright = getRaw(r + 1, c + 1) - (getRaw(r, c + 1) + getRaw(r + 2, c + 1) + getRaw(r + 1, c) + getRaw(r + 1, c + 2)) * 0.25f;
                                B = G + (b_g_topleft + b_g_topright + b_g_botleft + b_g_botright) * 0.25f;
                            } else if (color == 2) { // Blue pixel
                                B = getRaw(r, c);
                                G = G_val;
                                
                                // Interpolate Red at Blue site
                                float r_g_topleft  = getRaw(r - 1, c - 1) - (getRaw(r - 2, c - 1) + getRaw(r, c - 1) + getRaw(r - 1, c - 2) + getRaw(r - 1, c)) * 0.25f;
                                float r_g_topright = getRaw(r - 1, c + 1) - (getRaw(r - 2, c + 1) + getRaw(r, c + 1) + getRaw(r - 1, c) + getRaw(r - 1, c + 2)) * 0.25f;
                                float r_g_botleft  = getRaw(r + 1, c - 1) - (getRaw(r, c - 1) + getRaw(r + 2, c - 1) + getRaw(r + 1, c - 2) + getRaw(r + 1, c)) * 0.25f;
                                float r_g_botright = getRaw(r + 1, c + 1) - (getRaw(r, c + 1) + getRaw(r + 2, c + 1) + getRaw(r + 1, c) + getRaw(r + 1, c + 2)) * 0.25f;
                                R = G + (r_g_topleft + r_g_topright + r_g_botleft + r_g_botright) * 0.25f;
                            } else { // Green pixel
                                G = G_val;
                                bool isRedRow = (getPixelColorPattern(r, c - 1, cfaPattern) == 0 || getPixelColorPattern(r, c + 1, cfaPattern) == 0);
                                if (isRedRow) {
                                    R = (getRaw(r, c - 1) + getRaw(r, c + 1)) * 0.5f;
                                    B = (getRaw(r - 1, c) + getRaw(r + 1, c)) * 0.5f;
                                } else {
                                    R = (getRaw(r - 1, c) + getRaw(r + 1, c)) * 0.5f;
                                    B = (getRaw(r, c - 1) + getRaw(r, c + 1)) * 0.5f;
                                }
                            }

                            // CRITICAL: Direct Original Pixel Injection
                            // Inject the raw home pixel value directly to preserve 100% of the original sensor detail
                            float homeVal = getRaw(r, c);
                            if (color == 0) {
                                R = homeVal;
                            } else if (color == 1) {
                                G = homeVal;
                            } else {
                                B = homeVal;
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
            
            // Create a gamma-corrected copy for preview visualization (without modifying the linear pipeline data)
            std::vector<uint8_t> previewRgb(ctx.colorImage.data.size());
            for (size_t i = 0; i < previewRgb.size(); ++i) {
                float norm = ctx.colorImage.data[i] / 255.f;
                previewRgb[i] = static_cast<uint8_t>(std::clamp(std::sqrt(norm) * 255.f, 0.f, 255.f));
            }

            // JPEG preview
            saveRgbAsJpeg(
                previewRgb.data(), w, h,
                debugDir + "/stage_2_debayer/debayered.jpg");

            // Save white balance and CCM info
            std::string infoPath = debugDir + "/stage_2_debayer/color_info.txt";
            std::ofstream infoOut(infoPath);
            if (infoOut) {
                infoOut << "AWB Gains:\n";
                infoOut << "R: " << rGain << "\n";
                infoOut << "G: " << gGain << "\n";
                infoOut << "B: " << bGain << "\n\n";
                infoOut << "CCM Matrix:\n";
                infoOut << ccm[0] << ", " << ccm[1] << ", " << ccm[2] << "\n";
                infoOut << ccm[3] << ", " << ccm[4] << ", " << ccm[5] << "\n";
                infoOut << ccm[6] << ", " << ccm[7] << ", " << ccm[8] << "\n\n";
                infoOut << "CFA Pattern: " << cfaPattern << "\n\n";
                infoOut << "RAW detection:\n";
                infoOut << "Min Val: " << minVal << "\n";
                infoOut << "Max Val: " << maxVal << "\n";
                infoOut << "Black Level: " << blackLevel << "\n";
                infoOut << "White Level: " << whiteLevel << "\n";
                infoOut.close();
            }
        } catch (...) {}
    }

    LOGI("DebayerStage: converted %d×%d YUV → RGB", w, h);
    return true;
}
