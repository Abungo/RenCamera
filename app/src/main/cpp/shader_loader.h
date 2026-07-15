#pragma once
#include <string>

namespace BuiltInShaders {

const std::string SABRE_DEBAYER = R"glsl(
#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;

precision highp float;
precision highp usampler2DArray;
precision highp sampler2DArray;

uniform usampler2DArray u_raw_frames;
uniform sampler2DArray u_motion_fields;

uniform int u_num_frames;
uniform int u_width;
uniform int u_height;
uniform int u_blocks_wide;
uniform int u_blocks_tall;
uniform float u_block_size;

uniform float u_black_level;
uniform float u_scale;
uniform float u_raw_range_sigma;
uniform float u_r_gain;
uniform float u_g_gain;
uniform float u_b_gain;
uniform int u_cfa_pattern;

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    uint outRGB[];
};

int getPixelColor(ivec2 p) {
    int r = p.y % 2;
    int c = p.x % 2;
    if (u_cfa_pattern == 0) { // RGGB
        if (r == 0 && c == 0) return 0;
        if (r == 1 && c == 1) return 2;
        return 1;
    } else if (u_cfa_pattern == 1) { // GRBG
        if (r == 0 && c == 1) return 0;
        if (r == 1 && c == 0) return 2;
        return 1;
    } else if (u_cfa_pattern == 2) { // GBRG
        if (r == 1 && c == 0) return 0;
        if (r == 0 && c == 1) return 2;
        return 1;
    } else { // BGGR (3)
        if (r == 1 && c == 1) return 0;
        if (r == 0 && c == 0) return 2;
        return 1;
    }
}

