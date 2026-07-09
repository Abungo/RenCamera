#include "align_stage.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <vector>

// Highway SIMD — processes the Gaussian blur inner loop
#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

namespace {

static constexpr int BLOCK_SIZE    = 16;  // pixels per alignment block
static constexpr int SEARCH_RANGE  = 16;  // ±pixels at coarsest pyramid level
static constexpr int REFINE_RANGE  =  4;  // ±pixels at finer levels

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Copy a Camera2 YUV_420_888 Y-plane to a packed FloatImage downsampled by 2x
/// using a 2x2 average box filter. This speeds up alignment by 4x.
static FloatImage yPlaneToFloatDownsampled2x(const YuvFrame& f, bool useRaw) {
    FloatImage out;
    int dw = f.width / 2;
    int dh = f.height / 2;
    out.resize(dw, dh);
    
    if (useRaw) {
        int strideElements = f.yRowStride / 2;
        const uint16_t* rawBase = reinterpret_cast<const uint16_t*>(f.yPlane);
        for (int row = 0; row < dh; ++row) {
            const uint16_t* src0 = rawBase + (2 * row) * strideElements;
            const uint16_t* src1 = src0 + strideElements;
            float*         dst = out.ptr(row);
            for (int col = 0; col < dw; ++col) {
                // Average the 2x2 Bayer quadrant pixels and scale from 12-bit (0-4095) to 8-bit (0-255) range
                float sum = static_cast<float>(src0[2 * col]) + static_cast<float>(src0[2 * col + 1]) +
                            static_cast<float>(src1[2 * col]) + static_cast<float>(src1[2 * col + 1]);
                dst[col] = sum * 0.25f / 16.0f;
            }
        }
    } else {
        for (int row = 0; row < dh; ++row) {
            const uint8_t* src0 = f.yPlane + (2 * row) * f.yRowStride;
            const uint8_t* src1 = src0 + f.yRowStride;
            float*         dst = out.ptr(row);
            for (int col = 0; col < dw; ++col) {
                dst[col] = (src0[2 * col] + src0[2 * col + 1] +
                            src1[2 * col] + src1[2 * col + 1]) * 0.25f;
            }
        }
    }
    return out;
}

/// 1-D separable Gaussian blur using a 5-tap kernel [1 4 6 4 1]/16.
/// Processes one row in-place with clamped boundary.
static void blurRow(float* row, int width, std::vector<float>& tmp) {
    if (tmp.size() < static_cast<size_t>(width)) tmp.resize(width);
    for (int x = 0; x < width; ++x) {
        auto clamp = [&](int i) { return row[std::clamp(i, 0, width - 1)]; };
        tmp[x] = (clamp(x-2) + 4*clamp(x-1) + 6*row[x] + 4*clamp(x+1) + clamp(x+2)) / 16.f;
    }
    std::copy(tmp.begin(), tmp.begin() + width, row);
}

static void blurCol(FloatImage& img, int col, std::vector<float>& tmp) {
    int h = img.height;
    if (tmp.size() < static_cast<size_t>(h)) tmp.resize(h);
    for (int y = 0; y < h; ++y) {
        auto clamp = [&](int i) { return img.at(std::clamp(i, 0, h-1), col); };
        tmp[y] = (clamp(y-2) + 4*clamp(y-1) + 6*img.at(y,col) + 4*clamp(y+1) + clamp(y+2)) / 16.f;
    }
    for (int y = 0; y < h; ++y) img.at(y, col) = tmp[y];
}

/// Gaussian blur a FloatImage in-place (separable, O(5×w×h)).
static void gaussianBlur(FloatImage& img, std::vector<float>& scratchRow, std::vector<float>& scratchCol) {
    for (int r = 0; r < img.height; ++r) blurRow(img.ptr(r), img.width, scratchRow);
    for (int c = 0; c < img.width;  ++c) blurCol(img, c, scratchCol);
}

/// Downsample a FloatImage by 2 in each dimension (box average 2×2).
static FloatImage downsample(const FloatImage& src) {
    FloatImage dst;
    int dw = src.width  / 2;
    int dh = src.height / 2;
    dst.resize(dw, dh);
    for (int r = 0; r < dh; ++r) {
        for (int c = 0; c < dw; ++c) {
            float v = (src.at(2*r,   2*c)   + src.at(2*r,   2*c+1)
                     + src.at(2*r+1, 2*c)   + src.at(2*r+1, 2*c+1)) * 0.25f;
            dst.at(r, c) = v;
        }
    }
    return dst;
}

/// Build a 3-level Gaussian pyramid.  pyramid[0] = finest (original).
using Pyramid = std::vector<FloatImage>;

static Pyramid buildPyramid(FloatImage base) {
    Pyramid p;
    p.reserve(3);
    
    std::vector<float> scratchRow(base.width);
    std::vector<float> scratchCol(base.height);
    
    p.push_back(std::move(base));
    for (int lvl = 1; lvl < 3; ++lvl) {
        FloatImage blurred = p.back(); // copy
        gaussianBlur(blurred, scratchRow, scratchCol);
        p.push_back(downsample(blurred));
    }
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block matching — returns integer (dx, dy) in `level`-resolution pixels
// ─────────────────────────────────────────────────────────────────────────────

static MotionVec matchBlock(
    const FloatImage& ref,
    const FloatImage& src,
    int bx, int by,       // block index (not pixel)
    int blockPx,          // block size in this level's pixels
    int range,            // search range ±range pixels
    MotionVec hint,       // initial displacement in this level's pixels
    float regFactor
) {
    int imgW = ref.width, imgH = ref.height;
    int bestSSD = std::numeric_limits<int>::max();
    int bestCost = std::numeric_limits<int>::max();
    MotionVec best = hint;

    // Check if the block at maximum search displacement is fully within image bounds.
    // This allows us to skip boundary checks in the inner loop in 99% of cases.
    int minTdx = hint.dx - range;
    int maxTdx = hint.dx + range;
    int minTdy = hint.dy - range;
    int maxTdy = hint.dy + range;

    bool alwaysInside = (by * blockPx + minTdy >= 0) &&
                        ((by + 1) * blockPx + maxTdy <= imgH) &&
                        (bx * blockPx + minTdx >= 0) &&
                        ((bx + 1) * blockPx + maxTdx <= imgW);

    if (alwaysInside) {
        // 1. Evaluate the hint first to establish a tight threshold for early exit
        int hintSSD = 0;
        for (int ry = 0; ry < blockPx; ++ry) {
            int refY = by * blockPx + ry;
            int srcY = refY + hint.dy;
            const float* refRow = ref.ptr(refY);
            const float* srcRow = src.ptr(srcY);
            int startRefX = bx * blockPx;
            int startSrcX = startRefX + hint.dx;
            for (int rx = 0; rx < blockPx; ++rx) {
                float diff = refRow[startRefX + rx] - srcRow[startSrcX + rx];
                hintSSD += static_cast<int>(diff * diff);
            }
        }
        bestSSD = hintSSD;
        bestCost = hintSSD; // Hint has dx=0, dy=0 relative to itself, so penalty = 0

        // 2. Perform search range, skipping the hint position
        for (int dy = -range; dy <= range; ++dy) {
            for (int dx = -range; dx <= range; ++dx) {
                if (dx == 0 && dy == 0) continue; // hint was already evaluated
                int tdx = hint.dx + dx, tdy = hint.dy + dy;
                int ssd = 0;
                // Add a small penalty for moving away from the hint (coarse scale prediction)
                // to bias towards zero-motion in noisy areas (regularization).
                int penalty = static_cast<int>(regFactor * (dx * dx + dy * dy));
                
                for (int ry = 0; ry < blockPx; ++ry) {
                    int refY = by * blockPx + ry;
                    int srcY = refY + tdy;
                    
                    const float* refRow = ref.ptr(refY);
                    const float* srcRow = src.ptr(srcY);
                    
                    int startRefX = bx * blockPx;
                    int startSrcX = startRefX + tdx;
                    
                    for (int rx = 0; rx < blockPx; ++rx) {
                        float diff = refRow[startRefX + rx] - srcRow[startSrcX + rx];
                        ssd += static_cast<int>(diff * diff);
                    }
                    if (ssd + penalty >= bestCost) break; // Early exit
                }
                
                int cost = ssd + penalty;
                if (cost < bestCost) {
                    bestCost = cost;
                    bestSSD = ssd;
                    best = {tdx, tdy};
                }
            }
        }
    } else {
        // Fallback for border blocks with bounds check
        // First evaluate the hint
        int hintSSD = 0;
        bool hintValid = true;
        for (int ry = 0; ry < blockPx; ++ry) {
            int refY = by * blockPx + ry;
            int srcY = refY + hint.dy;
            if (refY >= imgH || srcY < 0 || srcY >= imgH) { hintValid = false; break; }
            
            const float* refRow = ref.ptr(refY);
            const float* srcRow = src.ptr(srcY);
            for (int rx = 0; rx < blockPx; ++rx) {
                int refX = bx * blockPx + rx;
                int srcX = refX + hint.dx;
                if (refX >= imgW || srcX < 0 || srcX >= imgW) continue;
                float diff = refRow[refX] - srcRow[srcX];
                hintSSD += static_cast<int>(diff * diff);
            }
        }
        if (hintValid) {
            bestSSD = hintSSD;
            bestCost = hintSSD;
        }

        for (int dy = -range; dy <= range; ++dy) {
            for (int dx = -range; dx <= range; ++dx) {
                if (hintValid && dx == 0 && dy == 0) continue;
                int tdx = hint.dx + dx, tdy = hint.dy + dy;
                int ssd = 0;
                int penalty = static_cast<int>(regFactor * (dx * dx + dy * dy));
                bool valid = true;
                for (int ry = 0; ry < blockPx && valid; ++ry) {
                    int refY = by * blockPx + ry;
                    int srcY = refY + tdy;
                    if (refY >= imgH || srcY < 0 || srcY >= imgH) { valid = false; break; }
                    
                    const float* refRow = ref.ptr(refY);
                    const float* srcRow = src.ptr(srcY);
                    
                    for (int rx = 0; rx < blockPx; ++rx) {
                        int refX = bx * blockPx + rx;
                        int srcX = refX + tdx;
                        if (refX >= imgW || srcX < 0 || srcX >= imgW) continue;
                        float diff = refRow[refX] - srcRow[srcX];
                        ssd += static_cast<int>(diff * diff);
                    }
                    if (ssd + penalty >= bestCost) break; // Early exit
                }
                int cost = ssd + penalty;
                if (valid && cost < bestCost) {
                    bestCost = cost;
                    bestSSD = ssd;
                    best = {tdx, tdy};
                }
            }
        }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// Coarse-to-fine alignment for one frame pair
// ─────────────────────────────────────────────────────────────────────────────

static MotionField alignFrame(const Pyramid& refPyr, const Pyramid& srcPyr, float regFactor) {
    const int levels = static_cast<int>(refPyr.size()); // 3
    int scale = 1 << (levels - 1); // 4 at level 2, 2 at level 1, 1 at level 0

    // Block size at the coarsest level (level 2 = 1/4 resolution)
    int blockPxL2 = std::max(1, BLOCK_SIZE / scale);
    const FloatImage& refL2 = refPyr[levels - 1];
    int bw = std::max(1, refL2.width  / blockPxL2);
    int bh = std::max(1, refL2.height / blockPxL2);

    // Allocate motion field (full-res block dimensions for level 0)
    int bwFull = std::max(1, refPyr[0].width  / BLOCK_SIZE);
    int bhFull = std::max(1, refPyr[0].height / BLOCK_SIZE);
    MotionField mf;
    mf.blocksWide = bwFull;
    mf.blocksTall = bhFull;
    mf.vectors.resize(static_cast<size_t>(bwFull) * bhFull);

    // Coarse level motion field
    std::vector<MotionVec> coarse(static_cast<size_t>(bw) * bh, {0, 0});

    // ── Level 2 (coarsest): full search ──────────────────────────────────────
    for (int row = 0; row < bh; ++row) {
        for (int col = 0; col < bw; ++col) {
            coarse[row * bw + col] = matchBlock(
                refPyr[2], srcPyr[2], col, row, blockPxL2, SEARCH_RANGE, {0, 0}, regFactor);
        }
    }

    // ── Levels 1 → 0: refine with hint from coarser level ───────────────────
    std::vector<MotionVec> prev = coarse;
    int prevBw = bw, prevBh = bh;

    for (int lvl = levels - 2; lvl >= 0; --lvl) {
        int curScale = 1 << lvl;
        int blockPx  = std::max(1, BLOCK_SIZE / curScale);
        const FloatImage& refL = refPyr[lvl];
        const FloatImage& srcL = srcPyr[lvl];
        int curBw = std::max(1, refL.width  / blockPx);
        int curBh = std::max(1, refL.height / blockPx);

        std::vector<MotionVec> cur(static_cast<size_t>(curBw) * curBh);
        for (int row = 0; row < curBh; ++row) {
            for (int col = 0; col < curBw; ++col) {
                // Map block to parent-level block (1-to-1 mapping as grid size remains constant)
                int pr = std::min(row, prevBh - 1);
                int pc = std::min(col, prevBw - 1);
                // Scale displacement ×2 (one more pyramid level = ×2 pixels)
                MotionVec hint = {prev[pr * prevBw + pc].dx * 2,
                                  prev[pr * prevBw + pc].dy * 2};
                cur[row * curBw + col] = matchBlock(
                    refL, srcL, col, row, blockPx, REFINE_RANGE, hint, regFactor);
            }
        }
        prev = cur; prevBw = curBw; prevBh = curBh;

        // At level 0, prev IS the full-resolution block motion field
        if (lvl == 0) {
            for (int i = 0; i < static_cast<int>(prev.size()); ++i) {
                mf.vectors[i] = prev[i];
            }
        }
    }

    return mf;
}

} // anonymous namespace

#include <future>

// ─────────────────────────────────────────────────────────────────────────────
// AlignStage::process
// ─────────────────────────────────────────────────────────────────────────────
bool AlignStage::process(FrameContext& ctx) {
    if (ctx.inputFrames.empty()) {
        LOGE("AlignStage: no input frames");
        return false;
    }

    bool useRaw = false;
    if (ctx.metadata.count("use_raw_capture")) {
        try {
            useRaw = std::any_cast<bool>(ctx.metadata.at("use_raw_capture"));
        } catch (...) {}
    }

    // Frame 0 is the reference (last captured = most recent, sharpest focus)
    const YuvFrame& refFrame = ctx.inputFrames[0];
    Pyramid refPyr = buildPyramid(yPlaneToFloatDownsampled2x(refFrame, useRaw));

    size_t numSrcFrames = ctx.inputFrames.size() - 1;
    ctx.motionFields.clear();
    ctx.motionFields.resize(numSrcFrames);

    std::vector<std::future<void>> futures;
    futures.reserve(numSrcFrames);

    float regFactor = 5.0f;
    if (ctx.metadata.count("alignment_regularization")) {
        try {
            regFactor = std::any_cast<float>(ctx.metadata.at("alignment_regularization"));
        } catch (...) {}
    }

    for (size_t i = 0; i < numSrcFrames; ++i) {
        size_t srcIndex = i + 1;
        futures.push_back(std::async(std::launch::async, [&ctx, &refPyr, srcIndex, i, regFactor, useRaw]() {
            Pyramid srcPyr = buildPyramid(yPlaneToFloatDownsampled2x(ctx.inputFrames[srcIndex], useRaw));
            MotionField mf = alignFrame(refPyr, srcPyr, regFactor);
            
            // Scale up the motion vectors and block size by 2x
            // because the alignment was computed on a 2x downsampled image
            for (auto& vec : mf.vectors) {
                vec.dx *= 2;
                vec.dy *= 2;
            }
            mf.blockSize *= 2;
            
            ctx.motionFields[i] = mf;
        }));
    }

    for (auto& fut : futures) {
        fut.get();
    }

    LOGI("AlignStage: aligned %zu frames in parallel (2x downsampled)", numSrcFrames);
    return true;
}
