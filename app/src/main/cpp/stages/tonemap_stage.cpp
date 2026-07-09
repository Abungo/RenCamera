#include "tonemap_stage.h"
#include "../debug_utils.h"
#include "../gl_helpers.h"

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
    float gammaMin = isNight ? 0.55f : 0.65f;
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

    float blackPointClamp = 0.02f;
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

    // ── GPU HEADLESS COMPUTE SHADER TONE MAPPING ───────────────────────────
    bool success = false;
    std::string errorLog;

    EglHeadlessSetup egl;
    if (egl.init(errorLog)) {
        const char* COMPUTE_TONEMAP_SRC = R"glsl(
            #version 310 es
            layout(local_size_x = 16, local_size_y = 16) in;

            precision highp float;
            precision highp sampler2D;

            uniform sampler2D u_input_texture;
            uniform int u_width;
            uniform int u_height;
            uniform float u_adaptive_gamma;
            uniform float u_black_point_clamp;
            uniform float u_detail_alpha;
            uniform float u_saturation_boost;

            layout(std430, binding = 0) writeonly buffer OutputBuffer {
                uint outRGB[];
            };

            float luma(vec3 rgb) {
                return 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
            }

            float acesFilm(float x) {
                float a = 2.51;
                float b = 0.03;
                float c = 2.43;
                float d = 0.59;
                float e = 0.14;
                return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
            }

            void main() {
                ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
                if (pos.x >= u_width || pos.y >= u_height) return;

                vec3 rgbVal = texelFetch(u_input_texture, pos, 0).rgb * 255.0;
                float L = luma(rgbVal);

                // Single-pass bilateral filter on the GPU to extract local low-frequency base layer
                float sumVal = 0.0;
                float sumW = 0.0;
                
                float spatial_sigma2 = 2.0 * 16.0 * 16.0;
                float range_sigma2 = 2.0 * 38.25 * 38.25;

                int radius = 12;
                for (int dy = -radius; dy <= radius; dy += 2) {
                    for (int dx = -radius; dx <= radius; dx += 2) {
                        ivec2 nPos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
                        vec3 nRgb = texelFetch(u_input_texture, nPos, 0).rgb * 255.0;
                        float nL = luma(nRgb);

                        float dS2 = float(dx * dx + dy * dy);
                        float dR = nL - L;
                        float dR2 = dR * dR;

                        float w = exp(-dS2 / spatial_sigma2) * exp(-dR2 / range_sigma2);
                        sumVal += w * nL;
                        sumW += w;
                    }
                }
                float baseL = max(1.0, sumVal / sumW);
                
                float logL = log2(L + 1.0);
                float logBase = log2(baseL + 1.0);
                logBase = clamp(logBase, 0.0, 8.0);
                
                float normBase = (pow(2.0, logBase) - 1.0) / 255.0;
                float boostedBase = pow(normBase, u_adaptive_gamma);
                if (boostedBase < u_black_point_clamp) {
                    boostedBase = (boostedBase * boostedBase) / u_black_point_clamp;
                }
                float compBase = acesFilm(boostedBase) * 255.0;

                float currentDetailAlpha = u_detail_alpha;
                if (baseL < 50.0) {
                    float factor = baseL / 50.0;
                    currentDetailAlpha = 1.0 + factor * (u_detail_alpha - 1.0);
                }

                float logDetail = logL - logBase;
                float compLogL = log2(compBase + 1.0) + logDetail * currentDetailAlpha;
                float compL = clamp(pow(2.0, compLogL) - 1.0, 0.0, 255.0);

                float scale = (L > 0.1) ? compL / L : 1.0;
                scale = min(scale, 10.0);

                vec3 oRgb = rgbVal * scale;
                float newL = luma(oRgb);

                if (newL > 0.1) {
                    float factor = u_saturation_boost;
                    if (newL > 200.0) {
                        float t = (newL - 200.0) / (255.0 - 200.0);
                        factor = factor * (1.0 - clamp(t, 0.0, 1.0));
                    }
                    oRgb = newL + factor * (oRgb - newL);
                }

                float maxChan = max(oRgb.r, max(oRgb.g, oRgb.b));
                if (maxChan > 255.0) {
                    float blendL = luma(oRgb);
                    if (maxChan - blendL > 1e-4) {
                        float blend = clamp((255.0 - blendL) / (maxChan - blendL), 0.0, 1.0);
                        oRgb = blendL + blend * (oRgb - blendL);
                    }
                }

                uint uR = uint(clamp(oRgb.r, 0.0, 255.0));
                uint uG = uint(clamp(oRgb.g, 0.0, 255.0));
                uint uB = uint(clamp(oRgb.b, 0.0, 255.0));

                uint packedVal = uR | (uG << 8) | (uB << 16) | (255u << 24);
                outRGB[pos.y * u_width + pos.x] = packedVal;
            }
        )glsl";

        GLuint program = createComputeProgram(COMPUTE_TONEMAP_SRC, errorLog);
        if (program != 0) {
            glUseProgram(program);

            // Upload input RGB image as Texture
            GLuint rgbTexture;
            glGenTextures(1, &rgbTexture);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, rgbTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, w, h);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, ctx.colorImage.data.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glUniform1i(glGetUniformLocation(program, "u_input_texture"), 0);
            glUniform1i(glGetUniformLocation(program, "u_width"), w);
            glUniform1i(glGetUniformLocation(program, "u_height"), h);
            glUniform1f(glGetUniformLocation(program, "u_adaptive_gamma"), adaptiveGamma);
            glUniform1f(glGetUniformLocation(program, "u_black_point_clamp"), blackPointClamp);
            glUniform1f(glGetUniformLocation(program, "u_detail_alpha"), detailAlpha);
            glUniform1f(glGetUniformLocation(program, "u_saturation_boost"), saturationBoost);

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
            } else {
                errorLog += "GL: failed to map tonemap output buffer\n";
            }

            glDeleteBuffers(1, &outBuffer);
            glDeleteTextures(1, &rgbTexture);
            glDeleteProgram(program);
        }
    }

    if (!success) {
        LOGE("GL Tone Mapping failed, falling back to CPU multi-threaded Bilateral Grid. GL Errors:\n%s", errorLog.c_str());
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

            futures.push_back(std::async(std::launch::async, [&ctx, &grid, rStart, rEnd, w, h, adaptiveGamma, isNight, blackPointClamp, detailAlpha, saturationBoost]() {
                for (int r = rStart; r < rEnd; ++r) {
                    const uint8_t* src = ctx.colorImage.rowPtr(r);
                    uint8_t*       dst = ctx.processedImage.rowPtr(r);

                    for (int c = 0; c < w; ++c) {
                        float R = src[c*3];
                        float G = src[c*3+1];
                        float B = src[c*3+2];
                        
                        float L = luma(R, G, B);
                        float logL = std::log2f(L + 1.f);

                        // Retrieve base illumination (local low-frequency contrast)
                        float gridVal = grid.sample(c, r, logL * (255.f / 8.f));
                        float logBase = gridVal * (8.f / 255.f);
                        logBase = std::clamp(logBase, 0.f, 8.f);
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
                        if (baseL < 50.f) {
                            float factor = baseL / 50.f;
                            currentDetailAlpha = 1.0f + factor * (detailAlpha - 1.0f);
                        }

                        // Add log details back (Log detail = logL - logBase)
                        float logDetail = logL - logBase;
                        float compLogL = std::log2f(compBase + 1.f) + logDetail * currentDetailAlpha;
                        float compL = std::exp2f(compLogL) - 1.f;
                        compL = std::clamp(compL, 0.f, 65535.f);

                        // Scale RGB channels to match the local tone mapped luminance
                        float scale = (L > 0.1f) ? compL / L : 1.f;
                        scale = std::min(scale, 10.0f);
                        
                        float oR = R * scale;
                        float oG = G * scale;
                        float oB = B * scale;

                        // Apply desaturation / saturation boost
                        float newL = luma(oR, oG, oB);
                        if (newL > 0.1f) {
                            float factor = saturationBoost;
                            if (newL > 200.f) {
                                float t = (newL - 200.f) / (255.f - 200.f);
                                factor = factor * (1.f - std::clamp(t, 0.f, 1.f));
                            }
                            oR = newL + factor * (oR - newL);
                            oG = newL + factor * (oG - newL);
                            oB = newL + factor * (oB - newL);
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

                        dst[c*3]   = static_cast<uint8_t>(std::clamp(oR, 0.f, 255.f));
                        dst[c*3+1] = static_cast<uint8_t>(std::clamp(oG, 0.f, 255.f));
                        dst[c*3+2] = static_cast<uint8_t>(std::clamp(oB, 0.f, 255.f));
                    }
                }
            }));
        }

        for (auto& fut : futures) {
            fut.get();
        }
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
            
            // JPEG previews
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
