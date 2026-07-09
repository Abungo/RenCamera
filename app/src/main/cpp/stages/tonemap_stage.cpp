#include "tonemap_stage.h"
#include "../debug_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <fstream>
#include <any>

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
                std::vector<float> tmp(INTENSITY_BINS), tmpW(INTENSITY_BINS);
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

        // Trilinear interpolation of (sum / weight)
        auto val = [&](int gya, int gxa, int gia) -> float {
            float s = splatSum[idx(gya, gxa, gia)];
            float w = splatWgt[idx(gya, gxa, gia)];
            return (w > 1e-6f) ? s / w : L; // fallback: identity
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

    // Calculate scene key (average luminance)
    double lumaSum = 0;
    uint64_t lumaCount = 0;
    for (int r = 0; r < h; r += 8) {
        const uint8_t* row = ctx.colorImage.rowPtr(r);
        for (int c = 0; c < w; c += 8) {
            lumaSum += luma(row[c*3], row[c*3+1], row[c*3+2]);
            lumaCount++;
        }
    }
    float meanL = (lumaCount > 0) ? static_cast<float>(lumaSum / lumaCount) : 10.f;

    bool isNight = false;
    if (ctx.metadata.count("night_mode")) {
        try {
            isNight = std::any_cast<bool>(ctx.metadata.at("night_mode"));
        } catch (...) {}
    }

    // Adapt gamma based on actual scene brightness
    // Darker scenes get a stronger shadow boost, brighter scenes stay natural.
    float gammaMin = isNight ? 0.40f : 0.55f;
    float gammaMax = isNight ? 0.80f : 0.95f;

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

    float detailAlpha = 1.15f;
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

    float blackPointClamp = 0.08f;
    if (ctx.metadata.count("black_point_clamp")) {
        try {
            blackPointClamp = std::any_cast<float>(ctx.metadata.at("black_point_clamp"));
        } catch (...) {}
    }

    // Save intermediate debug frames (Before White Balance / Debayered RGB)
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            saveRgbAsJpeg(
                ctx.colorImage.data.data(), w, h,
                debugDir + "/stage_3_tonemap/before_white_balance.jpg");
        } catch (...) {}
    }

    // Apply Auto White Balance to correct color casts before tone mapping
    float softness = isNight ? awbSoftnessNight : awbSoftnessNormal;
    applyWhiteBalance(ctx.colorImage, softness);

    // Save intermediate debug frames (After White Balance / Neutralized)
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            saveRgbAsJpeg(
                ctx.colorImage.data.data(), w, h,
                debugDir + "/stage_3_tonemap/after_white_balance.jpg");
        } catch (...) {}
    }

    ctx.processedImage.resize(w, h);

    // ── 1. Build bilateral grid ───────────────────────────────────────────────
    BilateralGrid grid;
    grid.init(w, h);

    for (int r = 0; r < h; ++r) {
        const uint8_t* row = ctx.colorImage.rowPtr(r);
        for (int c = 0; c < w; ++c) {
            float L = luma(row[c*3], row[c*3+1], row[c*3+2]);
            grid.splat(c, r, L);
        }
    }

    // ── 2. Blur grid ─────────────────────────────────────────────────────────
    grid.blur();

    // ── 3. Sample + reconstruct ───────────────────────────────────────────────
    for (int r = 0; r < h; ++r) {
        const uint8_t* src = ctx.colorImage.rowPtr(r);
        uint8_t*       dst = ctx.processedImage.rowPtr(r);

        for (int c = 0; c < w; ++c) {
            float L = luma(src[c*3], src[c*3+1], src[c*3+2]);

            float baseL = grid.sample(c, r, L);
            baseL = std::max(1.f, baseL);

            // Normalize base luminance to [0, 1] for filmic curve
            float normBase = baseL / 255.f;
            float boostedBase = std::pow(normBase, adaptiveGamma);
            
            // Soft black-point compression for night mode (preserves deep black skies)
            if (isNight && boostedBase < blackPointClamp) {
                boostedBase = (boostedBase * boostedBase) / blackPointClamp;
            }
            
            float compBase = acesFilm(boostedBase) * 255.f;

            // Multiplicative detail boosting (equivalent to log-domain tone mapping)
            float ratio = (L > 0.f) ? L / baseL : 0.f;
            float compL = compBase * std::pow(ratio, detailAlpha);
            compL = std::clamp(compL, 0.f, 255.f);

            // Scale RGB channels to match new luminance (preserves hue)
            float scale = (L > 0.1f) ? compL / L : 1.f;
            scale = std::min(scale, 8.0f); // limit maximum noise amplification
            uint8_t oR = static_cast<uint8_t>(std::clamp(src[c*3]   * scale, 0.f, 255.f));
            uint8_t oG = static_cast<uint8_t>(std::clamp(src[c*3+1] * scale, 0.f, 255.f));
            uint8_t oB = static_cast<uint8_t>(std::clamp(src[c*3+2] * scale, 0.f, 255.f));

            // Saturation boost
            adjustSaturation(oR, oG, oB, saturationBoost);

            dst[c*3] = oR; dst[c*3+1] = oG; dst[c*3+2] = oB;
        }
    }

    // Save intermediate tonemapped RGB + JPEG
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            
            // Raw PPM
            std::string ppmPath = debugDir + "/stage_3_tonemap/tonemapped.ppm";
            std::ofstream out(ppmPath, std::ios::binary);
            if (out) {
                out << "P6\n" << w << " " << h << "\n255\n";
                out.write(reinterpret_cast<const char*>(ctx.processedImage.data.data()), ctx.processedImage.data.size());
                out.close();
            }
            
            // JPEG preview
            saveRgbAsJpeg(
                ctx.processedImage.data.data(), w, h,
                debugDir + "/stage_3_tonemap/tonemapped.jpg");
            
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
