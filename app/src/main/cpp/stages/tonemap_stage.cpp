#include "tonemap_stage.h"
#include "../debug_utils.h"
#include "../gl_helpers.h"

#include "../shader_loader.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <fstream>
#include <any>
#include <future>

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Bilateral grid parameters
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int   SPATIAL_SIGMA   = 16;  // spatial bin width (pixels)
static constexpr int   INTENSITY_BINS  = 16;  // number of intensity levels in grid
static constexpr float DETAIL_ALPHA    = 1.2f; // detail boost factor (>1 = sharper)
static constexpr float SAT_BOOST       = 1.15f; // saturation multiplier

// ─────────────────────────────────────────────────────────────────────────────
// Luminance
// ─────────────────────────────────────────────────────────────────────────────
static inline float luma(uint8_t r, uint8_t g, uint8_t b) {
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

static void applyWhiteBalance(RgbImage& img, float alpha) {
    double rSum = 0, gSum = 0, bSum = 0;
    uint64_t count = 0;
    int w = img.width;
    int h = img.height;

    // Subsample for performance (every 4th pixel)
    for (int r = 0; r < h; r += 4) {
        const uint8_t* row = img.rowPtr(r);
        for (int c = 0; c < w; c += 4) {
            uint8_t R = row[c*3], G = row[c*3+1], B = row[c*3+2];
            float L = 0.299f * R + 0.587f * G + 0.114f * B;
            if (L > 5.f && L < 220.f) {
                rSum += R;
                gSum += G;
                bSum += B;
                count++;
            }
        }
    }

    if (count > 100) {
        double rMean = rSum / count;
        double gMean = gSum / count;
        double bMean = bSum / count;

        double gainR = gMean / std::max(1.0, rMean);
        double gainB = gMean / std::max(1.0, bMean);

        // Clamp gains to prevent extreme shifts
        gainR = std::max(0.4, std::min(gainR, 2.5));
        gainB = std::max(0.4, std::min(gainB, 2.5));

        // Soften white balance correction to keep some warm atmosphere
        gainR = 1.0 + alpha * (gainR - 1.0);
        gainB = 1.0 + alpha * (gainB - 1.0);

        LOGI("Auto White Balance: computed gains R=%.3f, B=%.3f (alpha=%.2f)", gainR, gainB, alpha);

        for (int r = 0; r < h; ++r) {
            uint8_t* row = img.rowPtr(r);
            for (int c = 0; c < w; ++c) {
                row[c*3]   = static_cast<uint8_t>(std::clamp(row[c*3]   * gainR, 0.0, 255.0));
                row[c*3+2] = static_cast<uint8_t>(std::clamp(row[c*3+2] * gainB, 0.0, 255.0));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ACES Filmic Tone Mapping Curve (fitted approximation)
//   Input range: [0, 1], Output range: [0, 1]
// ─────────────────────────────────────────────────────────────────────────────
static inline float acesFilm(float x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.f, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bilateral Grid — 3-D grid indexed by (gridY, gridX, intensityBin)
// ─────────────────────────────────────────────────────────────────────────────
struct BilateralGrid {
    // Stored as flat array: [gY * gW * INTENSITY_BINS + gX * INTENSITY_BINS + gI]
    std::vector<float> splatSum; // Σ w * L
    std::vector<float> splatWgt; // Σ w
    int gW = 0, gH = 0;

    void init(int imageW, int imageH) {
        gW = (imageW  + SPATIAL_SIGMA - 1) / SPATIAL_SIGMA;
        gH = (imageH  + SPATIAL_SIGMA - 1) / SPATIAL_SIGMA;
        size_t sz = static_cast<size_t>(gW) * gH * INTENSITY_BINS;
        splatSum.assign(sz, 0.f);
        splatWgt.assign(sz, 0.f);
    }

    size_t idx(int gy, int gx, int gi) const {
        return static_cast<size_t>(gy) * gW * INTENSITY_BINS
             + static_cast<size_t>(gx) * INTENSITY_BINS
             + gi;
    }

    // Splat pixel (x, y, L) into the grid
    void splat(int x, int y, float L) {
        int   gx = x / SPATIAL_SIGMA;
        int   gy = y / SPATIAL_SIGMA;
        float fi = L * (INTENSITY_BINS - 1) / 255.f;
        int   gi = std::clamp(static_cast<int>(fi), 0, INTENSITY_BINS - 2);
        float wI = fi - gi; // intensity interpolation weight

        // Trilinear splat into two intensity bins
        splatSum[idx(gy, gx, gi    )] += (1.f - wI) * L;
        splatSum[idx(gy, gx, gi + 1)] += wI          * L;
        splatWgt[idx(gy, gx, gi    )] += (1.f - wI);
        splatWgt[idx(gy, gx, gi + 1)] += wI;
    }

    // Blur the grid: 3-tap box filter along each dimension
    void blur() {
        // Spatial X blur
        for (int gy = 0; gy < gH; ++gy) {
            for (int gi = 0; gi < INTENSITY_BINS; ++gi) {
                std::vector<float> tmp(gW);
                for (int gx = 0; gx < gW; ++gx) {
                    int l = std::max(gx-1, 0), r = std::min(gx+1, gW-1);
                    tmp[gx] = (splatSum[idx(gy,l,gi)] + splatSum[idx(gy,gx,gi)] + splatSum[idx(gy,r,gi)]) / 3.f;
                }
                for (int gx = 0; gx < gW; ++gx) splatSum[idx(gy,gx,gi)] = tmp[gx];
                for (int gx = 0; gx < gW; ++gx) {
                    int l = std::max(gx-1,0), r = std::min(gx+1,gW-1);
                    tmp[gx] = (splatWgt[idx(gy,l,gi)] + splatWgt[idx(gy,gx,gi)] + splatWgt[idx(gy,r,gi)]) / 3.f;
                }
                for (int gx = 0; gx < gW; ++gx) splatWgt[idx(gy,gx,gi)] = tmp[gx];
            }
        }
        // Spatial Y blur
        for (int gx = 0; gx < gW; ++gx) {
            for (int gi = 0; gi < INTENSITY_BINS; ++gi) {
                std::vector<float> tmp(gH), tmpW(gH);
                for (int gy = 0; gy < gH; ++gy) {
                    int u = std::max(gy-1,0), d = std::min(gy+1,gH-1);
                    tmp [gy] = (splatSum[idx(u,gx,gi)] + splatSum[idx(gy,gx,gi)] + splatSum[idx(d,gx,gi)]) / 3.f;
                    tmpW[gy] = (splatWgt[idx(u,gx,gi)] + splatWgt[idx(gy,gx,gi)] + splatWgt[idx(d,gx,gi)]) / 3.f;
                }
                for (int gy = 0; gy < gH; ++gy) {
                    splatSum[idx(gy,gx,gi)] = tmp[gy];
                    splatWgt[idx(gy,gx,gi)] = tmpW[gy];
                }
            }
        }
        // Intensity Z blur (Range dimension)
        for (int gy = 0; gy < gH; ++gy) {
            for (int gx = 0; gx < gW; ++gx) {
                float tmp[INTENSITY_BINS];
                float tmpW[INTENSITY_BINS];
                for (int gi = 0; gi < INTENSITY_BINS; ++gi) {
                    int u = std::max(gi-1, 0), d = std::min(gi+1, INTENSITY_BINS-1);
                    tmp [gi] = (splatSum[idx(gy,gx,u)] + splatSum[idx(gy,gx,gi)] + splatSum[idx(gy,gx,d)]) / 3.f;
                    tmpW[gi] = (splatWgt[idx(gy,gx,u)] + splatWgt[idx(gy,gx,gi)] + splatWgt[idx(gy,gx,d)]) / 3.f;
                }
                for (int gi = 0; gi < INTENSITY_BINS; ++gi) {
                    splatSum[idx(gy,gx,gi)] = tmp[gi];
                    splatWgt[idx(gy,gx,gi)] = tmpW[gi];
                }
            }
        }
    }

    std::vector<float> normalizedGrid; // Precomputed sum / weight values

    void normalize() {
        size_t sz = splatSum.size();
        normalizedGrid.resize(sz);
        for (size_t i = 0; i < sz; ++i) {
            float s = splatSum[i];
            float w = splatWgt[i];
            normalizedGrid[i] = (w > 1e-6f) ? (s / w) : -1.f;
        }
    }

    // Sample base luminance for pixel (x, y, L) via trilinear interpolation
    float sample(int x, int y, float L) const {
        float fgx = static_cast<float>(x) / SPATIAL_SIGMA;
        float fgy = static_cast<float>(y) / SPATIAL_SIGMA;
        float fi  = L * (INTENSITY_BINS - 1) / 255.f;

        int gx0 = std::clamp(static_cast<int>(fgx), 0, gW - 1);
        int gy0 = std::clamp(static_cast<int>(fgy), 0, gH - 1);
        int gi0 = std::clamp(static_cast<int>(fi),  0, INTENSITY_BINS - 2);
        int gx1 = std::min(gx0 + 1, gW - 1);
        int gy1 = std::min(gy0 + 1, gH - 1);
        int gi1 = gi0 + 1;

        float wx = fgx - gx0, wy = fgy - gy0, wI = fi - gi0;

        // Trilinear interpolation using pre-normalized grid
        auto val = [&](int gya, int gxa, int gia) -> float {
            float val = normalizedGrid[idx(gya, gxa, gia)];
            return (val >= 0.f) ? val : L;
        };

        float v000 = val(gy0, gx0, gi0), v001 = val(gy0, gx0, gi1);
        float v010 = val(gy0, gx1, gi0), v011 = val(gy0, gx1, gi1);
        float v100 = val(gy1, gx0, gi0), v101 = val(gy1, gx0, gi1);
        float v110 = val(gy1, gx1, gi0), v111 = val(gy1, gx1, gi1);

        return (1-wy)*((1-wx)*((1-wI)*v000 + wI*v001) + wx*((1-wI)*v010 + wI*v011))
             +    wy *((1-wx)*((1-wI)*v100 + wI*v101) + wx*((1-wI)*v110 + wI*v111));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Saturation adjustment (Luma-Interpolation preserves hue and limits clipping)
// ─────────────────────────────────────────────────────────────────────────────
static void adjustSaturation(uint8_t& rr, uint8_t& gg, uint8_t& bb, float satBoost) {
    float r = rr;
    float g = gg;
    float b = bb;
    float L = 0.299f * r + 0.587f * g + 0.114f * b;
    rr = static_cast<uint8_t>(std::clamp(L + satBoost * (r - L), 0.f, 255.f));
    gg = static_cast<uint8_t>(std::clamp(L + satBoost * (g - L), 0.f, 255.f));
    bb = static_cast<uint8_t>(std::clamp(L + satBoost * (b - L), 0.f, 255.f));
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// ToneMapStage::process
// ─────────────────────────────────────────────────────────────────────────────
bool ToneMapStage::process(FrameContext& ctx) {
    if (ctx.colorImage.data.empty()) {
        LOGE("ToneMapStage: no color image (DebayerStage must run first)");
        return false;
    }

    int w = ctx.colorImage.width;
    int h = ctx.colorImage.height;

    // Calculate scene key (logarithmic mean luminance to be robust against small bright highlights)
    double logLumaSum = 0;
    uint64_t lumaCount = 0;
    for (int r = 0; r < h; r += 8) {
        const uint8_t* row = ctx.colorImage.rowPtr(r);
        for (int c = 0; c < w; c += 8) {
            float l = luma(row[c*3], row[c*3+1], row[c*3+2]);
            logLumaSum += std::logf(l + 1.f);
            lumaCount++;
        }
    }
    float meanL = (lumaCount > 0) ? (std::expf(static_cast<float>(logLumaSum / lumaCount)) - 1.f) : 10.f;

    bool isNight = false;
    if (ctx.metadata.count("night_mode")) {
        try {
            isNight = std::any_cast<bool>(ctx.metadata.at("night_mode"));
        } catch (...) {}
    }

    // Adapt gamma based on actual scene brightness
    // Darker scenes get a stronger shadow boost, brighter scenes stay natural.
    float gammaMin = isNight ? 0.45f : 0.50f;
    float gammaMax = isNight ? 0.65f : 0.70f;

    float t_mean = std::clamp((meanL - 10.f) / (80.f - 10.f), 0.f, 1.f);
    float adaptiveGamma = gammaMin + t_mean * (gammaMax - gammaMin);

    float awbSoftnessNormal = 0.60f;
    if (ctx.metadata.count("awb_softness_normal")) {
        try {
            awbSoftnessNormal = std::any_cast<float>(ctx.metadata.at("awb_softness_normal"));
        } catch (...) {}
    }

    float awbSoftnessNight = 0.85f;
    if (ctx.metadata.count("awb_softness_night")) {
        try {
            awbSoftnessNight = std::any_cast<float>(ctx.metadata.at("awb_softness_night"));
        } catch (...) {}
    }

    float detailAlpha = 1.35f;
    if (ctx.metadata.count("detail_alpha")) {
        try {
            detailAlpha = std::any_cast<float>(ctx.metadata.at("detail_alpha"));
        } catch (...) {}
    }

    float saturationBoost = 1.15f;
    if (ctx.metadata.count("saturation_boost")) {
        try {
            saturationBoost = std::any_cast<float>(ctx.metadata.at("saturation_boost"));
        } catch (...) {}
    }

    float blackPointClamp = 0.02f;
    if (ctx.metadata.count("black_point_clamp")) {
        try {
            blackPointClamp = std::any_cast<float>(ctx.metadata.at("black_point_clamp"));
        } catch (...) {}
    }

    std::vector<float> ccm = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };
    if (ctx.metadata.count("color_correction_matrix")) {
        try {
            ccm = std::any_cast<std::vector<float>>(ctx.metadata.at("color_correction_matrix"));
        } catch (...) {}
    }

    float appliedEvCompensation = 0.0f;
    if (ctx.metadata.count("applied_ev_compensation")) {
        try {
            appliedEvCompensation = std::any_cast<float>(ctx.metadata.at("applied_ev_compensation"));
        } catch (...) {}
    }
    int iso = 100;
    if (ctx.metadata.count("iso")) {
        try {
            iso = std::any_cast<int>(ctx.metadata.at("iso"));
        } catch (...) {}
    }

    float digitalGain = 1.0f;
    if (ctx.metadata.count("digital_gain")) {
        try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
    }
    float effectiveIso = iso * digitalGain;

    // ISO-adaptive luma denoise parameters (bilateral filter) using effectiveIso
    float spatialSigma = 2.0f + (effectiveIso / 100.0f) * 0.1f;
    spatialSigma = std::clamp(spatialSigma, 2.0f, 4.0f);

    float rangeSigma = 6.0f + (effectiveIso / 100.0f) * 1.5f;
    rangeSigma = std::clamp(rangeSigma, 6.0f, 35.0f);

    float chromaRangeSigma = 60.0f + (effectiveIso / 100.0f) * 3.5f;
    chromaRangeSigma = std::clamp(chromaRangeSigma, 60.0f, 150.0f);

    // ISO-adaptive final gamma correction using effectiveIso
    float finalGamma = 1.15f + (effectiveIso / 100.0f) * 0.015f;
    finalGamma = std::clamp(finalGamma, 1.15f, 1.35f);

    double S = 1.0e-5 * (effectiveIso / 100.0);
    double O = 1.0e-6 * (effectiveIso / 100.0) * (effectiveIso / 100.0);
    
    std::vector<float> noiseProfiles;
    if (ctx.metadata.count("noise_profiles")) {
        try {
            noiseProfiles = std::any_cast<std::vector<float>>(ctx.metadata.at("noise_profiles"));
            if (noiseProfiles.size() >= 8) {
                float sumS = 0.0f;
                float sumO = 0.0f;
                for (int ch = 0; ch < 4; ++ch) {
                    sumS += noiseProfiles[ch * 2];
                    sumO += noiseProfiles[ch * 2 + 1];
                }
                double avgS = sumS / 4.0;
                double avgO = sumO / 4.0;
                if (avgS > 1e-8) {
                    S = avgS;
                    O = avgO;
                } else {
                    LOGI("ToneMapStage: Bypassing camera hardware noise profile because values are near-zero (S=%.8f). Using fallback model.", avgS);
                }
            }
        } catch (...) {}
    }

    int spatialDenoiseStrength = 8;
    if (ctx.metadata.count("spatial_denoise_strength")) {
        try {
            spatialDenoiseStrength = std::any_cast<int>(ctx.metadata.at("spatial_denoise_strength"));
        } catch (...) {}
    }
    
    float nlmStrengthMultiplier = 0.0f;
    // Pre-tonemap NLM disabled: post-tonemap NLM with texture-aware gate handles all denoising
    // more effectively in gamma space. Pre-tonemap pass only risks blurring sharp detail.
    if (false && spatialDenoiseStrength > 0) {
        nlmStrengthMultiplier = (spatialDenoiseStrength / 8.0f) * 16.0f;
    }

    LOGI("ToneMapStage: Noise Model parameters -> S = %.8f, O = %.8f, NLM strength multiplier = %.2f (effectiveISO = %.1f)", 
         S, O, nlmStrengthMultiplier, effectiveIso);

    // Save intermediate debug frames (Before White Balance / Debayered RGB)
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            // Create a gamma-corrected copy for debug preview visualization
            std::vector<uint8_t> previewRgb(ctx.colorImage.data.size());
            for (size_t i = 0; i < previewRgb.size(); ++i) {
                float norm = ctx.colorImage.data[i] / 255.f;
                previewRgb[i] = static_cast<uint8_t>(std::clamp(std::sqrt(norm) * 255.f, 0.f, 255.f));
            }
            saveRgbAsJpeg(
                previewRgb.data(), w, h,
                debugDir + "/stage_3_tonemap/before_white_balance.jpg");
        } catch (...) {}
    }

    // Apply Auto White Balance to correct color casts before tone mapping
    if (!ctx.metadata.count("awb_gains")) {
        float softness = isNight ? awbSoftnessNight : awbSoftnessNormal;
        applyWhiteBalance(ctx.colorImage, softness);
    } else {
        LOGI("ToneMapStage: Bypassing Grey-World AWB because dynamic sensor AWB gains are active.");
    }

    // Save intermediate debug frames (After White Balance / Neutralized)
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            // Create a gamma-corrected copy for debug preview visualization
            std::vector<uint8_t> previewRgb(ctx.colorImage.data.size());
            for (size_t i = 0; i < previewRgb.size(); ++i) {
                float norm = ctx.colorImage.data[i] / 255.f;
                previewRgb[i] = static_cast<uint8_t>(std::clamp(std::sqrt(norm) * 255.f, 0.f, 255.f));
            }
            saveRgbAsJpeg(
                previewRgb.data(), w, h,
                debugDir + "/stage_3_tonemap/after_white_balance.jpg");
        } catch (...) {}
    }

    ctx.processedImage.resize(w, h);

    // ── GPU HEADLESS COMPUTE SHADER TONE MAPPING ───────────────────────────
    bool success = false;
    std::string errorLog;

    EglHeadlessSetup egl;
    if (egl.init(errorLog)) {
        const char* COMPUTE_NLM_SRC = BuiltInShaders::NLM_DENOISE.c_str();
        const char* COMPUTE_TONEMAP_SRC = BuiltInShaders::ADAPTIVE_TONEMAP.c_str();
        GLuint nlmProgram = 0;
        GLuint denoisedTexture = 0;
        
        if (nlmStrengthMultiplier > 0.0f) {
            nlmProgram = createComputeProgram(COMPUTE_NLM_SRC, errorLog);
            if (nlmProgram == 0) {
                LOGE("ToneMapStage: COMPUTE_NLM_SRC compilation failed! Errors:\n%s", errorLog.c_str());
            }
        }

        GLuint program = createComputeProgram(COMPUTE_TONEMAP_SRC, errorLog);
        if (program != 0) {
            // Convert RGB to RGBA on CPU to prevent GL_INVALID_OPERATION mismatch on mobile drivers
            std::vector<uint8_t> rgbaData(static_cast<size_t>(w) * h * 4);
            const uint8_t* srcData = ctx.colorImage.data.data();
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                rgbaData[i * 4 + 0] = srcData[i * 3 + 0];
                rgbaData[i * 4 + 1] = srcData[i * 3 + 1];
                rgbaData[i * 4 + 2] = srcData[i * 3 + 2];
                rgbaData[i * 4 + 3] = 255;
            }

            // Upload input RGB image as Texture
            GLuint rgbTexture;
            glGenTextures(1, &rgbTexture);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, rgbTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            // Clear any preceding GL errors before starting NLM
            while (glGetError() != GL_NO_ERROR);

            GLenum errSetup = GL_NO_ERROR;
            GLenum errDispatch = GL_NO_ERROR;

            // Execute GPU NLM pass if spatial denoising is requested
            if (nlmStrengthMultiplier > 0.0f && nlmProgram != 0) {
                glGenTextures(1, &denoisedTexture);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, denoisedTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glUseProgram(nlmProgram);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, rgbTexture);
                glUniform1i(glGetUniformLocation(nlmProgram, "u_input_texture"), 0);
                glUniform1i(glGetUniformLocation(nlmProgram, "u_width"), w);
                glUniform1i(glGetUniformLocation(nlmProgram, "u_height"), h);
                glUniform1f(glGetUniformLocation(nlmProgram, "u_S"), static_cast<float>(S));
                glUniform1f(glGetUniformLocation(nlmProgram, "u_O"), static_cast<float>(O));
                glUniform1f(glGetUniformLocation(nlmProgram, "u_strength_multiplier"), nlmStrengthMultiplier);

                errSetup = glGetError();

                glBindImageTexture(0, denoisedTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                glDispatchCompute(static_cast<GLuint>((w + 15) / 16), static_cast<GLuint>((h + 15) / 16), 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                errDispatch = glGetError();

                // Write NLM diagnostic logs to file
                if (ctx.metadata.count("debug_dir")) {
                    try {
                        std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
                        std::ofstream out(debugDir + "/stage_3_tonemap/nlm_debug.txt");
                        if (out) {
                            out << "nlmStrengthMultiplier: " << nlmStrengthMultiplier << "\n";
                            out << "nlmProgram: " << nlmProgram << "\n";
                            out << "S: " << S << "\n";
                            out << "O: " << O << "\n";
                            out << "GL Setup Error: " << errSetup << "\n";
                            out << "GL Dispatch Error: " << errDispatch << "\n";
                            out << "errorLog: " << errorLog << "\n";
                            out.close();
                        }
                    } catch (...) {}
                }

                // Unbind the image unit
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

                // Bind denoised texture as input to tone mapping
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, denoisedTexture);
            } else {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, rgbTexture);
            }

            float tonemapExposureBoost = 1.30f;
            if (ctx.metadata.count("tonemap_exposure_boost")) {
                try { tonemapExposureBoost = std::any_cast<float>(ctx.metadata.at("tonemap_exposure_boost")); } catch (...) {}
            }

            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "u_input_texture"), 0);
            glUniform1i(glGetUniformLocation(program, "u_width"), w);
            glUniform1i(glGetUniformLocation(program, "u_height"), h);
            glUniform1f(glGetUniformLocation(program, "u_adaptive_gamma"), adaptiveGamma);
            glUniform1f(glGetUniformLocation(program, "u_black_point_clamp"), blackPointClamp);
            glUniform1f(glGetUniformLocation(program, "u_detail_alpha"), detailAlpha);
            glUniform1f(glGetUniformLocation(program, "u_saturation_boost"), saturationBoost);
            glUniform1f(glGetUniformLocation(program, "u_ev_compensation"), appliedEvCompensation);
            glUniform1f(glGetUniformLocation(program, "u_final_gamma"), finalGamma);
            glUniform1f(glGetUniformLocation(program, "u_chroma_range_sigma"), chromaRangeSigma);
            glUniform1f(glGetUniformLocation(program, "u_effective_iso"), effectiveIso);
            glUniform1f(glGetUniformLocation(program, "u_exposure_boost"), tonemapExposureBoost);
            glUniformMatrix3fv(glGetUniformLocation(program, "u_ccm"), 1, GL_TRUE, ccm.data());

            // Create output SSBO for packed RGB
            GLuint outBuffer;
            glGenBuffers(1, &outBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);

            // Dispatch tone map compute shader
            glDispatchCompute(static_cast<GLuint>((w + 15) / 16), static_cast<GLuint>((h + 15) / 16), 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

            // Save intermediate pre-denoised tonemapped image
            if (ctx.metadata.count("debug_dir")) {
                try {
                    std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
                    void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), GL_MAP_READ_BIT);
                    if (ptr != nullptr) {
                        std::vector<uint8_t> tempBuffer(w * h * 3);
                        const uint32_t* src = static_cast<const uint32_t*>(ptr);
                        for (int i = 0; i < w * h; ++i) {
                            uint32_t val = src[i];
                            tempBuffer[i * 3 + 0] = val & 0xFF;
                            tempBuffer[i * 3 + 1] = (val >> 8) & 0xFF;
                            tempBuffer[i * 3 + 2] = (val >> 16) & 0xFF;
                        }
                        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                        saveRgbAsJpeg(tempBuffer.data(), w, h, debugDir + "/stage_3_tonemap/tonemapped.jpg");
                    }
                } catch (...) {}
            }

            // ── GPU POST-TONEMAP LUMA+CHROMA NLM PASS ──────────────────────────────────
            const char* COMPUTE_DENOISE_SRC = BuiltInShaders::POST_TONEMAP_DENOISE.c_str();

            // Extract pass configs from metadata
            bool pass1Enabled = true;
            float pass1Strength = 1.0f;
            bool pass2Enabled = true;
            float pass2Strength = 0.5f;
            if (ctx.metadata.count("denoise_pass1_enabled")) {
                try { pass1Enabled = std::any_cast<bool>(ctx.metadata.at("denoise_pass1_enabled")); } catch (...) {}
            }
            if (ctx.metadata.count("denoise_pass1_strength")) {
                try { pass1Strength = std::any_cast<float>(ctx.metadata.at("denoise_pass1_strength")); } catch (...) {}
            }
            if (ctx.metadata.count("denoise_pass2_enabled")) {
                try { pass2Enabled = std::any_cast<bool>(ctx.metadata.at("denoise_pass2_enabled")); } catch (...) {}
            }
            if (ctx.metadata.count("denoise_pass2_strength")) {
                try { pass2Strength = std::any_cast<float>(ctx.metadata.at("denoise_pass2_strength")); } catch (...) {}
            }

            GLuint denoiseProgram = createComputeProgram(COMPUTE_DENOISE_SRC, errorLog);
            if (denoiseProgram != 0) {
                // Create buffer for pass-1 output
                GLuint denoiseOutBuffer;
                glGenBuffers(1, &denoiseOutBuffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, denoiseOutBuffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);

                // Run Pass 1 if enabled
                if (pass1Enabled) {
                    glUseProgram(denoiseProgram);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, denoiseOutBuffer);

                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_width"), w);
                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_height"), h);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_spatial_sigma"), spatialSigma);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_range_sigma"), chromaRangeSigma);
                    int denoiseRadius = (effectiveIso >= 1600.f) ? 3 : 2;
                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_denoise_radius"), denoiseRadius);

                    float activeStrength = (spatialDenoiseStrength > 0) ? ((spatialDenoiseStrength / 8.0f) * 16.0f) : 16.0f;
                    // Apply pass-1 strength multiplier
                    float postTonemapH2 = static_cast<float>((S * 0.5 + O) * activeStrength * pass1Strength * 65025.0);
                    if (postTonemapH2 < 1e-5f) postTonemapH2 = 1e-5f;
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_luma_h2"), postTonemapH2);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_S"), static_cast<float>(S));
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_O"), static_cast<float>(O));
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_strength_multiplier"), activeStrength * pass1Strength);

                    glDispatchCompute(static_cast<GLuint>((w + 15) / 16), static_cast<GLuint>((h + 15) / 16), 1);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                } else {
                    // Copy outBuffer straight to denoiseOutBuffer (bypass pass 1)
                    glBindBuffer(GL_COPY_READ_BUFFER, outBuffer);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, denoiseOutBuffer);
                    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t));
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                }

                // Save pass-1 output debug image
                if (ctx.metadata.count("debug_dir")) {
                    const std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
                    try {
                        glBindBuffer(GL_SHADER_STORAGE_BUFFER, denoiseOutBuffer);
                        void* p1 = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), GL_MAP_READ_BIT);
                        if (p1 != nullptr) {
                            std::vector<uint8_t> p1Rgb(w * h * 3);
                            const uint32_t* ps = static_cast<const uint32_t*>(p1);
                            for (int i = 0; i < w * h; ++i) {
                                uint32_t val = ps[i];
                                p1Rgb[i * 3 + 0] = val & 0xFF;
                                p1Rgb[i * 3 + 1] = (val >> 8) & 0xFF;
                                p1Rgb[i * 3 + 2] = (val >> 16) & 0xFF;
                            }
                            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                            saveRgbAsJpeg(p1Rgb.data(), w, h, debugDir + "/stage_3_tonemap/denoise_pass1.jpg");
                        }
                    } catch (...) {}
                }

                // Create buffer for pass-2 output
                GLuint denoiseOut2Buffer;
                glGenBuffers(1, &denoiseOut2Buffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, denoiseOut2Buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);

                // Run Pass 2 if enabled
                if (pass2Enabled) {
                    glUseProgram(denoiseProgram);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, denoiseOutBuffer);  // pass-1 output -> input
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, denoiseOut2Buffer); // pass-2 output

                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_width"), w);
                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_height"), h);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_spatial_sigma"), spatialSigma);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_range_sigma"), chromaRangeSigma);
                    int denoiseRadius = (effectiveIso >= 1600.f) ? 3 : 2;
                    glUniform1i(glGetUniformLocation(denoiseProgram, "u_denoise_radius"), denoiseRadius);

                    float activeStrength = (spatialDenoiseStrength > 0) ? ((spatialDenoiseStrength / 8.0f) * 16.0f) : 16.0f;
                    // Apply pass-2 strength multiplier
                    float postTonemapH2_pass2 = static_cast<float>((S * 0.5 + O) * activeStrength * pass2Strength * 65025.0);
                    if (postTonemapH2_pass2 < 1e-5f) postTonemapH2_pass2 = 1e-5f;
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_luma_h2"), postTonemapH2_pass2);
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_S"), static_cast<float>(S));
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_O"), static_cast<float>(O));
                    glUniform1f(glGetUniformLocation(denoiseProgram, "u_strength_multiplier"), activeStrength * pass2Strength);

                    glDispatchCompute(static_cast<GLuint>((w + 15) / 16), static_cast<GLuint>((h + 15) / 16), 1);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                } else {
                    // Copy denoiseOutBuffer straight to denoiseOut2Buffer (bypass pass 2)
                    glBindBuffer(GL_COPY_READ_BUFFER, denoiseOutBuffer);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, denoiseOut2Buffer);
                    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t));
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                }

                // Read back final output (from denoiseOut2Buffer) and write to processedImage
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, denoiseOut2Buffer);
                void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(w) * h * sizeof(uint32_t), GL_MAP_READ_BIT);
                if (ptr != nullptr) {
                    uint8_t* dst = ctx.processedImage.data.data();
                    const uint32_t* src = static_cast<const uint32_t*>(ptr);
                    for (int i = 0; i < w * h; ++i) {
                        uint32_t val = src[i];
                        dst[i * 3 + 0] = val & 0xFF;
                        dst[i * 3 + 1] = (val >> 8) & 0xFF;
                        dst[i * 3 + 2] = (val >> 16) & 0xFF;
                    }
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                    success = true;
                    if (ctx.metadata.count("debug_dir")) {
                        try {
                            const std::string debugDir2 = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
                            saveRgbAsJpeg(ctx.processedImage.data.data(), w, h,
                                debugDir2 + "/stage_3_tonemap/denoise_pass2.jpg");
                        } catch (...) {}
                    }
                } else {
                    errorLog += "GL: failed to map denoise final output buffer\n";
                }

                glDeleteBuffers(1, &denoiseOut2Buffer);
                glDeleteBuffers(1, &denoiseOutBuffer);
                glDeleteProgram(denoiseProgram);
            }

            if (denoisedTexture != 0) {
                glDeleteTextures(1, &denoisedTexture);
            }
            if (nlmProgram != 0) {
                glDeleteProgram(nlmProgram);
            }

            glDeleteBuffers(1, &outBuffer);
            glDeleteTextures(1, &rgbTexture);
            glDeleteProgram(program);
        }
    }

    if (!success) {
        LOGE("GL Tone Mapping failed, falling back to CPU multi-threaded Bilateral Grid. GL Errors:\n%s", errorLog.c_str());
        if (ctx.metadata.count("debug_dir")) {
            try {
                std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
                std::ofstream errOut(debugDir + "/tonemap_gl_error.txt");
                if (errOut) {
                    errOut << errorLog;
                    errOut.close();
                }
            } catch (...) {}
        }
        // CPU fallback: run spatial NLM luma denoiser if requested
        if (nlmStrengthMultiplier > 0.0f) {
            RgbImage denoisedImage;
            denoisedImage.resize(w, h);

            int numThreads = 8;
            int rowsPerThread = (h + numThreads - 1) / numThreads;
            std::vector<std::future<void>> futures;

            for (int t = 0; t < numThreads; ++t) {
                int rStart = t * rowsPerThread;
                int rEnd = std::min(rStart + rowsPerThread, h);
                if (rStart >= h) break;

                futures.push_back(std::async(std::launch::async, [rStart, rEnd, w, h, S, O, nlmStrengthMultiplier, &ctx, &denoisedImage]() {
                    for (int r = rStart; r < rEnd; ++r) {
                        for (int c = 0; c < w; ++c) {
                            float sumW = 0.0f;
                            float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
                            
                            float centerL = luma(
                                ctx.colorImage.rowPtr(r)[c*3],
                                ctx.colorImage.rowPtr(r)[c*3+1],
                                ctx.colorImage.rowPtr(r)[c*3+2]
                            ) / 255.0f;

                            // Parabolic boost curve to strengthen NLM in both shadows and highlights (scaled for CPU 0-255 RGB values)
                            float boost = 1.5f - 2.0f * centerL * (1.0f - centerL);
                            float h2 = (S * centerL + O) * nlmStrengthMultiplier * 65025.0f * boost;
                            if (h2 < 1e-6f) h2 = 1e-6f;

                            for (int dy = -2; dy <= 2; ++dy) {
                                for (int dx = -2; dx <= 2; ++dx) {
                                    int nr = std::clamp(r + dy, 0, h - 1);
                                    int nc = std::clamp(c + dx, 0, w - 1);

                                    float ssd = 0.0f;
                                    for (int py = -1; py <= 1; ++py) {
                                        for (int px = -1; px <= 1; ++px) {
                                            int p1r = std::clamp(r + py, 0, h - 1);
                                            int p1c = std::clamp(c + px, 0, w - 1);
                                            int p2r = std::clamp(nr + py, 0, h - 1);
                                            int p2c = std::clamp(nc + px, 0, w - 1);

                                            float l1 = luma(
                                                ctx.colorImage.rowPtr(p1r)[p1c*3],
                                                ctx.colorImage.rowPtr(p1r)[p1c*3+1],
                                                ctx.colorImage.rowPtr(p1r)[p1c*3+2]
                                            );
                                            float l2 = luma(
                                                ctx.colorImage.rowPtr(p2r)[p2c*3],
                                                ctx.colorImage.rowPtr(p2r)[p2c*3+1],
                                                ctx.colorImage.rowPtr(p2r)[p2c*3+2]
                                            );
                                            float diff = l1 - l2;
                                            ssd += diff * diff;
                                        }
                                    }

                                    // Spatial-bilateral falloff to preserve local contrast/brightness
                                    float wVal = std::exp(-(ssd / 9.0f) / h2) * std::exp(-static_cast<float>(dx*dx + dy*dy) / 12.5f);
                                    uint8_t nrVal = ctx.colorImage.rowPtr(nr)[nc*3];
                                    uint8_t ngVal = ctx.colorImage.rowPtr(nr)[nc*3+1];
                                    uint8_t nbVal = ctx.colorImage.rowPtr(nr)[nc*3+2];

                                    sumR += wVal * nrVal;
                                    sumG += wVal * ngVal;
                                    sumB += wVal * nbVal;
                                    sumW += wVal;
                                }
                            }

                            denoisedImage.rowPtr(r)[c*3]   = std::clamp(sumR / sumW, 0.0f, 255.0f);
                            denoisedImage.rowPtr(r)[c*3+1] = std::clamp(sumG / sumW, 0.0f, 255.0f);
                            denoisedImage.rowPtr(r)[c*3+2] = std::clamp(sumB / sumW, 0.0f, 255.0f);
                        }
                    }
                }));
            }

            for (auto& fut : futures) {
                fut.wait();
            }
            ctx.colorImage = std::move(denoisedImage);
        }

        // CPU fallback: build bilateral grid in log-domain
        BilateralGrid grid;
        grid.init(w, h);

        // Splat only 1 in 16 pixels
        for (int r = 0; r < h; r += 4) {
            const uint8_t* row = ctx.colorImage.rowPtr(r);
            for (int c = 0; c < w; c += 4) {
                float L = luma(row[c*3], row[c*3+1], row[c*3+2]);
                float logL = std::log2f(L + 1.f);
                grid.splat(c, r, logL * (255.f / 8.f));
            }
        }

        // Blur grid
        grid.blur();
        grid.normalize();

        // Sample + reconstruct in parallel in log-domain
        int numThreads = 8;
        int rowsPerThread = h / numThreads;
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);

        for (int t = 0; t < numThreads; ++t) {
            int rStart = t * rowsPerThread;
            int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

            futures.push_back(std::async(std::launch::async, [&ctx, &grid, rStart, rEnd, w, h, adaptiveGamma, isNight, blackPointClamp, detailAlpha, saturationBoost, appliedEvCompensation, ccm, finalGamma, effectiveIso]() {
                // Dynamically compensate exposure bias (scale = 2^(-EV))
                float evScale = std::powf(2.0f, -appliedEvCompensation);
                for (int r = rStart; r < rEnd; ++r) {
                    const uint8_t* src = ctx.colorImage.rowPtr(r);
                    uint8_t*       dst = ctx.processedImage.rowPtr(r);

                    for (int c = 0; c < w; ++c) {
                        float R = src[c*3] * evScale;
                        float G = src[c*3+1] * evScale;
                        float B = src[c*3+2] * evScale;
                        
                        float L = luma(R, G, B);
                        float logL = std::log2f(L + 1.f);

                        // Retrieve base illumination (local low-frequency contrast)
                        float logBase = grid.sample(c, r, logL * (255.f / 8.f)) * (8.f / 255.f);
                        logBase = std::clamp(logBase, 0.f, 10.f);
                        float baseL = std::exp2f(logBase) - 1.f;
                        baseL = std::max(1.f, baseL);

                        // Compress base layer (dynamic range compression)
                        float normBase = baseL / 255.f;
                        float boostedBase = std::powf(normBase, adaptiveGamma);
                        
                        // Soft black-point compression
                        if (boostedBase < blackPointClamp) {
                            boostedBase = (boostedBase * boostedBase) / blackPointClamp;
                        }
                        
                        float compBase = acesFilm(boostedBase) * 255.f;

                        // Suppress detail boost in dark areas to prevent noise/grain amplification
                        float currentDetailAlpha = detailAlpha;
                        if (baseL < 80.f) {
                            float shadowFactor = baseL / 80.f;
                            float minDetail = 1.0f;
                            if (effectiveIso >= 1600.f) {
                                minDetail = 0.3f;
                            } else if (effectiveIso >= 800.f) {
                                minDetail = 0.5f;
                            } else if (effectiveIso >= 400.f) {
                                minDetail = 0.7f;
                            }
                            currentDetailAlpha = minDetail + shadowFactor * (detailAlpha - minDetail);
                        }

                        // Add log details back (Log detail = logL - logBase)
                        float logDetail = logL - logBase;
                        float compLogL = std::log2f(compBase + 1.f) + logDetail * currentDetailAlpha;
                        float compL = std::exp2f(compLogL) - 1.f;
                        compL = std::clamp(compL, 0.f, 65535.f);

                        // Scale RGB channels to match the local tone mapped luminance
                        float scale = (L > 0.1f) ? compL / L : 1.f;
                        scale = std::min(scale, 10.0f);
                        
                        float sR = R * scale;
                        float sG = G * scale;
                        float sB = B * scale;

                        // Apply Color Correction Matrix (CCM) to map sensor RGB to sRGB
                        float oR = ccm[0] * sR + ccm[1] * sG + ccm[2] * sB;
                        float oG = ccm[3] * sR + ccm[4] * sG + ccm[5] * sB;
                        float oB = ccm[6] * sR + ccm[7] * sG + ccm[8] * sB;

                        // Apply desaturation / saturation boost in sRGB space
                        float newL = luma(oR, oG, oB);
                        if (newL > 0.1f) {
                            float factor = saturationBoost;
                            if (newL < 30.f) {
                                // Linear ramp down to 0 saturation as luma goes from 30 down to 0
                                factor = factor * (newL / 30.f);
                            } else if (newL > 200.f) {
                                float t = (newL - 200.f) / (255.f - 200.f);
                                factor = factor * (1.f - std::clamp(t, 0.f, 1.f));
                            }
                            oR = newL + factor * (oR - newL);
                            oG = newL + factor * (oG - newL);
                            oB = newL + factor * (oB - newL);
                        }

                        // Apply skin tone adjustments to tone down yellowish skin in CPU fallback
                        float uVal = -0.1687f * oR - 0.3313f * oG + 0.5f * oB;
                        float vVal = 0.5f * oR - 0.4187f * oG - 0.0813f * oB;
                        if (vVal > 0.02f && uVal > -0.15f && uVal < -0.01f) {
                            vVal *= 0.88f;
                            uVal *= 0.95f;
                            oR = newL + 1.402f * vVal;
                            oG = newL - 0.34414f * uVal - 0.71414f * vVal;
                            oB = newL + 1.772f * uVal;
                        }

                        // Soft highlight roll-off
                        float maxChan = std::max({oR, oG, oB});
                        if (maxChan > 255.f) {
                            float blendL = luma(oR, oG, oB);
                            if (maxChan - blendL > 1e-4f) {
                                float blend = (255.f - blendL) / (maxChan - blendL);
                                blend = std::clamp(blend, 0.f, 1.f);
                                oR = blendL + blend * (oR - blendL);
                                oG = blendL + blend * (oG - blendL);
                                oB = blendL + blend * (oB - blendL);
                            }
                        }

                        // Apply an ISO-adaptive custom gamma correction to balance brightness
                        float gammaR = std::powf(std::clamp(oR / 255.f, 0.f, 1.f), 1.f / finalGamma) * 255.f;
                        float gammaG = std::powf(std::clamp(oG / 255.f, 0.f, 1.f), 1.f / finalGamma) * 255.f;
                        float gammaB = std::powf(std::clamp(oB / 255.f, 0.f, 1.f), 1.f / finalGamma) * 255.f;

                        dst[c*3]   = static_cast<uint8_t>(std::clamp(gammaR, 0.f, 255.f));
                        dst[c*3+1] = static_cast<uint8_t>(std::clamp(gammaG, 0.f, 255.f));
                        dst[c*3+2] = static_cast<uint8_t>(std::clamp(gammaB, 0.f, 255.f));
                    }
                }
            }));
        }

        for (auto& fut : futures) {
            fut.get();
        }

        // Multi-threaded CPU Bilateral Denoise Pass
        std::vector<uint8_t> denoised(w * h * 3);
        float spatial_sigma2 = 2.0f * spatialSigma * spatialSigma;
        float range_sigma2 = 2.0f * chromaRangeSigma * chromaRangeSigma;
        int radius = (effectiveIso >= 1600.f) ? 3 : 2;

        std::vector<std::future<void>> denoiseFutures;
        for (int t = 0; t < numThreads; ++t) {
            int rStart = t * rowsPerThread;
            int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

            denoiseFutures.push_back(std::async(std::launch::async, [&ctx, &denoised, rStart, rEnd, w, h, spatial_sigma2, range_sigma2, radius]() {
                for (int r = rStart; r < rEnd; ++r) {
                    for (int c = 0; c < w; ++c) {
                        float sumU = 0.f, sumV = 0.f, sumW = 0.f;
                        float centerR = ctx.processedImage.data[(r * w + c) * 3 + 0];
                        float centerG = ctx.processedImage.data[(r * w + c) * 3 + 1];
                        float centerB = ctx.processedImage.data[(r * w + c) * 3 + 2];
                        float centerL = 0.299f * centerR + 0.587f * centerG + 0.114f * centerB;

                        for (int dy = -radius; dy <= radius; ++dy) {
                            int nr = std::clamp(r + dy, 0, h - 1);
                            for (int dx = -radius; dx <= radius; ++dx) {
                                int nc = std::clamp(c + dx, 0, w - 1);
                                
                                float nR = ctx.processedImage.data[(nr * w + nc) * 3 + 0];
                                float nG = ctx.processedImage.data[(nr * w + nc) * 3 + 1];
                                float nB = ctx.processedImage.data[(nr * w + nc) * 3 + 2];
                                float nL = 0.299f * nR + 0.587f * nG + 0.114f * nB;

                                float dS2 = static_cast<float>(dx * dx + dy * dy);
                                float dR = nL - centerL;
                                float dR2 = dR * dR;

                                float weight = std::expf(-dS2 / spatial_sigma2) * std::expf(-dR2 / range_sigma2);
                                
                                float nU = -0.1687f * nR - 0.3313f * nG + 0.5f * nB;
                                float nV = 0.5f * nR - 0.4187f * nG - 0.0813f * nB;

                                sumU += weight * nU;
                                sumV += weight * nV;
                                sumW += weight;
                            }
                        }

                        float filteredU = sumU / sumW;
                        float filteredV = sumV / sumW;

                        float rVal = centerL + 1.402f * filteredV;
                        float gVal = centerL - 0.34414f * filteredU - 0.71414f * filteredV;
                        float bVal = centerL + 1.772f * filteredU;

                        denoised[(r * w + c) * 3 + 0] = static_cast<uint8_t>(std::clamp(rVal, 0.f, 255.f));
                        denoised[(r * w + c) * 3 + 1] = static_cast<uint8_t>(std::clamp(gVal, 0.f, 255.f));
                        denoised[(r * w + c) * 3 + 2] = static_cast<uint8_t>(std::clamp(bVal, 0.f, 255.f));
                    }
                }
            }));
        }
        for (auto& fut : denoiseFutures) {
            fut.get();
        }
        ctx.processedImage.data = std::move(denoised);
    }

    // Save intermediate tonemapped RGB + JPEG
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            
            // Raw PPM — only written when debug_raw_dumps is enabled
            bool rawDumps = false;
            if (ctx.metadata.count("debug_raw_dumps")) {
                try { rawDumps = std::any_cast<bool>(ctx.metadata.at("debug_raw_dumps")); } catch (...) {}
            }
            if (rawDumps) {
                std::string ppmPath = debugDir + "/stage_3_tonemap/tonemapped.ppm";
                std::ofstream out(ppmPath, std::ios::binary);
                if (out) {
                    out << "P6\n" << w << " " << h << "\n255\n";
                    out.write(reinterpret_cast<const char*>(ctx.processedImage.data.data()), ctx.processedImage.data.size());
                    out.close();
                }
            }
            
            // Saved final output matching name request
            saveRgbAsJpeg(
                ctx.processedImage.data.data(), w, h,
                debugDir + "/stage_3_tonemap/final_output.jpg");
        } catch (...) {}
    }

    LOGI("ToneMapStage: tone-mapped %d×%d (meanL=%.1f, gamma=%.2f)",
         w, h, meanL, adaptiveGamma);
    return true;
}
