#include "fusion_stage.h"
#include "../debug_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <fstream>
#include <future>

// Highway SIMD for the vectorized accumulation inner loop
#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

namespace {

// Ghosting rejection threshold: pixels with residual > this many luma levels
// receive near-zero weight.  Tuned for YUV_420_888 (0–255 range).
static constexpr float SIGMA = 20.f;
static constexpr float INV_2SIGMA2 = 1.f / (2.f * SIGMA * SIGMA);

/// Bilinearly sample the Y plane of a YuvFrame at sub-pixel (sx, sy).
/// Returns the float luma value clamped to the frame bounds.
static inline float sampleY(const YuvFrame& f, float sx, float sy) {
    int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
    x0 = std::clamp(x0, 0, f.width  - 1);
    y0 = std::clamp(y0, 0, f.height - 1);
    int x1 = std::min(x0 + 1, f.width  - 1);
    int y1 = std::min(y0 + 1, f.height - 1);
    float fx = sx - static_cast<float>(std::max(0, static_cast<int>(sx)));
    float fy = sy - static_cast<float>(std::max(0, static_cast<int>(sy)));

    float p00 = f.yPlane[y0 * f.yRowStride + x0];
    float p10 = f.yPlane[y0 * f.yRowStride + x1];
    float p01 = f.yPlane[y1 * f.yRowStride + x0];
    float p11 = f.yPlane[y1 * f.yRowStride + x1];
    return p00 * (1-fx)*(1-fy) + p10 * fx*(1-fy)
         + p01 * (1-fx)*fy    + p11 * fx*fy;
}

/// Sample U or V chroma plane (half resolution in each dimension).
static inline float sampleUV(const uint8_t* plane,
                              int rowStride, int pixelStride,
                              int planeW, int planeH,
                              float sx, float sy)
{
    int x0 = std::clamp(static_cast<int>(sx), 0, planeW - 1);
    int y0 = std::clamp(static_cast<int>(sy), 0, planeH - 1);
    // No bilinear for chroma — nearest neighbour is fast and close enough
    return static_cast<float>(plane[y0 * rowStride + x0 * pixelStride]);
}

static inline float sampleUVBilinear(const uint8_t* plane,
                                     int rowStride, int pixelStride,
                                     int planeW, int planeH,
                                     float sx, float sy)
{
    sx = std::clamp(sx, 0.f, static_cast<float>(planeW - 1));
    sy = std::clamp(sy, 0.f, static_cast<float>(planeH - 1));

    int x0 = static_cast<int>(sx);
    int y0 = static_cast<int>(sy);
    int x1 = std::min(x0 + 1, planeW - 1);
    int y1 = std::min(y0 + 1, planeH - 1);

    float fx = sx - x0;
    float fy = sy - y0;

    float p00 = plane[y0 * rowStride + x0 * pixelStride];
    float p10 = plane[y0 * rowStride + x1 * pixelStride];
    float p01 = plane[y1 * rowStride + x0 * pixelStride];
    float p11 = plane[y1 * rowStride + x1 * pixelStride];

    return p00 * (1.f - fx) * (1.f - fy) +
           p10 * fx * (1.f - fy) +
           p01 * (1.f - fx) * fy +
           p11 * fx * fy;
}

static void denoiseChroma(std::vector<uint8_t>& plane, int w, int h) {
    std::vector<uint8_t> tmp = plane;
    std::vector<float> rowTmp(w);
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            float sum = 0.f;
            float wSum = 0.f;
            for (int dc = -2; dc <= 2; ++dc) {
                int nc = std::clamp(c + dc, 0, w - 1);
                float weight = (dc == 0) ? 6.f : ((dc == -1 || dc == 1) ? 4.f : 1.f);
                sum += weight * tmp[r * w + nc];
                wSum += weight;
            }
            rowTmp[c] = sum / wSum;
        }
        for (int c = 0; c < w; ++c) {
            tmp[r * w + c] = static_cast<uint8_t>(std::clamp(rowTmp[c], 0.f, 255.f));
        }
    }
    std::vector<float> colTmp(h);
    for (int c = 0; c < w; ++c) {
        for (int r = 0; r < h; ++r) {
            float sum = 0.f;
            float wSum = 0.f;
            for (int dr = -2; dr <= 2; ++dr) {
                int nr = std::clamp(r + dr, 0, h - 1);
                float weight = (dr == 0) ? 6.f : ((dr == -1 || dr == 1) ? 4.f : 1.f);
                sum += weight * tmp[nr * w + c];
                wSum += weight;
            }
            colTmp[r] = sum / wSum;
        }
        for (int r = 0; r < h; ++r) {
            plane[r * w + c] = static_cast<uint8_t>(std::clamp(colTmp[r], 0.f, 255.f));
        }
    }
}