float getRawCorrected(ivec2 p, int frameIdx) {
    int cx = clamp(p.x, 0, u_width - 1);
    int cy = clamp(p.y, 0, u_height - 1);
    uint val = texelFetch(u_raw_frames, ivec3(cx, cy, frameIdx), 0).r;
    float centerCleanVal = max(0.0, (float(val) - u_black_level) * u_scale);

    // Bayer spatial bilateral filter on same-color pixels
    float sumVal = 0.0;
    float sumW = 0.0;
    
    float rSigma2 = 2.0 * u_raw_range_sigma * u_raw_range_sigma;
    float sSigma2 = 2.0 * 1.5 * 1.5;

    for (int dy = -2; dy <= 2; dy += 2) {
        for (int dx = -2; dx <= 2; dx += 2) {
            ivec2 np = clamp(p + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
            uint nRawVal = texelFetch(u_raw_frames, ivec3(np.x, np.y, frameIdx), 0).r;
            float nCleanVal = max(0.0, (float(nRawVal) - u_black_level) * u_scale);

            float diff = nCleanVal - centerCleanVal;
            float dS2 = float((dx/2)*(dx/2) + (dy/2)*(dy/2));
            float w = exp(-dS2 / sSigma2) * exp(-diff*diff / rSigma2);

            sumVal += w * nCleanVal;
            sumW += w;
        }
    }
    float cleanVal = (sumW > 1e-4) ? (sumVal / sumW) : centerCleanVal;

    int color = getPixelColor(p);
    if (color == 0) {
        return cleanVal * u_r_gain;
    } else if (color == 2) {
        return cleanVal * u_b_gain;
    } else {
        return cleanVal * u_g_gain;
    }
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    // Edge-adaptive kernel: compute horizontal and vertical green gradients on reference frame
    float gLeft = getRawCorrected(pos + ivec2(-1, 0), 0);
    float gRight = getRawCorrected(pos + ivec2(1, 0), 0);
    float gUp = getRawCorrected(pos + ivec2(0, -1), 0);
    float gDown = getRawCorrected(pos + ivec2(0, 1), 0);
    float edgeStrength = abs(gLeft - gRight) + abs(gUp - gDown);
    // Ramps from 3.5 (flat regions, strong denoising) to 8.0 (sharp edges, detail preservation)
    float kExponent = mix(3.5, 8.0, clamp(edgeStrength / 15.0, 0.0, 1.0));

    float sumR = 0.0, weightR = 0.0;
    float sumG = 0.0, weightG = 0.0;
    float sumB = 0.0, weightB = 0.0;

    // 1. Reference frame (Frame 0, pre-fused) local neighborhood contribution
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 samplePos = pos + ivec2(dx, dy);
            if (samplePos.x >= 0 && samplePos.x < u_width && samplePos.y >= 0 && samplePos.y < u_height) {
                float val = getRawCorrected(samplePos, 0);
                int color = getPixelColor(samplePos);
                
                float dist2 = float(dx * dx + dy * dy);
                float spatialW = exp(-dist2 * kExponent);

                if (color == 0) {
                    sumR += val * spatialW;
                    weightR += spatialW;
                } else if (color == 1) {
                    sumG += val * spatialW;
                    weightG += spatialW;
                } else {
                    sumB += val * spatialW;
                    weightB += spatialW;
                }
            }
        }
    }

    // 2. Subpixel multi-frame accumulation (with noise weight filtering)
    float noiseTolerance = 12.f / u_scale; // scaled to RAW pixel domain (12.f out of 255.f)
    for (int f = 1; f < u_num_frames; ++f) {
        float bx = (float(pos.x) - u_block_size * 0.5) / u_block_size;
        float by = (float(pos.y) - u_block_size * 0.5) / u_block_size;
        vec2 mv_uv = vec2((bx + 0.5) / float(u_blocks_wide), (by + 0.5) / float(u_blocks_tall));

        vec2 mv = texture(u_motion_fields, vec3(mv_uv, float(f - 1))).rg;
        vec2 targetPos = vec2(pos) + mv;
        ivec2 centerIdx = ivec2(round(targetPos.x), round(targetPos.y));

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ivec2 samplePos = centerIdx + ivec2(dx, dy);
                if (samplePos.x >= 0 && samplePos.x < u_width && samplePos.y >= 0 && samplePos.y < u_height) {
                    float val = getRawCorrected(samplePos, f);
                    
                    // Compare against reference pixel to reject noise and motion mismatches
                    ivec2 refSamplePos = pos + ivec2(dx, dy);
                    float refVal = getRawCorrected(clamp(refSamplePos, ivec2(0), ivec2(u_width - 1, u_height - 1)), 0);
                    float diff = abs(val - refVal);
                    
                    // Smoothly reject unaligned/noisy pixels
                    float weightFactor = exp(-diff * diff / (2.f * noiseTolerance * noiseTolerance));
                    if (weightFactor < 0.2) continue; // Tightened threshold from 0.1 to 0.2

                    int color = getPixelColor(samplePos);

                    vec2 delta = vec2(samplePos) - targetPos;
                    float dist2 = dot(delta, delta);
                    float spatialW = exp(-dist2 * kExponent) * weightFactor;

                    if (color == 0) {
                        sumR += val * spatialW;
                        weightR += spatialW;
                    } else if (color == 1) {
                        sumG += val * spatialW;
                        weightG += spatialW;
                    } else {
                        sumB += val * spatialW;
                        weightB += spatialW;
                    }
                }
            }
        }
    }

    float finalR = (weightR > 0.01) ? (sumR / weightR) : 0.0;
    float finalG = (weightG > 0.01) ? (sumG / weightG) : 0.0;
    float finalB = (weightB > 0.01) ? (sumB / weightB) : 0.0;

    // Fallback to high-quality local demosaicing if any channel lacks enough samples
    bool needR = (weightR <= 0.2);
    bool needG = (weightG <= 0.2);
    bool needB = (weightB <= 0.2);

    if (needR || needG || needB) {
        int r = pos.y;
        int c = pos.x;
        bool isEvenRow = (r % 2 == 0);
        bool isEvenCol = (c % 2 == 0);

        float fallbackR = 0.0;
        float fallbackG = 0.0;
        float fallbackB = 0.0;

        if (isEvenRow && isEvenCol) {
            fallbackB = getRawCorrected(pos, 0);
            fallbackG = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0) + getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.25 + 
                        (4.0 * fallbackB - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0) - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
            fallbackR = (getRawCorrected(ivec2(c-1, r-1), 0) + getRawCorrected(ivec2(c+1, r-1), 0) + getRawCorrected(ivec2(c-1, r+1), 0) + getRawCorrected(ivec2(c+1, r+1), 0)) * 0.25 + 
                        (6.0 * fallbackB - 1.5 * (getRawCorrected(ivec2(c-2, r), 0) + getRawCorrected(ivec2(c+2, r), 0) + getRawCorrected(ivec2(c, r-2), 0) + getRawCorrected(ivec2(c, r+2), 0))) * 0.0625;
        } else if (!isEvenRow && !isEvenCol) {
            fallbackR = getRawCorrected(pos, 0);
            fallbackG = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0) + getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.25 + 
                        (4.0 * fallbackR - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0) - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
            fallbackB = (getRawCorrected(ivec2(c-1, r-1), 0) + getRawCorrected(ivec2(c+1, r-1), 0) + getRawCorrected(ivec2(c-1, r+1), 0) + getRawCorrected(ivec2(c+1, r+1), 0)) * 0.25 + 
                        (6.0 * fallbackR - 1.5 * (getRawCorrected(ivec2(c-2, r), 0) + getRawCorrected(ivec2(c+2, r), 0) + getRawCorrected(ivec2(c, r-2), 0) + getRawCorrected(ivec2(c, r+2), 0))) * 0.0625;
        } else if (isEvenRow && !isEvenCol) {
            fallbackG = getRawCorrected(pos, 0);
            fallbackB = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0)) * 0.5 + 
                        (2.0 * fallbackG - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0)) * 0.125;
            fallbackR = (getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.5 + 
                        (2.0 * fallbackG - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
        } else {
            fallbackG = getRawCorrected(pos, 0);
            fallbackR = (getRawCorrected(ivec2(c-1, r), 0) + getRawCorrected(ivec2(c+1, r), 0)) * 0.5 + 
                        (2.0 * fallbackG - getRawCorrected(ivec2(c-2, r), 0) - getRawCorrected(ivec2(c+2, r), 0)) * 0.125;
            fallbackB = (getRawCorrected(ivec2(c, r-1), 0) + getRawCorrected(ivec2(c, r+1), 0)) * 0.5 + 
                        (2.0 * fallbackG - getRawCorrected(ivec2(c, r-2), 0) - getRawCorrected(ivec2(c, r+2), 0)) * 0.125;
        }

        if (needR) finalR = fallbackR;
        if (needG) finalG = fallbackG;
        if (needB) finalB = fallbackB;
    }

    vec3 sensorRgb = vec3(finalR, finalG, finalB);

    uint uR = uint(clamp(sensorRgb.r, 0.0, 255.0));
    uint uG = uint(clamp(sensorRgb.g, 0.0, 255.0));
    uint uB = uint(clamp(sensorRgb.b, 0.0, 255.0));

    uint packedVal = uR | (uG << 8) | (uB << 16) | (255u << 24);
    outRGB[pos.y * u_width + pos.x] = packedVal;
}
)glsl";

