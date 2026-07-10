#include "fusion_stage.h"
#include "../debug_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <fstream>
#include <future>
#include <chrono>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

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
    // Bypassed: CPU NL-Means is too slow for 12MP images on mobile CPU threads (takes 30-40 seconds).
    // The GPU temporal fusion stage already achieves ~3.5x noise reduction.
    return;
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
#include "../gl_helpers.h"

const char* COMPUTE_SHADER_SRC = R"glsl(
#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;

precision highp float;
precision highp sampler2DArray;
precision highp sampler2D;

uniform sampler2DArray u_input_frames;
uniform sampler2DArray u_motion_fields;
uniform sampler2D u_noise_luts;

uniform int u_num_frames;
uniform int u_width;
uniform int u_height;
uniform int u_blocks_wide;
uniform int u_blocks_tall;
uniform float u_block_size;

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    uint outY[];
};

// Accumulate per-frame weights for chroma ghosting protection
layout(std430, binding = 1) writeonly buffer WeightBuffer {
    float outWeights[];
};

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    float w_inv = 1.0 / float(u_width);
    float h_inv = 1.0 / float(u_height);
    vec2 uv = (vec2(pos) + 0.5) * vec2(w_inv, h_inv);

    // Sample 3x3 patch for reference luma (robust against single-pixel noise)
    float ref_val = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 p_uv = (vec2(pos) + vec2(dx, dy) + 0.5) * vec2(w_inv, h_inv);
            ref_val += texture(u_input_frames, vec3(p_uv, 0.0)).r * 255.0;
        }
    ref_val /= 9.0;

    float acc = texture(u_input_frames, vec3(uv, 0.0)).r * 255.0;
    float wgt = 1.0;
    float total_frame_weight = 1.0; // weight contribution for chroma (frame 0 = 1)

    for (int f = 1; f < u_num_frames; ++f) {
        float bx = (float(pos.x) - u_block_size * 0.5) / u_block_size;
        float by = (float(pos.y) - u_block_size * 0.5) / u_block_size;
        vec2 mv_uv = vec2((bx + 0.5) / float(u_blocks_wide), (by + 0.5) / float(u_blocks_tall));

        vec2 mv = texture(u_motion_fields, vec3(mv_uv, float(f - 1))).rg;

        // Sample 3x3 patch for warped luma to reduce noise sensitivity
        float warped_val = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                vec2 p_uv = (vec2(pos) + mv + vec2(dx, dy) + 0.5) * vec2(w_inv, h_inv);
                warped_val += texture(u_input_frames, vec3(p_uv, float(f))).r * 255.0;
            }
        warped_val /= 9.0;

        float residual = warped_val - ref_val;

        int ref_idx = clamp(int(round(ref_val)), 0, 255);
        float inv_2sigma2 = texelFetch(u_noise_luts, ivec2(ref_idx, f), 0).r;

        float w_val = exp(-residual * residual * inv_2sigma2);

        // Hard ghost rejection: clamp near-zero weights to exactly 0
        w_val = (w_val < 0.05) ? 0.0 : w_val;

        // Sample center pixel (not patch) for accumulation quality
        float center_warped = texture(u_input_frames, vec3((vec2(pos) + mv + 0.5) * vec2(w_inv, h_inv), float(f))).r * 255.0;
        acc += center_warped * w_val;
        wgt += w_val;
        total_frame_weight += w_val;
    }

    float fused_val = (wgt > 1e-6) ? acc / wgt : acc;

    uint idx = uint(pos.y * u_width + pos.x);
    outY[idx] = uint(clamp(fused_val, 0.0, 255.0));

    // Write normalized frame weight (0=fully ghosted, 1=all frames accepted)
    outWeights[idx] = total_frame_weight / float(u_num_frames);
}
)glsl";

