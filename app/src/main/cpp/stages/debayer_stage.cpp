#include "debayer_stage.h"
#include "../debug_utils.h"

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

        // Bilinear Demosaicing (RGGB layout) - Multi-threaded
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
                    
                    // Determine Bayer color filter grid position (BGGR pattern)
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