const std::string NLM_DENOISE = R"glsl(
#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;

precision highp float;
precision highp sampler2D;

uniform sampler2D u_input_texture;
uniform int u_width;
uniform int u_height;
uniform float u_S;
uniform float u_O;
uniform float u_strength_multiplier;

layout(rgba8, binding = 0) writeonly uniform image2D u_output_image;

float luma(vec3 rgb) {
    return 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    float sumW = 0.0;
    vec3 sumRGB = vec3(0.0);

    int search_r = 2; // 5x5 search window
    int patch_r = 1;  // 3x3 patch

    vec3 centerColor = texelFetch(u_input_texture, pos, 0).rgb;
    float centerL = luma(centerColor);

    // Parabolic boost curve to strengthen NLM in both shadows and highlights
    float boost = 1.5 - 2.0 * centerL * (1.0 - centerL);
    float h2 = (u_S * centerL + u_O) * u_strength_multiplier * boost;
    if (h2 < 1e-6) h2 = 1e-6;

    // ── Texture-Aware Variance Gate ───────────────────────────────────────
    // Compute local 3x3 variance. Text/detail has high variance >> noise level.
    // Only denoise where localVar ≈ noise variance (flat noisy region).
    float sumLp = 0.0, sumLp2 = 0.0;
    for (int py = -1; py <= 1; ++py) {
        for (int px = -1; px <= 1; ++px) {
            ivec2 pp = clamp(pos + ivec2(px, py), ivec2(0), ivec2(u_width - 1, u_height - 1));
            float pL = luma(texelFetch(u_input_texture, pp, 0).rgb);
            sumLp  += pL;
            sumLp2 += pL * pL;
        }
    }
    float localMean = sumLp / 9.0;
    float localVar  = max(0.0, sumLp2 / 9.0 - localMean * localMean);

    // textureGate → 1.0 in flat noisy areas, → 0.0 in textured/detail areas
    float textureGate = exp(-localVar / (h2 * 0.5));
    float effectiveH2 = h2 * max(0.05, textureGate);

    for (int dy = -search_r; dy <= search_r; ++dy) {
        for (int dx = -search_r; dx <= search_r; ++dx) {
            ivec2 nPos = pos + ivec2(dx, dy);
            nPos = clamp(nPos, ivec2(0), ivec2(u_width - 1, u_height - 1));

            float ssd = 0.0;
            for (int py = -patch_r; py <= patch_r; ++py) {
                for (int px = -patch_r; px <= patch_r; ++px) {
                    ivec2 p1 = clamp(pos + ivec2(px, py), ivec2(0), ivec2(u_width - 1, u_height - 1));
                    ivec2 p2 = clamp(nPos + ivec2(px, py), ivec2(0), ivec2(u_width - 1, u_height - 1));

                    float l1 = luma(texelFetch(u_input_texture, p1, 0).rgb);
                    float l2 = luma(texelFetch(u_input_texture, p2, 0).rgb);

                    float diff = l1 - l2;
                    ssd += diff * diff;
                }
            }

            // Spatial-bilateral NLM using texture-gated effectiveH2
            float w = exp(-(ssd / 9.0) / effectiveH2) * exp(-float(dx*dx + dy*dy) / 12.5);
            vec3 nColor = texelFetch(u_input_texture, nPos, 0).rgb;

            sumRGB += w * nColor;
            sumW += w;
        }
    }

    imageStore(u_output_image, pos, vec4(sumRGB / sumW, 1.0));
}
)glsl";