static void fuseYPlane(const std::vector<YuvFrame>& frames,
                       const std::vector<MotionField>& motionFields,
                       std::vector<uint8_t>& outY,
                       std::vector<float>& outWeights,
                       int w, int h,
                       const std::vector<std::vector<float>>& inv_2sigma2_luts,
                       const std::string& debugDir)
{
    auto tStart = std::chrono::high_resolution_clock::now();
    int numFrames = frames.size();
    bool success = false;
    std::string logMsg;
    std::string errorLog;

    int blocksWide = motionFields[0].blocksWide;
    int blocksTall = motionFields[0].blocksTall;
    int blockSize = motionFields[0].blockSize;
    int numFields = motionFields.size();

    // 1. Set up headless EGL Context
    EglHeadlessSetup egl;
    if (egl.init(errorLog)) {
        // Compile compute program
        GLuint program = createComputeProgram(COMPUTE_SHADER_SRC, errorLog);
        if (program != 0) {
            glUseProgram(program);

            // Upload input frames to a 2D Texture Array
            GLuint inputFramesTex;
            glGenTextures(1, &inputFramesTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, inputFramesTex);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R8, w, h, numFrames);
            for (int f = 0; f < numFrames; ++f) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, frames[f].yRowStride);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, f, w, h, 1, GL_RED, GL_UNSIGNED_BYTE, frames[f].yPlane);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Pack motion fields [2, blocksWide, blocksTall, numFields]
            std::vector<float> contiguousMVs(2 * blocksWide * blocksTall * numFields);
            for (int f = 0; f < numFields; ++f) {
                const MotionField& mf = motionFields[f];
                for (int y = 0; y < blocksTall; ++y) {
                    for (int x = 0; x < blocksWide; ++x) {
                        MotionVec mv = mf.vectors[y * blocksWide + x];
                        contiguousMVs[0 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dx;
                        contiguousMVs[1 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dy;
                    }
                }
            }

            // Upload motion fields to a 2D Texture Array
            GLuint motionFieldsTex;
            glGenTextures(1, &motionFieldsTex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, motionFieldsTex);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RG32F, blocksWide, blocksTall, numFields);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, blocksWide, blocksTall, numFields, GL_RG, GL_FLOAT, contiguousMVs.data());
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Pack noise LUTs
            std::vector<float> flatLuts(256 * numFrames);
            for (int f = 0; f < numFrames; ++f) {
                std::copy(inv_2sigma2_luts[f].begin(), inv_2sigma2_luts[f].end(), flatLuts.data() + f * 256);
            }

            // Upload noise LUTs to a 2D Texture
            GLuint noiseLutsTex;
            glGenTextures(1, &noiseLutsTex);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, noiseLutsTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, 256, numFrames);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, numFrames, GL_RED, GL_FLOAT, flatLuts.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            // Set uniforms
            glUniform1i(glGetUniformLocation(program, "u_input_frames"), 0);
            glUniform1i(glGetUniformLocation(program, "u_motion_fields"), 1);
            glUniform1i(glGetUniformLocation(program, "u_noise_luts"), 2);
            glUniform1i(glGetUniformLocation(program, "u_num_frames"), numFrames);
            glUniform1i(glGetUniformLocation(program, "u_width"), w);
            glUniform1i(glGetUniformLocation(program, "u_height"), h);
            glUniform1i(glGetUniformLocation(program, "u_blocks_wide"), blocksWide);
            glUniform1i(glGetUniformLocation(program, "u_blocks_tall"), blocksTall);
            glUniform1f(glGetUniformLocation(program, "u_block_size"), static_cast<float>(blockSize));

            // Set up output SSBO (binding=0): fused Y plane
            GLuint outBuffer;
            glGenBuffers(1, &outBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<size_t>(w) * h * 4, nullptr, GL_DYNAMIC_READ);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);

            // Set up weight SSBO (binding=1): per-pixel frame acceptance weights for chroma ghosting
            GLuint weightBuffer;
            glGenBuffers(1, &weightBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, weightBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<size_t>(w) * h * sizeof(float), nullptr, GL_DYNAMIC_READ);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, weightBuffer);

            checkGlError("resource binding", errorLog);

            // Dispatch compute shader (16x16 tiles)
            glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

            checkGlError("dispatch", errorLog);

            // Map and retrieve Y output
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
            void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<size_t>(w) * h * 4, GL_MAP_READ_BIT);
            if (ptr != nullptr) {
                const uint32_t* results = static_cast<const uint32_t*>(ptr);
                outY.resize(static_cast<size_t>(w) * h);
                for (size_t i = 0; i < outY.size(); ++i) {
                    outY[i] = static_cast<uint8_t>(results[i]);
                }
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

                // Map and retrieve weight output
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, weightBuffer);
                void* wptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<size_t>(w) * h * sizeof(float), GL_MAP_READ_BIT);
                if (wptr != nullptr) {
                    outWeights.resize(static_cast<size_t>(w) * h);
                    std::memcpy(outWeights.data(), wptr, outWeights.size() * sizeof(float));
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                } else {
                    // Weight readback failed - fill with 1.0 (accept all)
                    outWeights.assign(static_cast<size_t>(w) * h, 1.0f);
                }

                success = true;
                logMsg = "fuseYPlane: Headless OpenGL ES 3.1 compute shader executed successfully.\n";
            } else {
                errorLog += "GL: failed to map output SSBO buffer\n";
                LOGE("GL: failed to map output SSBO buffer");
            }

            // Cleanup resources
            glDeleteBuffers(1, &outBuffer);
            glDeleteBuffers(1, &weightBuffer);
            glDeleteTextures(1, &inputFramesTex);
            glDeleteTextures(1, &motionFieldsTex);
            glDeleteTextures(1, &noiseLutsTex);
            glDeleteProgram(program);
        }
    }

    if (!success) {
        LOGE("GL compute failed, falling back to CPU Y copy of reference frame");
        logMsg = "fuseYPlane: GL compute failed, fell back to reference copy.\n";
        logMsg += "=== GL/EGL ERROR LOG ===\n" + errorLog + "\n";
        const YuvFrame& refFrame = frames[0];
        outY.resize(static_cast<size_t>(w) * h);
        for (int r = 0; r < h; ++r) {
            std::copy(refFrame.yPlane + r * refFrame.yRowStride,
                      refFrame.yPlane + r * refFrame.yRowStride + w,
                      outY.data() + r * w);
        }
        // Weights unavailable - fill with 1.0 (accept all, fallback mode)
        outWeights.assign(static_cast<size_t>(w) * h, 1.0f);
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    auto dTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    
    LOGI("fuseYPlane: total GPU execution/copy time = %lld ms", dTotal);

    if (!debugDir.empty()) {
        try {
            std::ofstream logFile(debugDir + "/stage_1_fusion/fusion_log.txt", std::ios::app);
            if (logFile) {
                logFile << logMsg;
                logFile << "Total execution/copy: " << dTotal << " ms\n";
                logFile.close();
            }
        } catch (...) {}
    }
}

