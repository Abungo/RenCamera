#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <jpeglib.h>

inline bool saveRgbAsJpeg(const uint8_t* rgbData, int width, int height, const std::string& filePath, int quality = 90) {
    struct jpeg_compress_struct cinfo{};
    struct jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    std::FILE* outfile = std::fopen(filePath.c_str(), "wb");
    if (!outfile) {
        jpeg_destroy_compress(&cinfo);
        return false;
    }
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = static_cast<JDIMENSION>(width);
    cinfo.image_height = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* rowPtr = rgbData + cinfo.next_scanline * width * 3;
        JSAMPROW row = const_cast<JSAMPROW>(rowPtr);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    std::fclose(outfile);
    jpeg_destroy_compress(&cinfo);
    return true;
}

inline bool saveYuvAsJpeg(const uint8_t* y, const uint8_t* u, const uint8_t* v, int width, int height, const std::string& filePath, int quality = 90) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    int uvW = width / 2;
    for (int r = 0; r < height; ++r) {
        const uint8_t* yRow = y + r * width;
        const uint8_t* uRow = u + (r / 2) * uvW;
        const uint8_t* vRow = v + (r / 2) * uvW;
        uint8_t* rgbRow = rgb.data() + r * width * 3;

        for (int x = 0; x < width; ++x) {
            int Y = static_cast<int>(yRow[x]);
            int U = static_cast<int>(uRow[x >> 1]) - 128;
            int V = static_cast<int>(vRow[x >> 1]) - 128;

            // BT.601 YUV -> RGB conversion
            int R = Y + (179 * V + 64) / 128;
            int G = Y - (44 * U + 91 * V + 64) / 128;
            int B = Y + (227 * U + 64) / 128;

            rgbRow[x*3 + 0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
            rgbRow[x*3 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
            rgbRow[x*3 + 2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
        }
    }
    return saveRgbAsJpeg(rgb.data(), width, height, filePath, quality);
}

inline bool saveStridedYuvAsJpeg(
    const uint8_t* y, int yRowStride,
    const uint8_t* u, int uvRowStride,
    const uint8_t* v, int uvPixelStride,
    int width, int height,
    const std::string& filePath, int quality = 90)
{
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int r = 0; r < height; ++r) {
        const uint8_t* yRow = y + r * yRowStride;
        const uint8_t* uRow = u + (r / 2) * uvRowStride;
        const uint8_t* vRow = v + (r / 2) * uvRowStride;
        uint8_t* rgbRow = rgb.data() + r * width * 3;

        for (int x = 0; x < width; ++x) {
            int Y = static_cast<int>(yRow[x]);
            int U = static_cast<int>(uRow[(x >> 1) * uvPixelStride]) - 128;
            int V = static_cast<int>(vRow[(x >> 1) * uvPixelStride]) - 128;

            // BT.601 YUV -> RGB
            int R = Y + (179 * V + 64) / 128;
            int G = Y - (44 * U + 91 * V + 64) / 128;
            int B = Y + (227 * U + 64) / 128;

            rgbRow[x*3 + 0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
            rgbRow[x*3 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
            rgbRow[x*3 + 2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
        }
    }
    return saveRgbAsJpeg(rgb.data(), width, height, filePath, quality);
}