const std::string ADAPTIVE_TONEMAP = R"glsl(
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
uniform float u_ev_compensation;
uniform float u_final_gamma;
uniform float u_chroma_range_sigma;
uniform float u_effective_iso;
uniform float u_exposure_boost;
uniform mat3 u_ccm;

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

    // Dynamically compensate exposure bias (scale = 2^(-EV))
    // Scaled up by user-configurable u_exposure_boost
    float evScale = pow(2.0, -u_ev_compensation) * u_exposure_boost;
    vec3 rgbVal = texelFetch(u_input_texture, pos, 0).rgb * 255.0 * evScale;
    float L = luma(rgbVal);

    // Single-pass bilateral filter on the GPU to extract local low-frequency base layer
    float sumVal = 0.0;
    float sumW = 0.0;
    
    float spatial_sigma2 = 2.0 * 16.0 * 16.0;
    float range_sigma2 = 2.0 * 15.0 * 15.0;

    int radius = 12;
    for (int dy = -radius; dy <= radius; dy += 2) {
        for (int dx = -radius; dx <= radius; dx += 2) {
            ivec2 nPos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
            vec3 nRgb = texelFetch(u_input_texture, nPos, 0).rgb * 255.0 * evScale;
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
    logBase = clamp(logBase, 0.0, 10.0);
    
    float normBase = (pow(2.0, logBase) - 1.0) / 255.0;
    float boostedBase = pow(normBase, u_adaptive_gamma);
    if (boostedBase < u_black_point_clamp) {
        boostedBase = (boostedBase * boostedBase) / u_black_point_clamp;
    }
    float compBase = acesFilm(boostedBase) * 255.0;

    float currentDetailAlpha = u_detail_alpha;
    if (baseL < 80.0) {
        float shadowFactor = baseL / 80.0;
        float minDetail = 1.0;
        if (u_effective_iso >= 1600.0) {
            minDetail = 0.3;
        } else if (u_effective_iso >= 800.0) {
            minDetail = 0.5;
        } else if (u_effective_iso >= 400.0) {
            minDetail = 0.7;
        }
        currentDetailAlpha = minDetail + shadowFactor * (u_detail_alpha - minDetail);
    }

    float logDetail = logL - logBase;
    
    // Sobel-guided texture boost curve: amplify low-amplitude high-frequency details
    // while compressing large-amplitude transitions to prevent haloing.
    float signDetail = sign(logDetail);
    float absDetail = abs(logDetail);
    
    // Micro-contrast exponent curve (exponents < 1.0 lift low-contrast details)
    float detailExponent = 0.85;
    float boostedDetail = signDetail * pow(absDetail, detailExponent) * currentDetailAlpha;
    
    float compLogL = log2(compBase + 1.0) + boostedDetail;
    float compL = clamp(pow(2.0, compLogL) - 1.0, 0.0, 255.0);

    float scale = (L > 0.1) ? compL / L : 1.0;
    scale = min(scale, 10.0);

    // Apply CCM to the scaled center RGB to do YUV conversion in sRGB space
    vec3 srgbVal = u_ccm * (rgbVal * scale);
    float newL = luma(srgbVal);
    float centerLumaUnscaled = luma(u_ccm * (texelFetch(u_input_texture, pos, 0).rgb * 255.0));

    // Convert to YUV (YPbPr) space in sRGB
    float uVal = -0.1687 * srgbVal.r - 0.3313 * srgbVal.g + 0.5 * srgbVal.b;
    float vVal = 0.5 * srgbVal.r - 0.4187 * srgbVal.g - 0.0813 * srgbVal.b;

    // Wide-radius chroma denoiser: Bilateral blur on U & V channels
    // range_sigma FIXED at 15 (tight) — independent of post-tonemap u_chroma_range_sigma.
    // At a text edge (luma diff 50): exp(-2500/450) = 0.004 → no bleed.
    float sumU = 0.0;
    float sumV = 0.0;
    float sumChromaW = 0.0;

    // Local gradient magnitude: high on text edges, low on smooth areas
    float gradX = luma(u_ccm * (texelFetch(u_input_texture, clamp(pos + ivec2(1,0), ivec2(0), ivec2(u_width-1,u_height-1)), 0).rgb * 255.0))
                - luma(u_ccm * (texelFetch(u_input_texture, clamp(pos - ivec2(1,0), ivec2(0), ivec2(u_width-1,u_height-1)), 0).rgb * 255.0));
    float gradY = luma(u_ccm * (texelFetch(u_input_texture, clamp(pos + ivec2(0,1), ivec2(0), ivec2(u_width-1,u_height-1)), 0).rgb * 255.0))
                - luma(u_ccm * (texelFetch(u_input_texture, clamp(pos - ivec2(0,1), ivec2(0), ivec2(u_width-1,u_height-1)), 0).rgb * 255.0));
    float gradMag = sqrt(gradX * gradX + gradY * gradY);
    // edgeGate → 1.0 on flat areas (blur freely), → 0.0 on text/edges (no blur)
    float edgeGate = exp(-gradMag / 20.0);
    // In edge areas: collapse spatial sigma to ~1px. On flat areas: keep 3px sigma (div=18)
    float effectiveSpatialDiv = mix(2.0, 18.0, edgeGate);
    float inTonemapRangeSigma2 = 2.0 * 15.0 * 15.0; // tight — always 15, never elevated

    int chromaRadius = 6; // Large window for flat areas

    for (int dy = -chromaRadius; dy <= chromaRadius; ++dy) {
        for (int dx = -chromaRadius; dx <= chromaRadius; ++dx) {
            ivec2 nPos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));

            // Sample neighboring RGB, map to sRGB, and compute its unscaled luma
            vec3 nRgbUnscaled = texelFetch(u_input_texture, nPos, 0).rgb * 255.0;
            vec3 nSrgbUnscaled = u_ccm * nRgbUnscaled;
            float nLUnscaled = luma(nSrgbUnscaled);

            // Range weight: tight sigma preserves text edge colour boundaries
            float diffL = nLUnscaled - centerLumaUnscaled;
            float wChroma = exp(-float(dx*dx + dy*dy) / effectiveSpatialDiv)
                          * exp(-diffL * diffL / inTonemapRangeSigma2);

            vec3 nSrgb = nSrgbUnscaled * evScale;
            float nU = -0.1687 * nSrgb.r - 0.3313 * nSrgb.g + 0.5 * nSrgb.b;
            float nV = 0.5 * nSrgb.r - 0.4187 * nSrgb.g - 0.0813 * nSrgb.b;

            sumU += wChroma * nU;
            sumV += wChroma * nV;
            sumChromaW += wChroma;
        }
    }
    
    if (sumChromaW > 1e-4) {
        uVal = (sumU / sumChromaW) * scale;
        vVal = (sumV / sumChromaW) * scale;
    }

    // Convert back from YUV to RGB space using denoised U & V (which are already in sRGB)
    // Tone down yellow-red casts specifically in skin tone regions (V > 0.02, U is between -0.15 and -0.01)
    if (vVal > 0.02 && uVal > -0.15 && uVal < -0.01) {
        vVal *= 0.88; // reduce red-yellow excess selectively for skin
        uVal *= 0.95; // tone down green-yellow shift slightly
    }

    vec3 finalRgb;
    finalRgb.r = newL + 1.402 * vVal;
    finalRgb.g = newL - 0.34414 * uVal - 0.71414 * vVal;
    finalRgb.b = newL + 1.772 * uVal;

    if (newL > 0.1) {
        float factor = u_saturation_boost;
        if (newL > 200.0) {
            float t = (newL - 200.0) / (255.0 - 200.0);
            factor = factor * (1.0 - clamp(t, 0.0, 1.0));
        }
        finalRgb = newL + factor * (finalRgb - newL);
    }

    float maxChan = max(finalRgb.r, max(finalRgb.g, finalRgb.b));
    if (maxChan > 255.0) {
        float blendL = luma(finalRgb);
        if (maxChan - blendL > 1e-4) {
            float blend = clamp((255.0 - blendL) / (maxChan - blendL), 0.0, 1.0);
            finalRgb = blendL + blend * (finalRgb - blendL);
        }
    }

    // Apply an ISO-adaptive custom gamma correction to balance brightness
    vec3 gammaRgb = pow(clamp(finalRgb / 255.0, 0.0, 1.0), vec3(1.0 / u_final_gamma)) * 255.0;

    uint uR = uint(clamp(gammaRgb.r, 0.0, 255.0));
    uint uG = uint(clamp(gammaRgb.g, 0.0, 255.0));
    uint uB = uint(clamp(gammaRgb.b, 0.0, 255.0));

    uint packedVal = uR | (uG << 8) | (uB << 16) | (255u << 24);
    outRGB[pos.y * u_width + pos.x] = packedVal;
}
)glsl";

