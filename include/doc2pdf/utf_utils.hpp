#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace doc2pdf {

// UTF-8 与 UTF-16 编解码工具
std::vector<uint16_t> utf8ToUtf16(const std::string& utf8);
std::string utf16leToUtf8(const uint8_t* data, size_t byteLen);
void appendUtf8Char(std::string& out, uint32_t cp);

// 标点避头尾禁则判定 (UAX #14 / Kinsoku Shori)
bool isClosingPunctuation(uint32_t cp);
bool isOpeningPunctuation(uint32_t cp);

} // namespace doc2pdf