/// Non-Local Means spatial denoising on the luma (Y) plane.
/// template_half = half-size of the patch window (1 = 3x3 patch)
/// search_half   = half-size of the search window (3 = 7x7 search area)
/// h_strength    = filter parameter — higher means more smoothing
static void spatialDenoiseLuma(std::vector<uint8_t>& yPlane, int w, int h,
                                int h_strength, int template_half = 1, int search_half = 3)
{
    if (h_strength <= 0) return;
    const float h2 = static_cast<float>(h_strength * h_strength);
    std::vector<uint8_t> output(yPlane.size());

    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            float weightedSum = 0.f;
            float totalWeight = 0.f;

            for (int sr = r - search_half; sr <= r + search_half; ++sr) {
                for (int sc = c - search_half; sc <= c + search_half; ++sc) {
                    // Compute SSD of patch centred at (r,c) vs (sr,sc)
                    float patchDist = 0.f;
                    int patchCount = 0;
                    for (int tr = -template_half; tr <= template_half; ++tr) {
                        for (int tc = -template_half; tc <= template_half; ++tc) {
                            int r1 = std::clamp(r  + tr, 0, h - 1);
                            int c1 = std::clamp(c  + tc, 0, w - 1);
                            int r2 = std::clamp(sr + tr, 0, h - 1);
                            int c2 = std::clamp(sc + tc, 0, w - 1);
                            float diff = static_cast<float>(yPlane[r1 * w + c1])
                                       - static_cast<float>(yPlane[r2 * w + c2]);
                            patchDist += diff * diff;
                            ++patchCount;
                        }
                    }
                    patchDist /= static_cast<float>(patchCount);

                    float weight = std::exp(-patchDist / h2);
                    int clampedSr = std::clamp(sr, 0, h - 1);
                    int clampedSc = std::clamp(sc, 0, w - 1);
                    weightedSum += weight * static_cast<float>(yPlane[clampedSr * w + clampedSc]);
                    totalWeight += weight;
                }
            }

            output[r * w + c] = static_cast<uint8_t>(
                std::clamp(weightedSum / totalWeight, 0.f, 255.f));
        }
    }
    yPlane = std::move(output);
}

static inline MotionVec interpolateMotion(const MotionField& mf, int r, int c) {
    float bs = static_cast<float>(mf.blockSize);
    float by = (static_cast<float>(r) - bs * 0.5f) / bs;
    float bx = (static_cast<float>(c) - bs * 0.5f) / bs;

    int y0_raw = static_cast<int>(std::floor(by));
    int x0_raw = static_cast<int>(std::floor(bx));
    int y1_raw = y0_raw + 1;
    int x1_raw = x0_raw + 1;

    float fy = by - std::floor(by);
    float fx = bx - std::floor(bx);

    int y0 = std::clamp(y0_raw, 0, mf.blocksTall - 1);
    int x0 = std::clamp(x0_raw, 0, mf.blocksWide - 1);
    int y1 = std::clamp(y1_raw, 0, mf.blocksTall - 1);
    int x1 = std::clamp(x1_raw, 0, mf.blocksWide - 1);

    if (y0 == y1) fy = 0.f;
    if (x0 == x1) fx = 0.f;

    const MotionVec& mv00 = mf.at(y0, x0);
    const MotionVec& mv10 = mf.at(y0, x1);
    const MotionVec& mv01 = mf.at(y1, x0);
    const MotionVec& mv11 = mf.at(y1, x1);

    float dx = (1.f - fy) * ((1.f - fx) * mv00.dx + fx * mv10.dx)
             +        fy  * ((1.f - fx) * mv01.dx + fx * mv11.dx);

    float dy = (1.f - fy) * ((1.f - fx) * mv00.dy + fx * mv10.dy)
             +        fy  * ((1.f - fx) * mv01.dy + fx * mv11.dy);

    return { static_cast<int>(std::round(dx)), static_cast<int>(std::round(dy)) };
}