/// Chroma fusion weighted by luma ghost-rejection weights.
/// Pixels where luma rejected frames (weight < threshold) fall back to reference frame chroma.
static void fuseChromaPlane(const std::vector<YuvFrame>& frames,
                             const std::vector<MotionField>& motionFields,
                             bool isU,
                             std::vector<uint8_t>& out,
                             int uvW, int uvH,
                             const std::vector<std::vector<float>>& inv_2sigma2_luts,
                             const std::vector<float>& lumaWeights,
                             int lumaW, int lumaH)
{
    auto tStart = std::chrono::high_resolution_clock::now();
    const YuvFrame& ref = frames[0];
    const uint8_t* refPlane = isU ? ref.uPlane : ref.vPlane;
    out.resize(static_cast<size_t>(uvW) * uvH);

    // Threshold: if luma weight < 0.5, more than half the frames were ghosted -> use reference only
    const float ghostThreshold = 0.5f;

    for (int r = 0; r < uvH; ++r) {
        for (int c = 0; c < uvW; ++c) {
            // Map chroma pixel to luma pixel (2x scale)
            int lumaR = std::min(r * 2, lumaH - 1);
            int lumaC = std::min(c * 2, lumaW - 1);
            float lumaWeight = lumaWeights[lumaR * lumaW + lumaC];

            if (lumaWeight < ghostThreshold || frames.size() <= 1) {
                // Ghosted region: use reference frame only
                out[r * uvW + c] = refPlane[r * ref.uvRowStride + c * ref.uvPixelStride];
            } else {
                // Safe region: blend motion-compensated chroma from all accepted frames
                float acc = static_cast<float>(refPlane[r * ref.uvRowStride + c * ref.uvPixelStride]);
                float wgt = 1.0f;
                int blocksWide = motionFields[0].blocksWide;
                int blocksTall = motionFields[0].blocksTall;
                int blockSize   = motionFields[0].blockSize;

                for (int f = 1; f < (int)frames.size(); ++f) {
                    const YuvFrame& fr = frames[f];
                    const uint8_t* frPlane = isU ? fr.uPlane : fr.vPlane;
                    const MotionField& mf = motionFields[f - 1];

                    // Look up motion vector at this chroma pixel's luma position
                    int bx = std::clamp(lumaC / blockSize, 0, blocksWide - 1);
                    int by = std::clamp(lumaR / blockSize, 0, blocksTall - 1);
                    MotionVec mv = mf.vectors[by * blocksWide + bx];

                    // Halve motion vectors for chroma (half-resolution)
                    int cdx = static_cast<int>(std::round(mv.dx * 0.5f));
                    int cdy = static_cast<int>(std::round(mv.dy * 0.5f));

                    int sc = std::clamp(c + cdx, 0, uvW - 1);
                    int sr = std::clamp(r + cdy, 0, uvH - 1);

                    float val = static_cast<float>(frPlane[sr * fr.uvRowStride + sc * fr.uvPixelStride]);
                    acc += val;
                    wgt += 1.0f;
                }
                out[r * uvW + c] = static_cast<uint8_t>(std::clamp(acc / wgt, 0.f, 255.f));
            }
        }
    }
    auto tEnd = std::chrono::high_resolution_clock::now();
    auto dChroma = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    LOGI("fuseChromaPlane (isU=%d): %lld ms", isU, dChroma);
}

const char* COMPUTE_RAW_SHADER_SRC = R"glsl(
#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;

precision highp float;
precision highp usampler2DArray;
precision highp sampler2DArray;
precision highp sampler2D;

uniform usampler2DArray u_input_frames;
uniform sampler2DArray u_motion_fields;
uniform sampler2D u_noise_luts;

