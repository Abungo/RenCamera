#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <any>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// YuvFrame
//   Holds non-owning pointers into a Camera2 YUV_420_888 ImageReader buffer.
//   Valid only while the originating android.media.Image is open.
// ─────────────────────────────────────────────────────────────────────────────
struct YuvFrame {
    const uint8_t* yPlane        = nullptr;
    const uint8_t* uPlane        = nullptr;
    const uint8_t* vPlane        = nullptr;
    int            yRowStride    = 0;   // bytes per row, Y plane
    int            uvRowStride   = 0;   // bytes per row, U/V plane
    int            uvPixelStride = 0;   // 1 = planar (I420), 2 = semi-planar (NV12/NV21)
    int            width         = 0;
    int            height        = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MotionVec / MotionField
//   Per-block displacement (dx, dy) in full-resolution pixels.
//   Produced by AlignStage, consumed by FusionStage.
// ─────────────────────────────────────────────────────────────────────────────
struct MotionVec {
    int dx = 0;
    int dy = 0;
};

struct MotionField {
    std::vector<MotionVec> vectors; // row-major: [block_row * blocksWide + block_col]
    int blocksWide = 0;
    int blocksTall = 0;
    int blockSize  = 16; // pixels per block edge

    MotionVec& at(int row, int col) { return vectors[row * blocksWide + col]; }
    const MotionVec& at(int row, int col) const { return vectors[row * blocksWide + col]; }
};

// ─────────────────────────────────────────────────────────────────────────────
// FloatImage — single-channel floating-point image (0.0 – 255.0 range)
// ─────────────────────────────────────────────────────────────────────────────
struct FloatImage {
    std::vector<float> data;
    int width  = 0;
    int height = 0;

    void resize(int w, int h) {
        width = w; height = h;
        data.assign(static_cast<size_t>(w) * h, 0.f);
    }

    float& at(int row, int col)       { return data[row * width + col]; }
    float  at(int row, int col) const { return data[row * width + col]; }

    float* ptr(int row = 0) { return data.data() + row * width; }
    const float* ptr(int row = 0) const { return data.data() + row * width; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RgbImage — 3-channel interleaved RGB (uint8, 0-255)
// ─────────────────────────────────────────────────────────────────────────────
struct RgbImage {
    std::vector<uint8_t> data; // layout: R G B R G B …
    int width  = 0;
    int height = 0;

    void resize(int w, int h) {
        width = w; height = h;
        data.assign(static_cast<size_t>(w) * h * 3, 0u);
    }

    uint8_t* pixel(int row, int col)       { return data.data() + (row * width + col) * 3; }
    const uint8_t* pixel(int row, int col) const { return data.data() + (row * width + col) * 3; }
    uint8_t* rowPtr(int row) { return data.data() + row * width * 3; }
};

// ─────────────────────────────────────────────────────────────────────────────
// FrameContext
//   The "property bag" that flows through every PipelineStage.
//   Each stage reads its inputs and writes its outputs here.
//   Adding a new stage never requires changing the pipeline runner.
// ─────────────────────────────────────────────────────────────────────────────
struct FrameContext {
    // ── Stage inputs ──────────────────────────────────────────────────────────
    std::vector<YuvFrame> inputFrames;       // 12 raw frames from Camera2

    // ── Intermediate data ─────────────────────────────────────────────────────
    std::vector<MotionField> motionFields;   // [11] written by AlignStage

    // Fused luminance + chroma planes (uint8, I420 layout after fusion)
    std::vector<uint8_t> fusedY;             // written by FusionStage
    std::vector<uint8_t> fusedU;
    std::vector<uint8_t> fusedV;
    int fusedWidth  = 0;
    int fusedHeight = 0;

    RgbImage colorImage;                     // written by DebayerStage
    RgbImage processedImage;                 // written by ToneMapStage / future stages

    // ── Final output ──────────────────────────────────────────────────────────
    std::vector<uint8_t> jpegOutput;         // written by EncodeStage

    // ── Extensible metadata ───────────────────────────────────────────────────
    // Future stages can store arbitrary typed data without changing this struct.
    // Example: ctx.metadata["denoise_strength"] = 0.7f;
    std::unordered_map<std::string, std::any> metadata;

    // ── Convenience ───────────────────────────────────────────────────────────
    int width()  const { return inputFrames.empty() ? 0 : inputFrames[0].width; }
    int height() const { return inputFrames.empty() ? 0 : inputFrames[0].height; }
};
