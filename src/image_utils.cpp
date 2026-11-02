#include "doc2pdf/image_utils.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <zlib.h>

namespace doc2pdf {

bool parseImageDimensions(const uint8_t* data, size_t size, std::string& outFormat, int& outWidth, int& outHeight) {
    if (!data || size < 24) return false;

    // 1. PNG 检查: 89 50 4E 47 0D 0A 1A 0A
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        if (size >= 24 && data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R') {
            outFormat = "png";
            outWidth = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
            outHeight = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
            return outWidth > 0 && outHeight > 0;
        }
    }

    // 2. JPEG 检查: FF D8 FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        size_t i = 2;
        while (i + 9 <= size) {
            if (data[i] != 0xFF) { ++i; continue; }
            uint8_t marker = data[i + 1];
            if (marker == 0xC0 || marker == 0xC2) { // SOF0 或 SOF2
                outFormat = "jpeg";
                outHeight = (data[i + 5] << 8) | data[i + 6];
                outWidth = (data[i + 7] << 8) | data[i + 8];
                return outWidth > 0 && outHeight > 0;
            }
            if (marker == 0xD9 || marker == 0xDA) break;
            uint16_t len = (data[i + 2] << 8) | data[i + 3];
            if (len < 2) break;
            i += 2 + len;
        }
    }

    return false;
}

bool parsePicfHeader(const uint8_t* data, size_t offset, double& outDwPt, double& outDhPt) {
    if (offset < 68) return false;
    size_t searchStart = (offset >= 512) ? (offset - 512) : 0;
    for (size_t h = searchStart; h + 68 <= offset; ++h) {
        uint16_t cbHeader = *(const uint16_t*)&data[h + 4];
        if (cbHeader == 68) {
            uint32_t lcb = *(const uint32_t*)&data[h];
            if (h + lcb >= offset) {
                uint16_t dxaGoal = *(const uint16_t*)&data[h + 28];
                uint16_t dyaGoal = *(const uint16_t*)&data[h + 30];
                uint16_t mx = *(const uint16_t*)&data[h + 32];
                uint16_t my = *(const uint16_t*)&data[h + 34];
                if (mx == 0) mx = 1000;
                if (my == 0) my = 1000;
                if (dxaGoal > 0 && dyaGoal > 0) {
                    outDwPt = (dxaGoal * (mx / 1000.0)) / 20.0;
                    outDhPt = (dyaGoal * (my / 1000.0)) / 20.0;
                    return true;
                }
            }
        }
    }
    return false;
}

static uint8_t paethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

bool decodePngToRgbStream(const uint8_t* pngData, size_t pngSize,
                          int& width, int& height,
                          std::vector<uint8_t>& compressedRgbStream) {
    if (!pngData || pngSize < 33) return false;

    // 解析 IHDR
    width = (pngData[16] << 24) | (pngData[17] << 16) | (pngData[18] << 8) | pngData[19];
    height = (pngData[20] << 24) | (pngData[21] << 16) | (pngData[22] << 8) | pngData[23];
    uint8_t bitDepth = pngData[24];
    uint8_t colorType = pngData[25];

    if (bitDepth != 8 || (colorType != 2 && colorType != 6)) {
        return false;
    }

    int bpp = (colorType == 6) ? 4 : 3;
    size_t rowBytes = width * bpp;

    // 收集所有 IDAT 块
    std::vector<uint8_t> idatBuffer;
    size_t pos = 8;
    while (pos + 8 <= pngSize) {
        uint32_t chunkLen = (pngData[pos] << 24) | (pngData[pos + 1] << 16) | (pngData[pos + 2] << 8) | pngData[pos + 3];
        pos += 4;
        if (pos + 4 > pngSize) break;
        std::string chunkType((const char*)&pngData[pos], 4);
        pos += 4;
        if (chunkType == "IDAT") {
            if (pos + chunkLen > pngSize) break;
            idatBuffer.insert(idatBuffer.end(), &pngData[pos], &pngData[pos + chunkLen]);
        }
        pos += chunkLen + 4; // 跳过数据及 CRC
        if (chunkType == "IEND") break;
    }

    if (idatBuffer.empty()) return false;

    // 解压 IDAT
    size_t expectedRawSize = (size_t)height * (1 + rowBytes);
    std::vector<uint8_t> decompressed(expectedRawSize);
    uLongf destLen = expectedRawSize;
    if (uncompress(decompressed.data(), &destLen, idatBuffer.data(), idatBuffer.size()) != Z_OK) {
        return false;
    }

    // W3C 逐行反滤波重构与 RGBA 白色混合
    std::vector<uint8_t> rgbStream(width * height * 3);
    std::vector<uint8_t> prevRow(rowBytes, 0);
    std::vector<uint8_t> currRow(rowBytes, 0);

    size_t rawPos = 0;
    size_t outPos = 0;

    for (int y = 0; y < height; ++y) {
        if (rawPos >= destLen) break;
        uint8_t filterType = decompressed[rawPos++];

        for (size_t x = 0; x < rowBytes; ++x) {
            uint8_t raw = (rawPos < destLen) ? decompressed[rawPos++] : 0;
            int a = (x >= (size_t)bpp) ? currRow[x - bpp] : 0;
            int b = prevRow[x];
            int c = (x >= (size_t)bpp) ? prevRow[x - bpp] : 0;

            uint8_t reconstructed = 0;
            switch (filterType) {
                case 0: reconstructed = raw; break;
                case 1: reconstructed = raw + (uint8_t)a; break;
                case 2: reconstructed = raw + (uint8_t)b; break;
                case 3: reconstructed = raw + (uint8_t)((a + b) / 2); break;
                case 4: reconstructed = raw + paethPredictor(a, b, c); break;
                default: reconstructed = raw; break;
            }
            currRow[x] = reconstructed;
        }

        // 提取 RGB (若带 Alpha 则按纯白底色复合)
        for (int px = 0; px < width; ++px) {
            size_t cIdx = px * bpp;
            if (colorType == 6) { // RGBA
                uint8_t r = currRow[cIdx];
                uint8_t g = currRow[cIdx + 1];
                uint8_t b = currRow[cIdx + 2];
                uint8_t a = currRow[cIdx + 3];

                if (a == 255) {
                    rgbStream[outPos++] = r;
                    rgbStream[outPos++] = g;
                    rgbStream[outPos++] = b;
                } else if (a == 0) {
                    rgbStream[outPos++] = 255;
                    rgbStream[outPos++] = 255;
                    rgbStream[outPos++] = 255;
                } else {
                    double alpha = a / 255.0;
                    rgbStream[outPos++] = (uint8_t)(r * alpha + 255 * (1.0 - alpha) + 0.5);
                    rgbStream[outPos++] = (uint8_t)(g * alpha + 255 * (1.0 - alpha) + 0.5);
                    rgbStream[outPos++] = (uint8_t)(b * alpha + 255 * (1.0 - alpha) + 0.5);
                }
            } else { // RGB
                rgbStream[outPos++] = currRow[cIdx];
                rgbStream[outPos++] = currRow[cIdx + 1];
                rgbStream[outPos++] = currRow[cIdx + 2];
            }
        }
        prevRow = currRow;
    }

    // 执行 zlib Flate 压缩输出
    uLongf compLen = compressBound(rgbStream.size());
    compressedRgbStream.resize(compLen);
    if (compress(compressedRgbStream.data(), &compLen, rgbStream.data(), rgbStream.size()) != Z_OK) {
        return false;
    }
    compressedRgbStream.resize(compLen);
    return true;
}

