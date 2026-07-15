#include "defect_pixel_stage.h"
#include "../debug_utils.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <any>

bool DefectPixelStage::process(FrameContext& ctx) {
    bool useRaw = false;
    if (ctx.metadata.count("use_raw_capture")) {
        try {
            useRaw = std::any_cast<bool>(ctx.metadata.at("use_raw_capture"));
        } catch (...) {}
    }

    // Defect pixel correction only applies to RAW sensor frames
    if (!useRaw || ctx.inputFrames.empty()) {
        return true;
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    for (size_t fIdx = 0; fIdx < ctx.inputFrames.size(); ++fIdx) {
        const YuvFrame& f = ctx.inputFrames[fIdx];
        uint16_t* rawData = const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(f.yPlane));
        int strideElements = f.yRowStride / 2;
        int w = f.width;
        int h = f.height;

        // Perform Defect Pixel Correction (DPC)
        // 8-neighbor same-color search at offset of 2 pixels (since Bayer layout has R/G/B phase 2)
        #pragma omp parallel for collapse(2)
        for (int y = 2; y < h - 2; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                uint16_t* centerPtr = rawData + y * strideElements + x;
                uint16_t val = *centerPtr;

                // Query 8 same-color neighbors (Bayer phase offset = 2)
                uint16_t n0 = *(centerPtr - 2 * strideElements - 2);
                uint16_t n1 = *(centerPtr - 2 * strideElements);
                uint16_t n2 = *(centerPtr - 2 * strideElements + 2);
                uint16_t n3 = *(centerPtr - 2);
                uint16_t n4 = *(centerPtr + 2);
                uint16_t n5 = *(centerPtr + 2 * strideElements - 2);
                uint16_t n6 = *(centerPtr + 2 * strideElements);
                uint16_t n7 = *(centerPtr + 2 * strideElements + 2);

                // Find min and max of same-color neighbors
                uint16_t minVal = n0;
                uint16_t maxVal = n0;

                #define UPDATE_MIN_MAX(n) \
                    if ((n) < minVal) minVal = (n); \
                    if ((n) > maxVal) maxVal = (n);

                UPDATE_MIN_MAX(n1);
                UPDATE_MIN_MAX(n2);
                UPDATE_MIN_MAX(n3);
                UPDATE_MIN_MAX(n4);
                UPDATE_MIN_MAX(n5);
                UPDATE_MIN_MAX(n6);
                UPDATE_MIN_MAX(n7);

                #undef UPDATE_MIN_MAX

                // Defect threshold: 256. If a pixel is 256 values away from neighbor range, it is stuck.
                int threshold = 256;
                if (val > maxVal + threshold) {
                    *centerPtr = maxVal; // Hot pixel
                } else if (val < minVal - threshold) {
                    *centerPtr = minVal; // Dead pixel
                }
            }
        }
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    LOGI("DefectPixelStage: corrected stuck/dead pixels across %zu RAW frames in %lld ms", ctx.inputFrames.size(), elapsed);

    return true;
}