const std::string POST_TONEMAP_DENOISE = R"glsl(
#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;

precision highp float;

layout(std430, binding = 0) readonly buffer InputBuffer {
    uint inRGB[];
};

layout(std430, binding = 1) writeonly buffer OutputBuffer {
    uint outRGB[];
};

uniform int u_width;
uniform int u_height;
uniform float u_spatial_sigma;
uniform float u_range_sigma;
uniform int u_denoise_radius;
uniform float u_luma_h2;

vec3 unpackRGB(uint val) {
    float r = float(val & 0xFFu);
    float g = float((val >> 8) & 0xFFu);
    float b = float((val >> 16) & 0xFFu);
    return vec3(r, g, b);
}

float luma(vec3 rgb) {
    return 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    uint centerVal = inRGB[pos.y * u_width + pos.x];
    vec3 centerRgb = unpackRGB(centerVal);
    float centerL = luma(centerRgb);

    float sumWL = 0.0;
    float sumL = 0.0;
    float sumU = 0.0;
    float sumV = 0.0;
    float sumWC = 0.0;

    // ── Brightness Gate (Only denoise darker regions) ───────────────────
    // Denoise normally below 100 luma. Fade to 0 denoising by 150 luma.
    float brightnessGate = clamp(1.0 - (centerL - 100.0) / 50.0, 0.0, 1.0);

    if (brightnessGate <= 0.0) {
        // Completely skip denoising for brighter areas (like text on white background)
        outRGB[pos.y * u_width + pos.x] = centerVal;
        return;
    }

    float spatial_sigma2 = 2.0 * u_spatial_sigma * u_spatial_sigma;
    float range_sigma2 = 2.0 * u_range_sigma * u_range_sigma;
    // Parabolic boost: stronger at shadows, moderate in midtones
    float centerLNorm = centerL / 255.0;
    float boost = 2.0 - 2.5 * centerLNorm * (1.0 - centerLNorm);
    // Scale denoising parameter h2 by brightnessGate
    float h2 = u_luma_h2 * boost * brightnessGate;
    int search_r = 2; // 5x5 luma NLM window
    int patch_r = 1;  // 3x3 patch

    // ── Texture & Sobel Gradient Gate (Protects low-contrast text) ────────
    // Measure local variance and calculate Sobel gradient magnitudes
    float sumLp = 0.0, sumLp2 = 0.0;
    float gx = 0.0, gy = 0.0;
    
    // Sobel kernels:
    // H: [-1, 0, 1; -2, 0, 2; -1, 0, 1]
    // V: [-1, -2, -1;  0, 0, 0;  1, 2, 1]
    float pL_00 = luma(unpackRGB(inRGB[clamp(pos + ivec2(-1, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2(-1, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    float pL_01 = luma(unpackRGB(inRGB[clamp(pos + ivec2( 0, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2( 0, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    float pL_02 = luma(unpackRGB(inRGB[clamp(pos + ivec2( 1, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2( 1, -1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    
    float pL_10 = luma(unpackRGB(inRGB[clamp(pos + ivec2(-1,  0), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2(-1,  0), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    float pL_11 = luma(unpackRGB(inRGB[pos.y * u_width + pos.x]));
    float pL_12 = luma(unpackRGB(inRGB[clamp(pos + ivec2( 1,  0), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2( 1,  0), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    
    float pL_20 = luma(unpackRGB(inRGB[clamp(pos + ivec2(-1,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2(-1,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    float pL_21 = luma(unpackRGB(inRGB[clamp(pos + ivec2( 0,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2( 0,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));
    float pL_22 = luma(unpackRGB(inRGB[clamp(pos + ivec2( 1,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).y * u_width + clamp(pos + ivec2( 1,  1), ivec2(0), ivec2(u_width - 1, u_height - 1)).x]));

    gx = (pL_02 + 2.0 * pL_12 + pL_22) - (pL_00 + 2.0 * pL_10 + pL_20);
    gy = (pL_20 + 2.0 * pL_21 + pL_22) - (pL_00 + 2.0 * pL_01 + pL_02);
    float edgeMagnitude = sqrt(gx * gx + gy * gy);

    // Compute 3x3 variance
    sumLp = pL_00 + pL_01 + pL_02 + pL_10 + pL_11 + pL_12 + pL_20 + pL_21 + pL_22;
    sumLp2 = pL_00*pL_00 + pL_01*pL_01 + pL_02*pL_02 + pL_10*pL_10 + pL_11*pL_11 + pL_12*pL_12 + pL_20*pL_20 + pL_21*pL_21 + pL_22*pL_22;
    float localMean = sumLp / 9.0;
    float localVar  = max(0.0, sumLp2 / 9.0 - localMean * localMean);

    // textureGate based on variance
    float textureGate = exp(-localVar / (h2 * 2.0));
    
    // edgeGate scales down denoising on structured low-contrast Sobel edges
    float edgeGate = clamp(1.0 - (edgeMagnitude / 5.0), 0.0, 1.0); 

    // Combine texture variance gating and structured edge gating
    float effectiveH2 = h2 * textureGate * edgeGate; 

    if (effectiveH2 < 1e-5) {
        // If no luma denoising is needed, keep the original center luma
        sumL = centerL;
        sumWL = 1.0;
    } else {
        // ── Luma NLM pass (5x5 search) ────────────────────────────────────────
        for (int dy = -search_r; dy <= search_r; ++dy) {
            for (int dx = -search_r; dx <= search_r; ++dx) {
                ivec2 nPos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
                vec3 nRgb = unpackRGB(inRGB[nPos.y * u_width + nPos.x]);
                float nL = luma(nRgb);

                // NLM patch SSD for luma
                float ssd = 0.0;
                for (int py = -patch_r; py <= patch_r; ++py) {
                    for (int px = -patch_r; px <= patch_r; ++px) {
                        ivec2 p1 = clamp(pos + ivec2(px, py), ivec2(0), ivec2(u_width - 1, u_height - 1));
                        ivec2 p2 = clamp(nPos + ivec2(px, py), ivec2(0), ivec2(u_width - 1, u_height - 1));
                        float l1 = luma(unpackRGB(inRGB[p1.y * u_width + p1.x]));
                        float l2 = luma(unpackRGB(inRGB[p2.y * u_width + p2.x]));
                        float diff = l1 - l2;
                        ssd += diff * diff;
                    }
                }
                float wL = exp(-(ssd / 9.0) / effectiveH2) * exp(-float(dx*dx + dy*dy) / 12.5);
                sumL += wL * nL;
                sumWL += wL;
            }
        }
    }

    // Pure Gaussian chroma blur — radius narrowed by textureGate in detail areas
    // to prevent colour bleeding across text/edge boundaries
    int chroma_r = 10; // 21x21 window max
    // In flat noisy areas: full 5px sigma. In textured areas: narrow to ~1px sigma.
    float effectiveChromaSigma2 = mix(2.0, 50.0, textureGate);
    for (int dy = -chroma_r; dy <= chroma_r; ++dy) {
        for (int dx = -chroma_r; dx <= chroma_r; ++dx) {
            ivec2 nPos = clamp(pos + ivec2(dx, dy), ivec2(0), ivec2(u_width - 1, u_height - 1));
            vec3 nRgb = unpackRGB(inRGB[nPos.y * u_width + nPos.x]);

            float dS2 = float(dx * dx + dy * dy);
            float wC = exp(-dS2 / effectiveChromaSigma2);
            float nU = -0.1687 * nRgb.r - 0.3313 * nRgb.g + 0.5 * nRgb.b;
            float nV = 0.5 * nRgb.r - 0.4187 * nRgb.g - 0.0813 * nRgb.b;
            sumU += wC * nU;
            sumV += wC * nV;
            sumWC += wC;
        }
    }

    float filteredL = sumL / sumWL;
    float filteredU = sumU / sumWC;
    float filteredV = sumV / sumWC;

    float rVal = filteredL + 1.402 * filteredV;
    float gVal = filteredL - 0.34414 * filteredU - 0.71414 * filteredV;
    float bVal = filteredL + 1.772 * filteredU;

    uint uR = uint(clamp(rVal, 0.0, 255.0));
    uint uG = uint(clamp(gVal, 0.0, 255.0));
    uint uB = uint(clamp(bVal, 0.0, 255.0));

    outRGB[pos.y * u_width + pos.x] = uR | (uG << 8) | (uB << 16) | (255u << 24);
}
)glsl";

}