bool isHandwrittenInkPng(const uint8_t* pngData, size_t pngSize) {
    if (!pngData || pngSize < 33) return false;
    if (pngData[0] != 0x89 || pngData[1] != 0x50 || pngData[2] != 0x4E || pngData[3] != 0x47) return false;
    int w = (pngData[16] << 24) | (pngData[17] << 16) | (pngData[18] << 8) | pngData[19];
    int h = (pngData[20] << 24) | (pngData[21] << 16) | (pngData[22] << 8) | pngData[23];
    if (w <= 15 || h <= 15) return false;
    uint8_t bitDepth = pngData[24];
    uint8_t colorType = pngData[25];
    if (bitDepth != 8 || (colorType != 2 && colorType != 6)) return false;

    std::vector<uint8_t> idatBuffer;
    size_t pos = 8;
    while (pos + 8 <= pngSize) {
        uint32_t chunkLen = (pngData[pos] << 24) | (pngData[pos + 1] << 16) | (pngData[pos + 2] << 8) | pngData[pos + 3];
        pos += 4;
        if (pos + 4 > pngSize) break;
        std::string chunkType((const char*)&pngData[pos], 4);
        pos += 4;
        if (chunkType == "IDAT") {
            if (pos + chunkLen > pngSize) break;
            idatBuffer.insert(idatBuffer.end(), &pngData[pos], &pngData[pos + chunkLen]);
        }
        pos += chunkLen + 4;
        if (chunkType == "IEND") break;
    }
    if (idatBuffer.empty()) return false;

    int bpp = (colorType == 6) ? 4 : 3;
    size_t rowBytes = w * bpp;
    size_t expectedRawSize = (size_t)h * (1 + rowBytes);
    std::vector<uint8_t> decomp(expectedRawSize);
    uLongf destLen = expectedRawSize;
    if (uncompress(decomp.data(), &destLen, idatBuffer.data(), idatBuffer.size()) != Z_OK) {
        return false;
    }

    size_t redCount = 0;
    size_t rawPos = 0;
    for (int y = 0; y < h; ++y) {
        if (rawPos >= destLen) break;
        rawPos++; // skip filter byte
        for (int x = 0; x < w; ++x) {
            if (rawPos + bpp > destLen) break;
            uint8_t r = decomp[rawPos];
            uint8_t g = decomp[rawPos + 1];
            uint8_t b = decomp[rawPos + 2];
            uint8_t a = (bpp == 4) ? decomp[rawPos + 3] : 255;
            rawPos += bpp;
            if (a > 30 && r > 140 && g < 100 && b < 100) {
                redCount++;
                if (redCount > 30) return true;
            }
        }
    }
    return false;
}

} // namespace doc2pdf
