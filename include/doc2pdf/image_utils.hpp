#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace doc2pdf {

// 解析图像格式和像素尺寸 (支持 PNG 与 JPEG)
bool parseImageDimensions(const uint8_t* data, size_t size, std::string& outFormat, int& outWidth, int& outHeight);

// 解析 MS-DOC PICF 结构头提取精确目标显示尺寸 (点数 pt)
bool parsePicfHeader(const uint8_t* data, size_t offset, double& outDwPt, double& outDhPt);

// 将 PNG 数据解码重构为原始 RGB 流 (处理 PNG 滤波及 RGBA 白色混合)，并执行 Flate 压缩用于 PDF stream
bool decodePngToRgbStream(const uint8_t* pngData, size_t pngSize,
                          int& width, int& height,
                          std::vector<uint8_t>& compressedRgbStream);

} // namespace doc2pdf