/// Fuse one luma (Y) plane from frames into a float accumulator buffer,
/// then quantize to uint8.
static void fuseYPlane(const std::vector<YuvFrame>& frames,
                       const std::vector<MotionField>& motionFields,
                       std::vector<uint8_t>& outY,
                       int w, int h,
                       const std::vector<std::vector<float>>& inv_2sigma2_luts)
{
    std::vector<float> acc(static_cast<size_t>(w) * h, 0.f);
    std::vector<float> wgt(static_cast<size_t>(w) * h, 0.f);

    // ── Reference frame: weight 1.0 ──────────────────────────────────────────
    {
        const YuvFrame& ref = frames[0];
        for (int r = 0; r < h; ++r) {
            const uint8_t* row = ref.yPlane + r * ref.yRowStride;
            float*         a   = acc.data() + r * w;
            float*         ww  = wgt.data() + r * w;
            for (int c = 0; c < w; ++c) {
                a[c]  = static_cast<float>(row[c]);
                ww[c] = 1.f;
            }
        }
    }

    // ── Remaining frames: warp + weighted accumulate in parallel ──────────────
    int numThreads = 8;
    int rowsPerThread = h / numThreads;

    for (size_t fi = 1; fi < frames.size(); ++fi) {
        const YuvFrame&   src = frames[fi];
        const MotionField& mf = motionFields[fi - 1];
        const std::vector<float>& inv_2sigma2_lut = inv_2sigma2_luts[fi];

        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);

        for (int t = 0; t < numThreads; ++t) {
            int rStart = t * rowsPerThread;
            int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

            futures.push_back(std::async(std::launch::async, [&src, &mf, &inv_2sigma2_lut, &frames, &acc, &wgt, rStart, rEnd, w, h]() {
                for (int r = rStart; r < rEnd; ++r) {
                    const uint8_t* refRow = frames[0].yPlane + r * frames[0].yRowStride;
                    float*         a      = acc.data() + r * w;
                    float*         ww     = wgt.data() + r * w;

                    for (int c = 0; c < w; ++c) {
                        // Interpolate motion vector for smooth warping (prevents block artifacts)
                        MotionVec mv = interpolateMotion(mf, r, c);

                        float sx = static_cast<float>(c + mv.dx);
                        float sy = static_cast<float>(r + mv.dy);
                        float warped = sampleY(src, sx, sy);

                        float ref_val = static_cast<float>(refRow[c]);
                        float residual = warped - ref_val;

                        int refLumaIdx = std::clamp(static_cast<int>(ref_val), 0, 255);
                        float inv_2sigma2 = inv_2sigma2_lut[refLumaIdx];
                        float w_val = std::exp(-residual * residual * inv_2sigma2);

                        a[c]  += w_val * warped;
                        ww[c] += w_val;
                    }
                }
            }));
        }
        for (auto& fut : futures) {
            fut.get();
        }
    }

    outY.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < acc.size(); ++i) {
        float v = (wgt[i] > 1e-6f) ? acc[i] / wgt[i] : acc[i];
        outY[i] = static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
    }
}

