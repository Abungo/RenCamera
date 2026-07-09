#include "encode_stage.h"

#include <any>
#include <cstdlib>
#include <string>

// libjpeg-turbo (fetched by CMake FetchContent)
#include <jpeglib.h>

// ─────────────────────────────────────────────────────────────────────────────
// EncodeStage::process
// ─────────────────────────────────────────────────────────────────────────────
bool EncodeStage::process(FrameContext& ctx) {
    if (ctx.processedImage.data.empty()) {
        // Fall back to colorImage if tonemap stage was skipped
        if (ctx.colorImage.data.empty()) {
            LOGE("EncodeStage: no image to encode");
            return false;
        }
    }

    const RgbImage& src = ctx.processedImage.data.empty()
                            ? ctx.colorImage
                            : ctx.processedImage;

    // Read quality from pipeline metadata (default 95)
    int quality = 95;
    auto it = ctx.metadata.find("jpeg_quality");
    if (it != ctx.metadata.end()) {
        try { quality = std::any_cast<int>(it->second); } catch (...) {}
    }
    quality = std::clamp(quality, 50, 100);

    // ── libjpeg-turbo compress ────────────────────────────────────────────────
    struct jpeg_compress_struct cinfo{};
    struct jpeg_error_mgr       jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned long jpegSize = 0;
    uint8_t*      jpegBuf  = nullptr;
    jpeg_mem_dest(&cinfo, &jpegBuf, &jpegSize);

    cinfo.image_width      = static_cast<JDIMENSION>(src.width);
    cinfo.image_height     = static_cast<JDIMENSION>(src.height);
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    std::vector<JSAMPROW> rowPointers(src.height);
    for (int y = 0; y < src.height; ++y) {
        rowPointers[y] = const_cast<JSAMPLE*>(src.data.data() + y * src.width * 3);
    }
    jpeg_write_scanlines(&cinfo, rowPointers.data(), src.height);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    // Move JPEG bytes into context
    ctx.jpegOutput.assign(jpegBuf, jpegBuf + jpegSize);
    std::free(jpegBuf);

    LOGI("EncodeStage: encoded %d×%d → %zu bytes (q=%d)",
         src.width, src.height, ctx.jpegOutput.size(), quality);
    return true;
}
