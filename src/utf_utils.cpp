#include "doc2pdf/utf_utils.hpp"

namespace doc2pdf {

void appendUtf8Char(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string utf16leToUtf8(const uint8_t* data, size_t byteLen) {
    std::string res;
    size_t count = byteLen / 2;
    for (size_t i = 0; i < count; ++i) {
        uint16_t w1 = data[i * 2] | (data[i * 2 + 1] << 8);
        if (w1 >= 0xD800 && w1 <= 0xDBFF && i + 1 < count) {
            uint16_t w2 = data[(i + 1) * 2] | (data[(i + 1) * 2 + 1] << 8);
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                uint32_t cp = 0x10000 + (((w1 & 0x3FF) << 10) | (w2 & 0x3FF));
                appendUtf8Char(res, cp);
                ++i;
                continue;
            }
        }
        appendUtf8Char(res, w1);
    }
    return res;
}

std::vector<uint16_t> utf8ToUtf16(const std::string& utf8) {
    std::vector<uint16_t> utf16;
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= utf8.size()) break;
            cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= utf8.size()) break;
            cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= utf8.size()) break;
            cp = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) | ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
            i += 4;
        } else {
            i += 1;
            continue;
        }

        if (cp <= 0xFFFF) {
            utf16.push_back((uint16_t)cp);
        } else {
            cp -= 0x10000;
            utf16.push_back((uint16_t)(0xD800 + (cp >> 10)));
            utf16.push_back((uint16_t)(0xDC00 + (cp & 0x3FF)));
        }
    }
    return utf16;
}

bool isClosingPunctuation(uint32_t cp) {
    if (cp == ',' || cp == '.' || cp == '!' || cp == '?' || cp == ';' || cp == ':' ||
        cp == ')' || cp == ']' || cp == '}' || cp == '>' || cp == '\'' || cp == '"') return true;
    if (cp == 0xFF0C || // ，
        cp == 0x3002 || // 。
        cp == 0xFF01 || // ！
        cp == 0xFF1F || // ？
        cp == 0xFF1B || // ；
        cp == 0xFF1A || // ：
        cp == 0x3001 || // 、
        cp == 0xFF09 || // ）
        cp == 0x3011 || // 】
        cp == 0x300B || // 》
        cp == 0x201D || // ”
        cp == 0x2019 || // ’
        cp == 0x2026)   // …
        return true;
    return false;
}

bool isOpeningPunctuation(uint32_t cp) {
    if (cp == '(' || cp == '[' || cp == '{' || cp == '<') return true;
    if (cp == 0xFF08 || // （
        cp == 0x3010 || // 【
        cp == 0x300A || // 《
        cp == 0x201C || // “
        cp == 0x2018)   // ‘
        return true;
    return false;
}

} // namespace doc2pdf