/// Simple nearest-neighbour chroma fusion (average of all frames, no motion).
/// Chroma detail at half-res contributes little to perceived sharpness.
static void fuseChromaPlane(const std::vector<YuvFrame>& frames,
                             const std::vector<MotionField>& motionFields,
                             bool isU,
                             std::vector<uint8_t>& out,
                             int uvW, int uvH,
                             const std::vector<std::vector<float>>& inv_2sigma2_luts)
{
    std::vector<float> acc(static_cast<size_t>(uvW) * uvH, 0.f);
    std::vector<float> wgt(static_cast<size_t>(uvW) * uvH, 0.f);

    // ── Reference frame: weight 1.0 ──────────────────────────────────────────
    {
        const YuvFrame& ref = frames[0];
        const uint8_t* plane = isU ? ref.uPlane : ref.vPlane;
        for (int r = 0; r < uvH; ++r) {
            float* a = acc.data() + r * uvW;
            float* ww = wgt.data() + r * uvW;
            for (int c = 0; c < uvW; ++c) {
                a[c] = static_cast<float>(plane[r * ref.uvRowStride + c * ref.uvPixelStride]);
                ww[c] = 1.f;
            }
        }
    }

    // ── Remaining frames: warp + accumulate ──────────────────────────────────
    for (size_t fi = 1; fi < frames.size(); ++fi) {
        const YuvFrame&   src = frames[fi];
        const MotionField& mf = motionFields[fi - 1];
        const uint8_t* plane = isU ? src.uPlane : src.vPlane;
        const std::vector<float>& inv_2sigma2_lut = inv_2sigma2_luts[fi];

        for (int r = 0; r < uvH; ++r) {
            float* a = acc.data() + r * uvW;
            float* ww = wgt.data() + r * uvW;
            int lumaR = r * 2;

            for (int c = 0; c < uvW; ++c) {
                int lumaC = c * 2;

                // Interpolate motion vector at luma scale, then scale by 0.5x
                MotionVec mv = interpolateMotion(mf, lumaR, lumaC);

                // Compute ghosting rejection weight from Y plane residual
                float sxLuma = static_cast<float>(lumaC + mv.dx);
                float syLuma = static_cast<float>(lumaR + mv.dy);
                float warpedY = sampleY(src, sxLuma, syLuma);

                float refY = static_cast<float>(frames[0].yPlane[lumaR * frames[0].yRowStride + lumaC]);
                float residual = warpedY - refY;

                int refLumaIdx = std::clamp(static_cast<int>(refY), 0, 255);
                float inv_2sigma2 = inv_2sigma2_lut[refLumaIdx];
                float w_val = std::exp(-residual * residual * inv_2sigma2);

                // Scaled down motion vectors for half-resolution chroma
                float sx = static_cast<float>(c) + mv.dx * 0.5f;
                float sy = static_cast<float>(r) + mv.dy * 0.5f;

                float warped = sampleUVBilinear(plane, src.uvRowStride, src.uvPixelStride, uvW, uvH, sx, sy);

                a[c]  += w_val * warped;
                ww[c] += w_val;
            }
        }
    }

    out.resize(static_cast<size_t>(uvW) * uvH);
    for (size_t i = 0; i < acc.size(); ++i) {
        float v = (wgt[i] > 1e-6f) ? acc[i] / wgt[i] : acc[i];
        out[i] = static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
    }
}