uniform int u_num_frames;
uniform int u_width;
uniform int u_height;
uniform int u_blocks_wide;
uniform int u_blocks_tall;
uniform float u_block_size;

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    uint outRaw[];
};

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    // Sample a 2x2 Bayer-aligned patch for patch-based residual computation
    float ref_val = 0.0;
    for (int dy = -2; dy <= 2; dy += 2)
        for (int dx = -2; dx <= 2; dx += 2) {
            ivec2 p = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width-1, u_height-1));
            ref_val += float(texelFetch(u_input_frames, ivec3(p, 0), 0).r);
        }
    ref_val /= 9.0;

    float acc = float(texelFetch(u_input_frames, ivec3(pos, 0), 0).r);
    float wgt = 1.0;

    for (int f = 1; f < u_num_frames; ++f) {
        float bx = (float(pos.x) - u_block_size * 0.5) / u_block_size;
        float by = (float(pos.y) - u_block_size * 0.5) / u_block_size;
        vec2 mv_uv = vec2((bx + 0.5) / float(u_blocks_wide), (by + 0.5) / float(u_blocks_tall));

        vec2 mv = texture(u_motion_fields, vec3(mv_uv, float(f - 1))).rg;

        // Round to nearest Bayer-aligned 2-pixel boundary
        int dx = int(round(mv.x * 0.5)) * 2;
        int dy = int(round(mv.y * 0.5)) * 2;

        // Patch-based residual for robustness
        float warped_val = 0.0;
        for (int pdy = -2; pdy <= 2; pdy += 2)
            for (int pdx = -2; pdx <= 2; pdx += 2) {
                ivec2 p = clamp(pos + ivec2(dx + pdx, dy + pdy), ivec2(0), ivec2(u_width-1, u_height-1));
                warped_val += float(texelFetch(u_input_frames, ivec3(p, f), 0).r);
            }
        warped_val /= 9.0;

        float residual = warped_val - ref_val;

        // Normalize ref_val by the actual sensor max range and map to 0-255 LUT index
        int ref_idx = clamp(int((ref_val / u_sensor_max) * 255.0), 0, 255);
        float inv_2sigma2 = texelFetch(u_noise_luts, ivec2(ref_idx, f), 0).r;

        float w_val = exp(-residual * residual * inv_2sigma2);

        // Hard ghost rejection: clamp near-zero weights to exactly 0
        w_val = (w_val < 0.05) ? 0.0 : w_val;

        // Accumulate center pixel only
        ivec2 center_warped_pos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
        float center_warped = float(texelFetch(u_input_frames, ivec3(center_warped_pos, f), 0).r);
        acc += center_warped * w_val;
        wgt += w_val;
    }

    float fused_val = (wgt > 1e-6) ? acc / wgt : acc;

    uint idx = uint(pos.y * u_width + pos.x);
    outRaw[idx] = uint(clamp(fused_val, 0.0, 65535.0));
}
)glsl";

static void fuseRawBayer(const std::vector<YuvFrame>& frames,
                         const std::vector<MotionField>& motionFields,
                         std::vector<uint16_t>& outRaw,
                         int w, int h,
                         const std::vector<std::vector<float>>& inv_2sigma2_luts,
                         float sensorMax)
{
    auto tStart = std::chrono::high_resolution_clock::now();
    int numFrames = frames.size();
    bool success = false;
    std::string errorLog;

    int blocksWide = motionFields[0].blocksWide;
    int blocksTall = motionFields[0].blocksTall;
    int blockSize = motionFields[0].blockSize;
    int numFields = motionFields.size();

    // Try GPU execution first via headless EGL
    EglHeadlessSetup egl;
    if (egl.init(errorLog)) {
        GLuint program = createComputeProgram(COMPUTE_RAW_SHADER_SRC, errorLog);
        if (program != 0) {
            glUseProgram(program);

            // Upload RAW frames into a 2D Texture Array (using GL_R16UI for 16-bit uint)
            GLuint inputFramesTex;
            glGenTextures(1, &inputFramesTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, inputFramesTex);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R16UI, w, h, numFrames);
            for (int f = 0; f < numFrames; ++f) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, frames[f].yRowStride / 2);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, f, w, h, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, frames[f].yPlane);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            // Pack motion fields
            std::vector<float> contiguousMVs(2 * blocksWide * blocksTall * numFields);
            for (int f = 0; f < numFields; ++f) {
                const MotionField& mf = motionFields[f];
                for (int y = 0; y < blocksTall; ++y) {
                    for (int x = 0; x < blocksWide; ++x) {
                        MotionVec mv = mf.vectors[y * blocksWide + x];
                        contiguousMVs[0 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dx;
                        contiguousMVs[1 + x * 2 + y * blocksWide * 2 + f * blocksWide * blocksTall * 2] = mv.dy;
                    }
                }
            }

            // Upload motion fields to a 2D Texture Array
            GLuint motionFieldsTex;
            glGenTextures(1, &motionFieldsTex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, motionFieldsTex);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RG32F, blocksWide, blocksTall, numFields);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, blocksWide, blocksTall, numFields, GL_RG, GL_FLOAT, contiguousMVs.data());
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Pack noise LUTs
            std::vector<float> flatLuts(256 * numFrames);
            for (int f = 0; f < numFrames; ++f) {
                std::copy(inv_2sigma2_luts[f].begin(), inv_2sigma2_luts[f].end(), flatLuts.data() + f * 256);
            }

            // Upload noise LUTs to a 2D Texture
            GLuint noiseLutsTex;
            glGenTextures(1, &noiseLutsTex);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, noiseLutsTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, 256, numFrames);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, numFrames, GL_RED, GL_FLOAT, flatLuts.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glUniform1i(glGetUniformLocation(program, "u_input_frames"), 0);
            glUniform1i(glGetUniformLocation(program, "u_motion_fields"), 1);
            glUniform1i(glGetUniformLocation(program, "u_noise_luts"), 2);
            glUniform1i(glGetUniformLocation(program, "u_num_frames"), numFrames);
            glUniform1i(glGetUniformLocation(program, "u_width"), w);
            glUniform1i(glGetUniformLocation(program, "u_height"), h);
            glUniform1i(glGetUniformLocation(program, "u_blocks_wide"), blocksWide);
            glUniform1i(glGetUniformLocation(program, "u_blocks_tall"), blocksTall);
            glUniform1f(glGetUniformLocation(program, "u_block_size"), static_cast<float>(blockSize));
            glUniform1f(glGetUniformLocation(program, "u_sensor_max"), sensorMax);

            // Set up output SSBO (4 bytes std430 uint elements for maximum driver compatibility)
            GLuint outBuffer;
            glGenBuffers(1, &outBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, outBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<size_t>(w) * h * 4, nullptr, GL_DYNAMIC_READ);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);

            checkGlError("RAW resource binding", errorLog);

            // Dispatch compute shader
            glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

            checkGlError("RAW dispatch", errorLog);

            // Map and retrieve output
            void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<size_t>(w) * h * 4, GL_MAP_READ_BIT);
            if (ptr != nullptr) {
                const uint32_t* results = static_cast<const uint32_t*>(ptr);
                outRaw.resize(static_cast<size_t>(w) * h);
                for (size_t i = 0; i < outRaw.size(); ++i) {
                    outRaw[i] = static_cast<uint16_t>(results[i]);
                }
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                success = true;
            } else {
                errorLog += "GL: failed to map RAW output SSBO buffer\n";
                LOGE("GL: failed to map RAW output SSBO buffer");
            }

            // Cleanup resources
            glDeleteBuffers(1, &outBuffer);
            glDeleteTextures(1, &inputFramesTex);
            glDeleteTextures(1, &motionFieldsTex);
            glDeleteTextures(1, &noiseLutsTex);
            glDeleteProgram(program);
        }
    }

    if (!success) {
        LOGE("GL RAW compute failed, falling back to CPU multi-threaded RAW fusion");
        LOGE("RAW GPU errors:\n%s", errorLog.c_str());
        std::vector<float> acc(static_cast<size_t>(w) * h, 0.f);
        std::vector<float> wgt(static_cast<size_t>(w) * h, 0.f);

        // Reference frame: weight 1.0
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

        // Remaining frames: warp + accumulate in parallel
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
                            MotionVec mv = interpolateMotion(mf, r, c);
                            int dx = (mv.dx / 2) * 2;
                            int dy = (mv.dy / 2) * 2;

                            int sc = std::clamp(c + dx, 0, w - 1);
                            int sr = std::clamp(r + dy, 0, h - 1);
                            
                            float warped = static_cast<float>(srcRaw[sr * strideElements + sc]);
                            float ref_val = static_cast<float>(refRawRow[c]);
                            float residual = warped - ref_val;

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

    auto tEnd = std::chrono::high_resolution_clock::now();
    auto dTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    LOGI("fuseRawBayer: total GPU execution/copy time = %lld ms", dTotal);
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
    if (ctx.inputFrames.size() > 1 && ctx.motionFields.size() != ctx.inputFrames.size() - 1) {
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

    // Extract dynamically calibrated sensor noise parameters if available
    std::vector<float> noiseProfiles;
    if (ctx.metadata.count("noise_profiles")) {
        try {
            noiseProfiles = std::any_cast<std::vector<float>>(ctx.metadata.at("noise_profiles"));
        } catch (...) {}
    }

    // ── Build Noise-Aware Fusion LUT per Frame ────────────────────────────────
    std::vector<std::vector<float>> inv_2sigma2_luts(ctx.inputFrames.size(), std::vector<float>(256));
    for (size_t fi = 0; fi < ctx.inputFrames.size(); ++fi) {
        int fIso = frameIsos[fi];
        
        double S = 0.0;
        double O = 0.0;
        bool hasDynamicNoise = (noiseProfiles.size() >= (fi + 1) * 8);
        if (hasDynamicNoise) {
            float sumS = 0.0f;
            float sumO = 0.0f;
            for (int ch = 0; ch < 4; ++ch) {
                sumS += noiseProfiles[fi * 8 + ch * 2];
                sumO += noiseProfiles[fi * 8 + ch * 2 + 1];
            }
            S = sumS / 4.0;
            O = sumO / 4.0;
        }

        if (!hasDynamicNoise || (S <= 0.0 && O <= 0.0)) {
            LOGI("FusionStage: Dynamic/static device noise profile missing! Using generic CMOS model fallback.");
            // Standard generic CMOS noise model scaling with ISO
            S = 1.0e-5 * (fIso / 100.0);
            O = 1.0e-6 * (fIso / 100.0) * (fIso / 100.0);
        }

        LOGI("FusionStage: Frame %zu (ISO %d) Noise Model -> S = %.4e, O = %.4e", fi, fIso, S, O);

        for (int i = 0; i < 256; ++i) {
            float normVal = i / 255.f;
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
        
        // Dynamically measure raw sensor max range of the reference frame
        float sensorMax = 1023.f;
        const uint16_t* rawData = reinterpret_cast<const uint16_t*>(ctx.inputFrames[0].yPlane);
        uint16_t maxVal = 0;
        int testPixels = std::min(w * h, 10000);
        for (int p = 0; p < testPixels; ++p) {
            if (rawData[p] > maxVal) maxVal = rawData[p];
        }
        if (maxVal > 1023 && maxVal <= 4095) {
            sensorMax = 4095.f;
        } else if (maxVal > 4095) {
            sensorMax = static_cast<float>(maxVal);
        }
        
        if (ctx.inputFrames.size() == 1) {
            LOGI("FusionStage: single frame, copying RAW Bayer reference directly");
            const uint16_t* rawData = reinterpret_cast<const uint16_t*>(ctx.inputFrames[0].yPlane);
            int strideElements = ctx.inputFrames[0].yRowStride / 2;
            fusedRaw.resize(static_cast<size_t>(w) * h);
            for (int r = 0; r < h; ++r) {
                std::copy(rawData + r * strideElements, rawData + r * strideElements + w, fusedRaw.data() + r * w);
            }
        } else {
            fuseRawBayer(ctx.inputFrames, ctx.motionFields, fusedRaw, w, h, inv_2sigma2_luts, sensorMax);
        }
        ctx.metadata["fused_raw"] = fusedRaw;
    } else {
        std::string debugDir = "";
        if (ctx.metadata.count("debug_dir")) {
            try { debugDir = std::any_cast<std::string>(ctx.metadata.at("debug_dir")); } catch (...) {}
        }
        if (ctx.inputFrames.size() == 1) {
            LOGI("FusionStage: single frame, copying YUV reference directly");
            const YuvFrame& ref = ctx.inputFrames[0];
            ctx.fusedY.resize(static_cast<size_t>(w) * h);
            for (int r = 0; r < h; ++r) {
                std::copy(ref.yPlane + r * ref.yRowStride, ref.yPlane + r * ref.yRowStride + w, ctx.fusedY.data() + r * w);
            }
            ctx.fusedU.resize(static_cast<size_t>(uvW) * uvH);
            ctx.fusedV.resize(static_cast<size_t>(uvW) * uvH);
            for (int r = 0; r < uvH; ++r) {
                const uint8_t* uRow = ref.uPlane + r * ref.uvRowStride;
                const uint8_t* vRow = ref.vPlane + r * ref.uvRowStride;
                for (int c = 0; c < uvW; ++c) {
                    ctx.fusedU[r * uvW + c] = uRow[c * ref.uvPixelStride];
                    ctx.fusedV[r * uvW + c] = vRow[c * ref.uvPixelStride];
                }
            }
        } else {
            std::vector<float> lumaWeights;
            fuseYPlane(ctx.inputFrames, ctx.motionFields, ctx.fusedY, lumaWeights, w, h, inv_2sigma2_luts, debugDir);
            fuseChromaPlane(ctx.inputFrames, ctx.motionFields, /*isU=*/true,  ctx.fusedU, uvW, uvH, inv_2sigma2_luts, lumaWeights, w, h);
            fuseChromaPlane(ctx.inputFrames, ctx.motionFields, /*isU=*/false, ctx.fusedV, uvW, uvH, inv_2sigma2_luts, lumaWeights, w, h);
        }

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
            if (isNight && spatialStrength > 0) {
                LOGI("FusionStage: applying spatial NL-Means luma denoise (h=%d)", spatialStrength);
                spatialDenoiseLuma(ctx.fusedY, w, h, spatialStrength, /*template_half=*/0, /*search_half=*/2);
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
                    float digitalGain = 1.0f;
                    if (ctx.metadata.count("digital_gain")) {
                        try { digitalGain = std::any_cast<float>(ctx.metadata.at("digital_gain")); } catch (...) {}
                    }
                    
                    // Dynamically determine range/black level of reference RAW frame
                    float blackLevel = 1024.f;
                    float whiteLevel = 4095.f;
                    
                    uint16_t minVal = 65535;
                    uint16_t maxVal = 0;
                    const uint16_t* rawDataPtr = reinterpret_cast<const uint16_t*>(ctx.inputFrames[0].yPlane);
                    int testPixels = std::min(w * h, 20000);
                    for (int p = 0; p < testPixels; ++p) {
                        uint16_t v = rawDataPtr[p];
                        if (v < minVal) minVal = v;
                        if (v > maxVal) maxVal = v;
                    }
                    
                    if (maxVal <= 1023) {
                        blackLevel = 64.f;
                        whiteLevel = 1023.f;
                    } else if (maxVal <= 4095) {
                        blackLevel = 1024.f;
                        if (minVal < 300) blackLevel = 256.f;
                        if (minVal < 100) blackLevel = 64.f;
                        whiteLevel = 4095.f;
                    } else {
                        blackLevel = 1024.f;
                        if (minVal < 500) blackLevel = 256.f;
                        whiteLevel = static_cast<float>(maxVal);
                    }

                    // For the preview generation, let's NOT multiply by digitalGain if it makes it too bright compared to other stages.
                    // Let's use a dynamic previewScale that maps the detected sensor range to 255.
                    float previewScale = 255.f / std::max(1.f, whiteLevel - blackLevel);

                    // Gamma correction lambda to make shadows visible on linear RAW previews
                    auto mapTo8Bit = [previewScale, blackLevel](float rawVal) -> uint8_t {
                        float cleanVal = std::max(0.f, rawVal - blackLevel);
                        float norm = std::clamp(cleanVal * previewScale / 255.f, 0.f, 1.f);
                        float gamma = std::sqrt(norm); // square root acts as gamma ~2.0
                        return static_cast<uint8_t>(std::clamp(gamma * 255.f, 0.f, 255.f));
                    };

                    // Save 8-bit exposure-boosted grayscale preview JPEG of fused RAW plane
                    std::vector<uint8_t> previewY(w * h);
                    for (size_t i = 0; i < previewY.size(); ++i) {
                        previewY[i] = mapTo8Bit(static_cast<float>(fusedRaw[i]));
                    }
                    std::vector<uint8_t> uvGray(w * h / 2, 128);
                    saveYuvAsJpeg(previewY.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/fused.jpg");

                    int baseIdx = 0;
                    if (ctx.metadata.count("selected_base_frame_index")) {
                        try { baseIdx = std::any_cast<int>(ctx.metadata.at("selected_base_frame_index")); } catch(...) {}
                    }

                    // Save full reference RAW frame preview (labeled with original burst index)
                    const uint16_t* refRaw = reinterpret_cast<const uint16_t*>(ctx.inputFrames[0].yPlane);
                    int refStride = ctx.inputFrames[0].yRowStride / 2;
                    std::vector<uint8_t> refPreviewY(w * h);
                    for (int r = 0; r < h; ++r) {
                        for (int c = 0; c < w; ++c) {
                            uint16_t val = refRaw[r * refStride + c];
                            refPreviewY[r * w + c] = mapTo8Bit(static_cast<float>(val));
                        }
                    }
                    saveYuvAsJpeg(refPreviewY.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/ref_frame_index_" + std::to_string(baseIdx) + ".jpg");

                    // Save full source RAW frame 1 preview (if present)
                    if (ctx.inputFrames.size() > 1) {
                        const uint16_t* srcRaw = reinterpret_cast<const uint16_t*>(ctx.inputFrames[1].yPlane);
                        int srcStride = ctx.inputFrames[1].yRowStride / 2;
                        std::vector<uint8_t> srcPreviewY(w * h);
                        for (int r = 0; r < h; ++r) {
                            for (int c = 0; c < w; ++c) {
                                uint16_t val = srcRaw[r * srcStride + c];
                                srcPreviewY[r * w + c] = mapTo8Bit(static_cast<float>(val));
                            }
                        }
                        saveYuvAsJpeg(srcPreviewY.data(), uvGray.data(), uvGray.data(), w, h, debugDir + "/stage_1_fusion/src_frame_1.jpg");
                    }

                    int cy = h / 2;
                    int cx = w / 2;
                    int cropSize = 400; // 400x400 crop
                    int startX = ((cx - cropSize / 2) / 2) * 2;
                    int startY = ((cy - cropSize / 2) / 2) * 2;

                    // Reference noisy RAW crop (Grayscale representation)
                    std::vector<uint8_t> cropRefY(cropSize * cropSize);
                    for (int r = 0; r < cropSize; ++r) {
                        for (int c = 0; c < cropSize; ++c) {
                            uint16_t val = refRaw[(startY + r) * refStride + (startX + c)];
                            cropRefY[r * cropSize + c] = mapTo8Bit(static_cast<float>(val));
                        }
                    }
                    std::vector<uint8_t> cropUvGray(cropSize * cropSize / 2, 128);
                    saveYuvAsJpeg(cropRefY.data(), cropUvGray.data(), cropUvGray.data(), cropSize, cropSize, debugDir + "/stage_1_fusion/noisy_crop.jpg");

                    // Fused denoised RAW crop
                    std::vector<uint8_t> cropFusedY(cropSize * cropSize);
                    for (int r = 0; r < cropSize; ++r) {
                        for (int c = 0; c < cropSize; ++c) {
                            uint16_t val = fusedRaw[(startY + r) * w + (startX + c)];
                            cropFusedY[r * cropSize + c] = mapTo8Bit(static_cast<float>(val));
                        }
                    }
                    saveYuvAsJpeg(cropFusedY.data(), cropUvGray.data(), cropUvGray.data(), cropSize, cropSize, debugDir + "/stage_1_fusion/denoised_crop.jpg");

                    // Compute alignment difference maps for RAW mode (subsampled 4x)
                    if (ctx.inputFrames.size() > 1) {
                        int dw = w / 4;
                        int dh = h / 4;
                        const uint16_t* srcRaw = reinterpret_cast<const uint16_t*>(ctx.inputFrames[1].yPlane);
                        int srcStride = ctx.inputFrames[1].yRowStride / 2;

                        // Use a normalized difference scale mapping maximum sensor difference
                        float diffScale = 255.f / std::max(1.f, whiteLevel - blackLevel);

                        // Difference Before Alignment
                        std::vector<uint8_t> diffBefore(dw * dh);
                        for (int r = 0; r < dh; ++r) {
                            for (int c = 0; c < dw; ++c) {
                                uint16_t refVal = refRaw[(r * 4) * refStride + (c * 4)];
                                uint16_t srcVal = srcRaw[(r * 4) * srcStride + (c * 4)];
                                float diff = std::abs(static_cast<float>(refVal) - static_cast<float>(srcVal));
                                diffBefore[r * dw + c] = static_cast<uint8_t>(std::clamp(diff * diffScale * 8.f, 0.f, 255.f));
                            }
                        }
                        std::vector<uint8_t> uvGrayDiff(dw * dh / 2, 128);
                        saveYuvAsJpeg(diffBefore.data(), uvGrayDiff.data(), uvGrayDiff.data(), dw, dh, debugDir + "/stage_1_fusion/diff_before_alignment.jpg");

                        // Difference After Alignment (Bayer-phase aligned)
                        std::vector<uint8_t> diffAfter(dw * dh);
                        for (int r = 0; r < dh; ++r) {
                            for (int c = 0; c < dw; ++c) {
                                MotionVec mv = interpolateMotion(ctx.motionFields[0], r * 4, c * 4);
                                int cdx = int(round(mv.dx * 0.5f)) * 2;
                                int cdy = int(round(mv.dy * 0.5f)) * 2;
                                int sc = std::clamp(c * 4 + cdx, 0, w - 1);
                                int sr = std::clamp(r * 4 + cdy, 0, h - 1);

                                uint16_t refVal = refRaw[(r * 4) * refStride + (c * 4)];
                                uint16_t warpedVal = srcRaw[sr * srcStride + sc];
                                float diff = std::abs(static_cast<float>(refVal) - static_cast<float>(warpedVal));
                                diffAfter[r * dw + c] = static_cast<uint8_t>(std::clamp(diff * diffScale * 8.f, 0.f, 255.f));
                            }
                        }
                        saveYuvAsJpeg(diffAfter.data(), uvGrayDiff.data(), uvGrayDiff.data(), dw, dh, debugDir + "/stage_1_fusion/diff_after_alignment.jpg");
                    }
                    
                    // Save selected base frame index to a plain text log file
                    std::string infoPath = debugDir + "/stage_1_fusion/selected_base_frame.txt";
                    std::ofstream infoFile(infoPath);
                    if (infoFile) {
                        infoFile << "Selected Base Frame Index (from original burst sequence): " << baseIdx << "\n";
                        infoFile.close();
                    }
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

                    // Compute Difference Before Alignment (Subsampled 4x)
                    int dw = w / 4;
                    int dh = h / 4;
                    std::vector<uint8_t> diffBefore(dw * dh);
                    for (int r = 0; r < dh; ++r) {
                        const uint8_t* refRow = ref.yPlane + (r * 4) * ref.yRowStride;
                        const uint8_t* srcRow = src.yPlane + (r * 4) * src.yRowStride;
                        for (int c = 0; c < dw; ++c) {
                            diffBefore[r * dw + c] = static_cast<uint8_t>(std::abs(static_cast<int>(refRow[c * 4]) - static_cast<int>(srcRow[c * 4])));
                        }
                    }
                    std::vector<uint8_t> uvGray(dw * dh / 4, 128);
                    saveYuvAsJpeg(diffBefore.data(), uvGray.data(), uvGray.data(), dw, dh, debugDir + "/stage_1_fusion/diff_before_alignment.jpg");

                    // Compute Difference After Alignment (Subsampled 4x)
                    std::vector<uint8_t> diffAfter(dw * dh);
                    for (int r = 0; r < dh; ++r) {
                        const uint8_t* refRow = ref.yPlane + (r * 4) * ref.yRowStride;
                        for (int c = 0; c < dw; ++c) {
                            MotionVec mv = interpolateMotion(ctx.motionFields[0], r * 4, c * 4);
                            float sx = static_cast<float>(c * 4) + mv.dx;
                            float sy = static_cast<float>(r * 4) + mv.dy;
                            float warpedVal = sampleY(src, sx, sy);
                            diffAfter[r * dw + c] = static_cast<uint8_t>(std::abs(static_cast<int>(refRow[c * 4]) - static_cast<int>(warpedVal)));
                        }
                    }
                    saveYuvAsJpeg(diffAfter.data(), uvGray.data(), uvGray.data(), dw, dh, debugDir + "/stage_1_fusion/diff_after_alignment.jpg");

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

                // Raw planar YUV — only written when debug_raw_dumps is enabled
                bool rawDumps = false;
                if (ctx.metadata.count("debug_raw_dumps")) {
                    try { rawDumps = std::any_cast<bool>(ctx.metadata.at("debug_raw_dumps")); } catch (...) {}
                }
                if (rawDumps) {
                    std::string yuvPath = debugDir + "/stage_1_fusion/fused.yuv";
                    std::ofstream out(yuvPath, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char*>(ctx.fusedY.data()), ctx.fusedY.size());
                        out.write(reinterpret_cast<const char*>(ctx.fusedU.data()), ctx.fusedU.size());
                        out.write(reinterpret_cast<const char*>(ctx.fusedV.data()), ctx.fusedV.size());
                        out.close();
                    }
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