static void fuseRawBayer(const std::vector<YuvFrame>& frames,
                         const std::vector<MotionField>& motionFields,
                         std::vector<uint16_t>& outRaw,
                         int w, int h,
                         const std::vector<std::vector<float>>& inv_2sigma2_luts)
{
    std::vector<float> acc(static_cast<size_t>(w) * h, 0.f);
    std::vector<float> wgt(static_cast<size_t>(w) * h, 0.f);

    // ── Reference frame: weight 1.0 ──────────────────────────────────────────
    {
        const YuvFrame& ref = frames[0];
        const uint16_t* refRaw = reinterpret_cast<const uint16_t*>(ref.yPlane);
        int strideElements = ref.yRowStride / 2;
        for (int r = 0; r < h; ++r) {
            const uint16_t* row = refRaw + r * strideElements;
            float* a = acc.data() + r * w;
            float* ww = wgt.data() + r * w;
            for (int c = 0; c < w; ++c) {
                a[c] = static_cast<float>(row[c]);
                ww[c] = 1.f;
            }
        }
    }

    // ── Remaining frames: warp + accumulate in parallel ──────────────────────
    int numThreads = 8;
    int rowsPerThread = h / numThreads;

    for (size_t fi = 1; fi < frames.size(); ++fi) {
        const YuvFrame& src = frames[fi];
        const MotionField& mf = motionFields[fi - 1];
        const uint16_t* srcRaw = reinterpret_cast<const uint16_t*>(src.yPlane);
        int strideElements = src.yRowStride / 2;
        const std::vector<float>& inv_2sigma2_lut = inv_2sigma2_luts[fi];

        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);

        for (int t = 0; t < numThreads; ++t) {
            int rStart = t * rowsPerThread;
            int rEnd = (t == numThreads - 1) ? h : (t + 1) * rowsPerThread;

            futures.push_back(std::async(std::launch::async, [srcRaw, &mf, &inv_2sigma2_lut, &frames, &acc, &wgt, rStart, rEnd, w, h, strideElements]() {
                for (int r = rStart; r < rEnd; ++r) {
                    const uint16_t* refRawRow = reinterpret_cast<const uint16_t*>(frames[0].yPlane) + r * (frames[0].yRowStride / 2);
                    float* a = acc.data() + r * w;
                    float* ww = wgt.data() + r * w;

                    for (int c = 0; c < w; ++c) {
                        // Interpolate motion vector
                        MotionVec mv = interpolateMotion(mf, r, c);

                        // Constrain motion vectors to even steps to maintain Bayer grid color order
                        int dx = (mv.dx / 2) * 2;
                        int dy = (mv.dy / 2) * 2;

                        int sc = std::clamp(c + dx, 0, w - 1);
                        int sr = std::clamp(r + dy, 0, h - 1);
                        
                        float warped = static_cast<float>(srcRaw[sr * strideElements + sc]);
                        float ref_val = static_cast<float>(refRawRow[c]);
                        float residual = warped - ref_val;

                        // Scale to YUV-like 0-255 range for the LUT lookup
                        int refLumaIdx = std::clamp(static_cast<int>(ref_val / 16.0f), 0, 255);
                        float inv_2sigma2 = inv_2sigma2_lut[refLumaIdx];
                        float w_val = std::exp(-residual * residual * inv_2sigma2);

                        a[c] += w_val * warped;
                        ww[c] += w_val;
                    }
                }
            }));
        }
        for (auto& fut : futures) {
            fut.get();
        }
    }

    // Normalise and store in 16-bit output
    outRaw.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < acc.size(); ++i) {
        float v = (wgt[i] > 1e-6f) ? acc[i] / wgt[i] : acc[i];
        outRaw[i] = static_cast<uint16_t>(std::clamp(v, 0.f, 65535.f));
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// FusionStage::process
// ─────────────────────────────────────────────────────────────────────────────
bool FusionStage::process(FrameContext& ctx) {
    if (ctx.inputFrames.empty()) {
        LOGE("FusionStage: no input frames");
        return false;
    }
    if (ctx.motionFields.size() != ctx.inputFrames.size() - 1) {
        LOGE("FusionStage: motion field count mismatch (%zu fields, %zu frames)",
             ctx.motionFields.size(), ctx.inputFrames.size());
        return false;
    }

    int w = ctx.inputFrames[0].width;
    int h = ctx.inputFrames[0].height;
    int uvW = w / 2;
    int uvH = h / 2;

    bool isNight = false;
    if (ctx.metadata.count("night_mode")) {
        try {
            isNight = std::any_cast<bool>(ctx.metadata.at("night_mode"));
        } catch (...) {}
    }

    int iso = 400;
    if (ctx.metadata.count("iso")) {
        try {
            iso = std::any_cast<int>(ctx.metadata.at("iso"));
        } catch (...) {}
    }

    // Retrieve frame-specific ISO values
    std::vector<int> frameIsos;
    if (ctx.metadata.count("frame_isos")) {
        try {
            frameIsos = std::any_cast<std::vector<int>>(ctx.metadata.at("frame_isos"));
        } catch (...) {}
    }
    while (frameIsos.size() < ctx.inputFrames.size()) {
        frameIsos.push_back(iso);
    }

    float noiseMultiplier = 3.0f;
    if (ctx.metadata.count("fusion_noise_multiplier")) {
        try {
            noiseMultiplier = std::any_cast<float>(ctx.metadata.at("fusion_noise_multiplier"));
        } catch (...) {}
    }

    // ── Build Noise-Aware Fusion LUT per Frame ────────────────────────────────
    std::vector<std::vector<float>> inv_2sigma2_luts(ctx.inputFrames.size(), std::vector<float>(256));
    for (size_t fi = 0; fi < ctx.inputFrames.size(); ++fi) {
        int fIso = frameIsos[fi];
        for (int i = 0; i < 256; ++i) {
            float normVal = i / 255.f;

            // IMX882 Noise Model parameters
            double A = 1.210813e-06;
            double B = 9.711493e-06;
            double C = 1.376002e-12;
            double D = 1.901007e-07;

            double S = A * fIso + B;
            double digital_gain = (fIso / 3200.0) < 1.0 ? 1.0 : (fIso / 3200.0);
            double O = C * fIso * fIso + D * digital_gain * digital_gain;

            double variance = S * normVal + O;
            float noise_sigma = std::sqrt(variance < 0.0 ? 0.0 : variance) * 255.f;

            float threshold = std::max(2.5f, noiseMultiplier * noise_sigma);
            inv_2sigma2_luts[fi][i] = 1.f / (2.f * threshold * threshold);
        }
    }

    bool useRaw = false;
    if (ctx.metadata.count("use_raw_capture")) {
        try {
            useRaw = std::any_cast<bool>(ctx.metadata.at("use_raw_capture"));
        } catch (...) {}
    }

    if (useRaw) {
        LOGI("FusionStage: fusing 16-bit RAW Bayer frames");
        std::vector<uint16_t> fusedRaw;
        fuseRawBayer(ctx.inputFrames, ctx.motionFields, fusedRaw, w, h, inv_2sigma2_luts);
        ctx.metadata["fused_raw"] = fusedRaw;
    } else {
        fuseYPlane(ctx.inputFrames, ctx.motionFields, ctx.fusedY, w, h, inv_2sigma2_luts);
        fuseChromaPlane(ctx.inputFrames, ctx.motionFields, /*isU=*/true,  ctx.fusedU, uvW, uvH, inv_2sigma2_luts);
        fuseChromaPlane(ctx.inputFrames, ctx.motionFields, /*isU=*/false, ctx.fusedV, uvW, uvH, inv_2sigma2_luts);

        bool chromaDenoise = true;
        if (ctx.metadata.count("chroma_denoise_enabled")) {
            try {
                chromaDenoise = std::any_cast<bool>(ctx.metadata.at("chroma_denoise_enabled"));
            } catch (...) {}
        }

        if (isNight && chromaDenoise) {
            denoiseChroma(ctx.fusedU, uvW, uvH);
            denoiseChroma(ctx.fusedV, uvW, uvH);
        }

        // ── Spatial Luma Denoising (NL-Means) ──────────────────────────────────────
        // Applied after temporal fusion to clean up residual noise from imperfect alignment.
        // h_strength = 0 means disabled.
        {
            int spatialStrength = 8; // default
            if (ctx.metadata.count("spatial_denoise_strength")) {
                try {
                    spatialStrength = std::any_cast<int>(ctx.metadata.at("spatial_denoise_strength"));
                } catch (...) {}
            }
            if (spatialStrength > 0) {
                LOGI("FusionStage: applying spatial NL-Means luma denoise (h=%d)", spatialStrength);
                spatialDenoiseLuma(ctx.fusedY, w, h, spatialStrength);
            }
        }
    }

    ctx.fusedWidth  = w;
    ctx.fusedHeight = h;

    // Save intermediate fused YUV + JPEG
    if (ctx.metadata.count("debug_dir")) {
        try {
            std::string debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir"));
            if (useRaw) {
                if (ctx.metadata.count("fused_raw")) {
                    const auto& fusedRaw = std::any_cast<const std::vector<uint16_t>&>(ctx.metadata.at("fused_raw"));
                    std::string rawPath = debugDir + "/stage_1_fusion/fused.raw";
                    std::ofstream out(rawPath, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char*>(fusedRaw.data()), fusedRaw.size() * sizeof(uint16_t));
                        out.close();
                    }
                    // Save 8-bit grayscale preview JPEG of fused RAW plane
                    std::vector<uint8_t> previewY(w * h);
                    for (size_t i = 0; i < previewY.size(); ++i) {
                        previewY[i] = static_cast<uint8_t>(std::clamp(fusedRaw[i] / 16.f, 0.f, 255.f));
                    }
                    std::vector<uint8_t> uvGray(w * h / 2, 128);
                    saveYuvAsJpeg(previewY.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/fused.jpg");
                }
            } else {
                // Save Reference Frame
                const YuvFrame& ref = ctx.inputFrames[0];
                saveStridedYuvAsJpeg(
                    ref.yPlane, ref.yRowStride, ref.uPlane, ref.uvRowStride, ref.vPlane, ref.uvPixelStride,
                    w, h, debugDir + "/stage_1_fusion/ref_frame.jpg");

                // Save Source Frame 1 (Offset)
                if (ctx.inputFrames.size() > 1) {
                    const YuvFrame& src = ctx.inputFrames[1];
                    saveStridedYuvAsJpeg(
                        src.yPlane, src.yRowStride, src.uPlane, src.uvRowStride, src.vPlane, src.uvPixelStride,
                        w, h, debugDir + "/stage_1_fusion/src_frame_1.jpg");

                    // Compute Difference Before Alignment
                    std::vector<uint8_t> diffBefore(static_cast<size_t>(w) * h);
                    for (int r = 0; r < h; ++r) {
                        const uint8_t* refRow = ref.yPlane + r * ref.yRowStride;
                        const uint8_t* srcRow = src.yPlane + r * src.yRowStride;
                        for (int c = 0; c < w; ++c) {
                            diffBefore[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>(std::abs(static_cast<int>(refRow[c]) - static_cast<int>(srcRow[c])));
                        }
                    }
                    std::vector<uint8_t> uvGray(static_cast<size_t>(w / 2) * (h / 2), 128);
                    saveYuvAsJpeg(diffBefore.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/diff_before_alignment.jpg");

                    // Compute Difference After Alignment
                    std::vector<uint8_t> diffAfter(static_cast<size_t>(w) * h);
                    for (int r = 0; r < h; ++r) {
                        const uint8_t* refRow = ref.yPlane + r * ref.yRowStride;
                        for (int c = 0; c < w; ++c) {
                            MotionVec mv = interpolateMotion(ctx.motionFields[0], r, c);
                            float sx = static_cast<float>(c) + mv.dx;
                            float sy = static_cast<float>(r) + mv.dy;
                            float warpedVal = sampleY(src, sx, sy);
                            diffAfter[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>(std::abs(static_cast<int>(refRow[c]) - static_cast<int>(warpedVal)));
                        }
                    }
                    saveYuvAsJpeg(diffAfter.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/diff_after_alignment.jpg");

                    // Crop and save 200x200 center crops
                    int cy = h / 2;
                    int cx = w / 2;
                    int cropSize = 200;
                    int startX = ((cx - cropSize / 2) / 2) * 2;
                    int startY = ((cy - cropSize / 2) / 2) * 2;

                    // Reference noisy crop
                    std::vector<uint8_t> cropRefY(cropSize * cropSize);
                    std::vector<uint8_t> cropRefU((cropSize / 2) * (cropSize / 2));
                    std::vector<uint8_t> cropRefV((cropSize / 2) * (cropSize / 2));
                    for (int r = 0; r < cropSize; ++r) {
                        std::copy(
                            ref.yPlane + (startY + r) * ref.yRowStride + startX,
                            ref.yPlane + (startY + r) * ref.yRowStride + startX + cropSize,
                            cropRefY.data() + r * cropSize
                        );
                        if (r % 2 == 0) {
                            int uvCropW = cropSize / 2;
                            for (int c = 0; c < uvCropW; ++c) {
                                cropRefU[(r / 2) * uvCropW + c] = ref.uPlane[((startY + r) / 2) * ref.uvRowStride + (startX / 2 + c) * ref.uvPixelStride];
                                cropRefV[(r / 2) * uvCropW + c] = ref.vPlane[((startY + r) / 2) * ref.uvRowStride + (startX / 2 + c) * ref.uvPixelStride];
                            }
                        }
                    }
                    saveYuvAsJpeg(cropRefY.data(), cropRefU.data(), cropRefV.data(), cropSize, cropSize, debugDir + "/stage_1_fusion/noisy_crop.jpg");

                    // Fused denoised crop
                    std::vector<uint8_t> cropFusedY(cropSize * cropSize);
                    std::vector<uint8_t> cropFusedU((cropSize / 2) * (cropSize / 2));
                    std::vector<uint8_t> cropFusedV((cropSize / 2) * (cropSize / 2));
                    for (int r = 0; r < cropSize; ++r) {
                        std::copy(
                            ctx.fusedY.data() + (startY + r) * w + startX,
                            ctx.fusedY.data() + (startY + r) * w + startX + cropSize,
                            cropFusedY.data() + r * cropSize
                        );
                        if (r % 2 == 0) {
                            int uvW = w / 2;
                            int uvCropW = cropSize / 2;
                            std::copy(
                                ctx.fusedU.data() + ((startY + r) / 2) * uvW + (startX / 2),
                                ctx.fusedU.data() + ((startY + r) / 2) * uvW + (startX / 2) + uvCropW,
                                cropFusedU.data() + (r / 2) * uvCropW
                            );
                            std::copy(
                                ctx.fusedV.data() + ((startY + r) / 2) * uvW + (startX / 2),
                                ctx.fusedV.data() + ((startY + r) / 2) * uvW + (startX / 2) + uvCropW,
                                cropFusedV.data() + (r / 2) * uvCropW
                            );
                        }
                    }
                    saveYuvAsJpeg(cropFusedY.data(), cropFusedU.data(), cropFusedV.data(), cropSize, cropSize, debugDir + "/stage_1_fusion/denoised_crop.jpg");
                }

                // Raw planar YUV
                std::string yuvPath = debugDir + "/stage_1_fusion/fused.yuv";
                std::ofstream out(yuvPath, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(ctx.fusedY.data()), ctx.fusedY.size());
                    out.write(reinterpret_cast<const char*>(ctx.fusedU.data()), ctx.fusedU.size());
                    out.write(reinterpret_cast<const char*>(ctx.fusedV.data()), ctx.fusedV.size());
                    out.close();
                }

                // JPEG preview
                saveYuvAsJpeg(
                    ctx.fusedY.data(), ctx.fusedU.data(), ctx.fusedV.data(),
                    w, h,
                    debugDir + "/stage_1_fusion/fused.jpg");
            }
        } catch (...) {}
    }

    LOGI("FusionStage: fused %d×%d from %zu frames (isNight = %d)", w, h, ctx.inputFrames.size(), isNight);
    return true;
}
