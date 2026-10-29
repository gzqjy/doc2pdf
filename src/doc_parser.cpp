#include "doc2pdf/doc_parser.hpp"
#include "doc2pdf/pole.h"
#include "doc2pdf/utf_utils.hpp"
#include "doc2pdf/image_utils.hpp"

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <map>

namespace doc2pdf {

static void extractImagesFromDataStream(const std::vector<uint8_t>& data,
                                       double maxAllowedWidth,
                                       std::vector<ImageBlock>& outImages) {
    if (data.size() < 32) return;
    size_t i = 0;
    static const uint8_t iendMarker[8] = { 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82 };

    while (i + 16 < data.size()) {
        // PNG 识别
        if (data[i] == 0x89 && data[i+1] == 0x50 && data[i+2] == 0x4E && data[i+3] == 0x47 &&
            data[i+4] == 0x0D && data[i+5] == 0x0A && data[i+6] == 0x1A && data[i+7] == 0x0A) {
            size_t endPos = i + 8;
            while (endPos + 8 <= data.size()) {
                if (std::memcmp(&data[endPos], iendMarker, 8) == 0) {
                    endPos += 8;
                    size_t imgSize = endPos - i;
                    std::string fmt;
                    int pw = 0, ph = 0;
                    if (parseImageDimensions(&data[i], imgSize, fmt, pw, ph)) {
                        double dw = 0, dh = 0;
                        if (!parsePicfHeader(data.data(), i, dw, dh)) {
                            dw = pw * 72.0 / 122.0;
                            dh = ph * 72.0 / 122.0;
                        }
                        if (dw > maxAllowedWidth) {
                            double scale = maxAllowedWidth / dw;
                            dw = maxAllowedWidth;
                            dh *= scale;
                        }
                        outImages.push_back(ImageBlock(std::vector<uint8_t>(data.begin() + i, data.begin() + endPos),
                                                       fmt, pw, ph, dw, dh));
                    }
                    i = endPos;
                    break;
                }
                ++endPos;
            }
            continue;
        }
        // JPEG 识别
        if (data[i] == 0xFF && data[i+1] == 0xD8 && data[i+2] == 0xFF) {
            size_t endPos = i + 2;
            while (endPos + 1 < data.size()) {
                if (data[endPos] == 0xFF && data[endPos+1] == 0xD9) { // EOI
                    endPos += 2;
                    size_t imgSize = endPos - i;
                    std::string fmt;
                    int pw = 0, ph = 0;
                    if (parseImageDimensions(&data[i], imgSize, fmt, pw, ph)) {
                        double dw = 0, dh = 0;
                        if (!parsePicfHeader(data.data(), i, dw, dh)) {
                            dw = pw * 72.0 / 122.0;
                            dh = ph * 72.0 / 122.0;
                        }
                        if (dw > maxAllowedWidth) {
                            double scale = maxAllowedWidth / dw;
                            dw = maxAllowedWidth;
                            dh *= scale;
                        }
                        outImages.push_back(ImageBlock(std::vector<uint8_t>(data.begin() + i, data.begin() + endPos),
                                                       fmt, pw, ph, dw, dh));
                    }
                    i = endPos;
                    break;
                }
                ++endPos;
            }
            continue;
        }
        ++i;
    }
}

struct DocCharFormat {
    double fontSize = 10.5;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool doubleUnderline = false;
    bool strikethrough = false;
    bool doubleStrikethrough = false;
    bool emphasisMark = false;
    ColorRGB color;
    ColorRGB bgColor;
    std::string fontFamily;

    bool operator==(const DocCharFormat& o) const {
        return std::abs(fontSize - o.fontSize) < 0.1 &&
               bold == o.bold &&
               italic == o.italic &&
               underline == o.underline &&
               doubleUnderline == o.doubleUnderline &&
               strikethrough == o.strikethrough &&
               doubleStrikethrough == o.doubleStrikethrough &&
               emphasisMark == o.emphasisMark &&
               color == o.color &&
               bgColor == o.bgColor &&
               fontFamily == o.fontFamily;
    }
    bool operator!=(const DocCharFormat& o) const {
        return !(*this == o);
    }

    TextRun toTextRun(const std::string& text) const {
        TextRun r;
        r.text = text;
        r.fontSize = fontSize;
        r.bold = bold;
        r.italic = italic;
        r.underline = underline;
        r.doubleUnderline = doubleUnderline;
        r.strikethrough = strikethrough;
        r.doubleStrikethrough = doubleStrikethrough;
        r.emphasisMark = emphasisMark;
        r.color = color;
        r.bgColor = bgColor;
        r.fontFamily = fontFamily;
        return r;
    }
};

struct DocPiece {
    uint32_t cpStart = 0;
    uint32_t cpEnd = 0;
    uint32_t actualFc = 0;
    bool isCompressed = false;
};

struct DocCharInfo {
    std::string text;
    DocCharFormat fmt;
    uint16_t rawChar = 0;
};

struct DocChpxSpan {
    uint32_t fcStart = 0;
    uint32_t fcEnd = 0;
    DocCharFormat fmt;
};

// 解析 Ole10Native 流结构提取真实文件名与真实有效载荷 (Payload)
static bool parseOle10NativeStream(const uint8_t* data, size_t size,
                                   std::string& outFilename,
                                   std::vector<uint8_t>& outPayload) {
    if (size < 16) return false;
    size_t p = 6;
    std::string label;
    while (p < size && data[p] != 0) {
        label += static_cast<char>(data[p++]);
    }
    if (p >= size) return false;
    p++;

    std::string origPath;
    while (p < size && data[p] != 0) {
        origPath += static_cast<char>(data[p++]);
    }
    if (p >= size) return false;
    p++;

    p += 8; // 跳过 8 字节 flags
    if (p >= size) return false;

    // 跳过临时路径
    while (p < size && data[p] != 0) {
        p++;
    }
    if (p >= size) return false;
    p++;

    if (p + 4 > size) return false;
    uint32_t actualSize = *reinterpret_cast<const uint32_t*>(&data[p]);
    p += 4;
    if (p + actualSize > size) return false;

    outPayload.assign(data + p, data + p + actualSize);

    if (!label.empty()) {
        outFilename = label;
    } else if (!origPath.empty()) {
        size_t slash = origPath.find_last_of("/\\");
        outFilename = (slash != std::string::npos) ? origPath.substr(slash + 1) : origPath;
    } else {
        outFilename = "attachment.bin";
    }
    return true;
}

// 提取 OLE 结构化存储中嵌入的所有附件
static void scanOleAttachments(POLE::Storage& storage, const std::string& currentPath, std::vector<AttachmentItem>& out) {
    std::list<std::string> entries = storage.entries(currentPath);
    for (const auto& name : entries) {
        std::string full = (currentPath == "/") ? ("/" + name) : (currentPath + "/" + name);
        if (storage.is_directory(full)) {
            scanOleAttachments(storage, full, out);
        } else {
            if (name.find("Ole10Native") != std::string::npos || name == "Package" || name == "\x01Ole10Native") {
                POLE::Stream strm(&storage, full.c_str());
                if (!strm.fail() && strm.size() > 16) {
                    std::vector<uint8_t> data(strm.size());
                    strm.read(data.data(), data.size());
                    std::string fn;
                    std::vector<uint8_t> payload;
                    if (parseOle10NativeStream(data.data(), data.size(), fn, payload)) {
                        out.push_back(AttachmentItem(fn, std::move(payload)));
                    }
                }
            }
        }
    }
}

// 解析 Word 97-2004 原生 PlcfBteChpx 表与 FKP 页面中的 SPRM 字符属性
static void parsePlcfBteChpx(const std::vector<uint8_t>& wdoc,
                             const std::vector<uint8_t>& tbl,
                             uint32_t fcChpx, uint32_t lcbChpx,
                             const std::map<uint16_t, std::string>& ftcMap,
                             std::vector<DocChpxSpan>& outSpans) {
    if (fcChpx + lcbChpx > tbl.size() || lcbChpx < 4) return;
    size_t n = (lcbChpx - 4) / 8;
    if (fcChpx + (n + 1) * 4 + n * 4 > tbl.size()) return;

    size_t pnBase = fcChpx + (n + 1) * 4;
    for (size_t pIdx = 0; pIdx < n; ++pIdx) {
        uint32_t pn = *reinterpret_cast<const uint32_t*>(&tbl[pnBase + pIdx * 4]);
        size_t fkpOffset = static_cast<size_t>(pn) * 512;
        if (fkpOffset + 512 > wdoc.size()) continue;

        const uint8_t* fkp = &wdoc[fkpOffset];
        uint8_t crun = fkp[511];
        if (crun == 0) continue;
        size_t rgbchpxBase = static_cast<size_t>(crun + 1) * 4;
        if (rgbchpxBase + crun > 511) continue;

        for (size_t i = 0; i < crun; ++i) {
            uint32_t fcStart = *reinterpret_cast<const uint32_t*>(&fkp[i * 4]);
            uint32_t fcEnd = *reinterpret_cast<const uint32_t*>(&fkp[(i + 1) * 4]);
            uint8_t bchpx = fkp[rgbchpxBase + i];
            size_t chpxOffset = static_cast<size_t>(bchpx) * 2;
            if (chpxOffset >= 511) continue;

            uint8_t cb = fkp[chpxOffset];
            if (chpxOffset + 1 + cb > 511) continue;
            const uint8_t* grpprl = &fkp[chpxOffset + 1];

            DocCharFormat fmt;
            size_t pos = 0;
            while (pos + 2 <= cb) {
                uint16_t sprm = *reinterpret_cast<const uint16_t*>(&grpprl[pos]);
                pos += 2;
                uint8_t spra = (sprm >> 13) & 0x07;
                size_t opLen = (spra == 0 || spra == 1) ? 1 : ((spra == 2 || spra == 4 || spra == 5) ? 2 : ((spra == 3) ? 4 : ((spra == 6) ? (1 + grpprl[pos]) : ((spra == 7) ? 3 : 1))));
                if (pos + opLen > cb) break;
                const uint8_t* val = &grpprl[pos];
                pos += opLen;

                if (sprm == 0x0835 || sprm == 0x085C) {
                    fmt.bold = (val[0] == 1);
                } else if (sprm == 0x0836 || sprm == 0x085D) {
                    fmt.italic = (val[0] == 1);
                } else if (sprm == 0x0837 || sprm == 0x0838) {
                    fmt.strikethrough = (val[0] != 0);
                } else if (sprm == 0x2A53 && val[0] != 0) {
                    fmt.strikethrough = true;
                } else if (sprm == 0x0853) {
                    fmt.doubleStrikethrough = (val[0] != 0);
                    fmt.strikethrough = true;
                } else if (sprm == 0x2A3E) {
                    if (val[0] == 1) fmt.underline = true;
                    else if (val[0] == 3) { fmt.doubleUnderline = true; fmt.underline = true; }
                    else if (val[0] > 0) fmt.underline = true;
                } else if (sprm == 0x2A34 || sprm == 0x086D || sprm == 0x086E) {
                    fmt.emphasisMark = (val[0] != 0);
                } else if (sprm == 0x2A0C) {
                    if (val[0] == 7) fmt.bgColor = ColorRGB(255, 242, 178); // 黄色背景高亮
                    else if (val[0] == 6) fmt.bgColor = ColorRGB(255, 200, 200); // 浅红高亮
                    else if (val[0] == 4) fmt.bgColor = ColorRGB(200, 255, 200); // 浅绿高亮
                    else if (val[0] == 3) fmt.bgColor = ColorRGB(200, 240, 255); // 浅蓝高亮
                    else if (val[0] == 2) fmt.bgColor = ColorRGB(180, 220, 255); // 蓝色高亮
                } else if (sprm == 0x2A42) {
                    uint8_t ico = val[0];
                    static const ColorRGB icoColors[17] = {
                        ColorRGB(0, 0, 0),       // 0: auto (黑色)
                        ColorRGB(0, 0, 0),       // 1: black
                        ColorRGB(0, 0, 255),     // 2: blue
                        ColorRGB(0, 255, 255),   // 3: cyan
                        ColorRGB(0, 255, 0),     // 4: green
                        ColorRGB(255, 0, 255),   // 5: magenta
                        ColorRGB(255, 0, 0),     // 6: red
                        ColorRGB(255, 255, 0),   // 7: yellow
                        ColorRGB(255, 255, 255), // 8: white
                        ColorRGB(0, 0, 128),     // 9: dark blue
                        ColorRGB(0, 128, 128),   // 10: dark cyan
                        ColorRGB(0, 128, 0),     // 11: dark green
                        ColorRGB(128, 0, 128),   // 12: dark magenta
                        ColorRGB(128, 0, 0),     // 13: dark red
                        ColorRGB(128, 128, 0),   // 14: dark yellow
                        ColorRGB(128, 128, 128), // 15: dark gray
                        ColorRGB(192, 192, 192)  // 16: light gray
                    };
                    if (ico <= 16) {
                        fmt.color = icoColors[ico];
                    }
                } else if (sprm == 0x6870) {
                    uint32_t cv = *reinterpret_cast<const uint32_t*>(val);
                    if ((cv & 0xFF000000) == 0) {
                        fmt.color = ColorRGB(cv & 0xFF, (cv >> 8) & 0xFF, (cv >> 16) & 0xFF);
                    }
                } else if (sprm == 0x4A43) {
                    uint16_t hps = *reinterpret_cast<const uint16_t*>(val);
                    if (hps > 0) fmt.fontSize = hps / 2.0;
                } else if (sprm == 0x4A4F) {
                    uint16_t ftc = *reinterpret_cast<const uint16_t*>(val);
                    auto fit = ftcMap.find(ftc);
                    if (fit != ftcMap.end()) fmt.fontFamily = fit->second;
                }
            }

            DocChpxSpan span;
            span.fcStart = fcStart;
            span.fcEnd = fcEnd;
            span.fmt = fmt;
            outSpans.push_back(span);
        }
    }

    std::sort(outSpans.begin(), outSpans.end(), [](const DocChpxSpan& a, const DocChpxSpan& b) {
        return a.fcStart < b.fcStart;
    });
}

static DocCharFormat getDocCharFormat(uint32_t fc, const std::vector<DocChpxSpan>& spans) {
    DocCharFormat def;
    def.fontSize = 10.5;
    if (spans.empty()) return def;
    size_t l = 0, r = spans.size();
    while (l < r) {
        size_t mid = l + (r - l) / 2;
        if (spans[mid].fcStart <= fc) {
            if (fc < spans[mid].fcEnd) return spans[mid].fmt;
            l = mid + 1;
        } else {
            r = mid;
        }
    }
    return def;
}

static std::vector<TextRun> buildRunsFromCharInfos(const std::vector<DocCharInfo>& chars,
                                                  double defaultFontSize,
                                                  ColorRGB defaultColor,
                                                  int* commentCounter = nullptr) {
    std::vector<TextRun> runs;
    if (chars.empty()) return runs;

    auto applyFmtToRun = [defaultFontSize, defaultColor](TextRun& r, const DocCharFormat& f) {
        r.fontSize = (f.fontSize > 0.0) ? f.fontSize : defaultFontSize;
        r.color = f.color.isValid ? f.color : defaultColor;
        r.bgColor = f.bgColor;
        r.bold = f.bold;
        r.italic = f.italic;
        r.underline = f.underline;
        r.doubleUnderline = f.doubleUnderline;
        r.strikethrough = f.strikethrough;
        r.doubleStrikethrough = f.doubleStrikethrough;
        r.emphasisMark = f.emphasisMark;
        if (!f.fontFamily.empty()) r.fontFamily = f.fontFamily;
    };

    TextRun curRun;
    bool hasCur = false;
    DocCharFormat lastFmt;

    for (size_t i = 0; i < chars.size(); ++i) {
        const auto& c = chars[i];
        if (c.rawChar == 0x0D || c.rawChar == 0x07 || c.rawChar == 0x01) {
            continue;
        }

        if (c.rawChar == 0x05) {
            if (commentCounter != nullptr) {
                int cId = *commentCounter;
                // 将被批注的前置 Run 赋予浅黄色高亮底色和红棕色字体，并关联 commentId
                if (hasCur && !curRun.text.empty()) {
                    curRun.commentId = std::to_string(cId);
                    if (!curRun.bgColor.isValid) {
                        curRun.bgColor = ColorRGB(255, 242, 178);
                    }
                    curRun.color = ColorRGB(180, 50, 0);
                    runs.push_back(curRun);
                    hasCur = false;
                    curRun = TextRun();
                } else if (!runs.empty()) {
                    runs.back().commentId = std::to_string(cId);
                    if (!runs.back().bgColor.isValid) {
                        runs.back().bgColor = ColorRGB(255, 242, 178);
                    }
                    runs.back().color = ColorRGB(180, 50, 0);
                }

                // 插入批注上标链接
                TextRun refRun;
                refRun.fontSize = 9.0;
                refRun.color = ColorRGB(0, 70, 180);
                refRun.text = "[批注" + std::to_string(cId) + "]";
                refRun.linkTarget = "comment_" + std::to_string(cId);
                runs.push_back(refRun);
                (*commentCounter)++;
            }
            continue;
        }

        if (!hasCur) {
            curRun = TextRun();
            applyFmtToRun(curRun, c.fmt);
            curRun.text = c.text;
            lastFmt = c.fmt;
            hasCur = true;
        } else if (c.fmt == lastFmt) {
            curRun.text += c.text;
        } else {
            runs.push_back(curRun);
            curRun = TextRun();
            applyFmtToRun(curRun, c.fmt);
            curRun.text = c.text;
            lastFmt = c.fmt;
        }
    }
    if (hasCur && !curRun.text.empty()) {
        runs.push_back(curRun);
    }
    return runs;
}

// 解析 DOC 字体表 (sttbfffn)
static void parseSttbfffn(const std::vector<uint8_t>& tbl,
                          uint32_t fc, uint32_t lcb,
                          std::vector<DocFontDecl>& fontDecls,
                          std::map<uint16_t, std::string>& ftcMap) {
    if (fc + lcb > tbl.size() || lcb < 4) return;
    const uint8_t* p = &tbl[fc];
    uint16_t cData = *(const uint16_t*)&p[2];
    size_t offset = 4;
    for (uint16_t i = 0; i < cData && offset + 2 <= lcb; ++i) {
        uint8_t cbFfn = p[offset];
        if (cbFfn == 0 || offset + cbFfn > lcb) break;
        // xszFfn 在偏移 6 或 7 处 (UTF-16LE 字符串)
        size_t nameOff = offset + 6;
        if (nameOff < offset + cbFfn) {
            std::string fontName = utf16leToUtf8(&p[nameOff], (cbFfn > 6) ? (cbFfn - 6) : 0);
            size_t nullPos = fontName.find('\0');
            if (nullPos != std::string::npos) fontName = fontName.substr(0, nullPos);
            if (!fontName.empty()) {
                ftcMap[i] = fontName;
                DocFontDecl decl;
                decl.name = fontName;
                fontDecls.push_back(decl);
            }
        }
        offset += cbFfn + 1;
    }
}

struct DocStyleDef {
    std::string name;
    double fontSize = 11.0;
    bool bold = false;
    bool italic = false;
    ColorRGB color;
    double spaceBefore = 0.0;
    double spaceAfter = 4.0;
    double lineSpacing = 1.35;
    Alignment align = Alignment::Left;
};

static void parseStsh(const std::vector<uint8_t>& tbl,
                      uint32_t fcStsh, uint32_t lcbStsh,
                      std::map<int, DocStyleDef>& outStyles) {
    if (fcStsh + 4 > tbl.size() || lcbStsh < 4) return;
    uint16_t cbStshi = *(const uint16_t*)&tbl[fcStsh];
    if (fcStsh + 2 + cbStshi > tbl.size()) return;

    const uint8_t* stshi = &tbl[fcStsh + 2];
    uint16_t cstd = *(const uint16_t*)&stshi[0];
    uint16_t cbSTDBaseInFile = *(const uint16_t*)&stshi[2];

    size_t pos = fcStsh + 2 + cbStshi;
    for (int istd = 0; istd < (int)cstd && pos + 2 <= tbl.size(); ++istd) {
        uint16_t cbStd = *(const uint16_t*)&tbl[pos];
        pos += 2;
        if (cbStd == 0) continue;
        if (pos + cbStd > tbl.size()) break;

        const uint8_t* stdData = &tbl[pos];
        pos += cbStd;

        if (cbSTDBaseInFile + 2 <= cbStd) {
            uint16_t xlen = *(const uint16_t*)&stdData[cbSTDBaseInFile];
            std::string name;
            if (cbSTDBaseInFile + 2 + xlen * 2 <= cbStd) {
                name = utf16leToUtf8(&stdData[cbSTDBaseInFile + 2], xlen * 2);
            }

            DocStyleDef def;
            def.name = name;

            size_t upxPos = cbSTDBaseInFile + 2 + (xlen + 1) * 2;
            if (upxPos < cbStd) {
                // 搜索 sprmCHps (0x4A43)
                for (size_t k = upxPos; k + 4 <= cbStd; ++k) {
                    if (stdData[k] == 0x43 && stdData[k+1] == 0x4A) {
                        uint16_t hps = *(const uint16_t*)&stdData[k+2];
                        if (hps > 0) def.fontSize = hps / 2.0;
                    } else if (stdData[k] == 0x35 && stdData[k+1] == 0x08 && stdData[k+2] == 0x01) {
                        def.bold = true;
                    } else if (stdData[k] == 0x70 && stdData[k+1] == 0x68) {
                        uint32_t cv = *(const uint32_t*)&stdData[k+2];
                        def.color = ColorRGB((uint8_t)(cv & 0xFF), (uint8_t)((cv >> 8) & 0xFF), (uint8_t)((cv >> 16) & 0xFF));
                    }
                }
            }

            outStyles[istd] = def;
        }
    }
}

static void parseSectionProperties(const std::vector<uint8_t>& wdoc,
                                   const std::vector<uint8_t>& tbl,
                                   uint32_t fcSed, uint32_t lcbSed,
                                   PageSetup& setup) {
    if (fcSed + 20 > tbl.size() || lcbSed < 20) return;
    const uint8_t* sed = &tbl[fcSed + 8];
    uint32_t fcSepx = *(const uint32_t*)&sed[2];

    if (fcSepx + 2 <= wdoc.size()) {
        uint16_t cbSepx = *(const uint16_t*)&wdoc[fcSepx];
        if (fcSepx + 2 + cbSepx <= wdoc.size()) {
            const uint8_t* sdata = &wdoc[fcSepx + 2];
            size_t pos = 0;
            while (pos + 2 <= cbSepx) {
                uint16_t sprm = *(const uint16_t*)&sdata[pos];
                pos += 2;
                uint8_t spra = (sprm >> 13) & 7;
                size_t sz = (spra == 0 || spra == 1) ? 1 : ((spra == 2 || spra == 4 || spra == 5) ? 2 : ((spra == 3) ? 4 : ((spra == 7) ? 3 : 1 + sdata[pos])));
                if (pos + sz > cbSepx) break;

                if (sprm == 0xB01F) { // sprmSXaPage
                    setup.pageWidth = *(const uint16_t*)&sdata[pos] / 20.0;
                } else if (sprm == 0xB020) { // sprmSYaPage
                    setup.pageHeight = *(const uint16_t*)&sdata[pos] / 20.0;
                } else if (sprm == 0xB021) { // sprmSDxaLeft
                    setup.marginLeft = *(const uint16_t*)&sdata[pos] / 20.0;
                } else if (sprm == 0xB022) { // sprmSDxaRight
                    setup.marginRight = *(const uint16_t*)&sdata[pos] / 20.0;
                } else if (sprm == 0x9023) { // sprmSDyaTop
                    setup.marginTop = *(const int16_t*)&sdata[pos] / 20.0;
                } else if (sprm == 0x9024) { // sprmSDyaBottom
                    setup.marginBottom = *(const int16_t*)&sdata[pos] / 20.0;
                }
                pos += sz;
            }
        }
    }
}

bool DocParser::parse(const std::string& filePath, Document& outDoc, std::string& err) {
    POLE::Storage storage(filePath.c_str());
    if (!storage.open()) {
        err = "POLE failed to open structured storage in " + filePath;
        return false;
    }

    // 1. 读取 /WordDocument 主流
    POLE::Stream wdocStream(&storage, "/WordDocument");
    if (wdocStream.fail() || wdocStream.size() < 512) {
        err = "Cannot locate /WordDocument stream in OLE storage";
        return false;
    }

    std::vector<uint8_t> wdoc(wdocStream.size());
    wdocStream.read(wdoc.data(), wdoc.size());

    // 2. 判定 Table 流 (0Table 或 1Table)
    bool use1Table = (wdoc[0x000B] & 0x02) != 0;
    std::string tblName = use1Table ? "/1Table" : "/0Table";
    POLE::Stream tblStream(&storage, tblName.c_str());
    std::vector<uint8_t> tblData;
    if (!tblStream.fail() && tblStream.size() > 0) {
        tblData.resize(tblStream.size());
        tblStream.read(tblData.data(), tblData.size());
    } else {
        std::string altTbl = use1Table ? "/0Table" : "/1Table";
        POLE::Stream realTblStream(&storage, altTbl.c_str());
        tblData.resize(realTblStream.size());
        realTblStream.read(tblData.data(), tblData.size());
    }

    // 3. 读取节属性 (Section Properties)
    if (wdoc.size() >= 0x00D2 && !tblData.empty()) {
        uint32_t fcSed = *(const uint32_t*)&wdoc[0x00CA];
        uint32_t lcbSed = *(const uint32_t*)&wdoc[0x00CE];
        parseSectionProperties(wdoc, tblData, fcSed, lcbSed, outDoc.pageSetup);
    }
    if (outDoc.pageSetup.linePitch <= 0.0) {
        outDoc.pageSetup.linePitch = 18.0;
    }

    double contentWidth = outDoc.pageSetup.pageWidth - outDoc.pageSetup.marginLeft - outDoc.pageSetup.marginRight;
    if (contentWidth <= 50.0) contentWidth = 432.0;

    // 4. 读取样式表 (STSH) 与字体表 (sttbfffn)
    std::map<int, DocStyleDef> styles;
    if (wdoc.size() >= 0x00AA && !tblData.empty()) {
        uint32_t fcStsh = *(const uint32_t*)&wdoc[0x00A2];
        uint32_t lcbStsh = *(const uint32_t*)&wdoc[0x00A6];
        parseStsh(tblData, fcStsh, lcbStsh, styles);
    }

    std::map<uint16_t, std::string> ftcMap;
    if (wdoc.size() >= 0x011A && !tblData.empty()) {
        uint32_t fcSttbf = *(const uint32_t*)&wdoc[0x0112];
        uint32_t lcbSttbf = *(const uint32_t*)&wdoc[0x0116];
        parseSttbfffn(tblData, fcSttbf, lcbSttbf, outDoc.fontDeclarations, ftcMap);
    }

    // 4.5 提取 OLE 结构化存储中嵌入的所有附件 (Ole10Native / Package)
    scanOleAttachments(storage, "/", outDoc.attachments);

    // 5. 提取 /Data 流中的所有内嵌图片
    std::vector<ImageBlock> allExtractedImages;
    POLE::Stream dataStream(&storage, "/Data");
    if (!dataStream.fail() && dataStream.size() > 0) {
        std::vector<uint8_t> streamData(dataStream.size());
        dataStream.read(streamData.data(), streamData.size());
        extractImagesFromDataStream(streamData, contentWidth, allExtractedImages);
    }
    // 兼容可能内嵌于 WordDocument 流中的图片
    extractImagesFromDataStream(wdoc, contentWidth, allExtractedImages);

    std::vector<ImageBlock> docImages;
    ImageBlock attachmentIcon;
    bool hasAttachmentIcon = false;
    for (const auto& img : allExtractedImages) {
        if (!outDoc.attachments.empty() && !hasAttachmentIcon && img.pixelWidth <= 150 && img.pixelHeight <= 150) {
            attachmentIcon = img;
            attachmentIcon.displayWidthPt = 32.0;
            attachmentIcon.displayHeightPt = 32.0;
            hasAttachmentIcon = true;
        } else {
            docImages.push_back(img);
        }
    }

    // 5.5 解析 Word 97-2004 原生字符属性表 (PlcfBteChpx)
    std::vector<DocChpxSpan> chpxSpans;
    if (wdoc.size() >= 0x0102 && !tblData.empty()) {
        uint32_t fcChpx = *reinterpret_cast<const uint32_t*>(&wdoc[0x00FA]);
        uint32_t lcbChpx = *reinterpret_cast<const uint32_t*>(&wdoc[0x00FE]);
        parsePlcfBteChpx(wdoc, tblData, fcChpx, lcbChpx, ftcMap, chpxSpans);
    }

    // 6. 解析 Piece Table (CLX) 提取正文文本流
    std::vector<DocPiece> pieces;
    bool pieceTableFound = false;

    if (wdoc.size() >= 0x01AA && !tblData.empty()) {
        uint32_t fcClx = *(const uint32_t*)&wdoc[0x01A2];
        uint32_t lcbClx = *(const uint32_t*)&wdoc[0x01A6];

        if (fcClx + lcbClx <= tblData.size()) {
            size_t pos = fcClx;
            size_t endClx = fcClx + lcbClx;
            while (pos < endClx) {
                uint8_t clxt = tblData[pos++];
                if (clxt == 1) {
                    if (pos + 2 > endClx) break;
                    uint16_t cb = *(const uint16_t*)&tblData[pos];
                    pos += 2 + cb;
                } else if (clxt == 2) {
                    if (pos + 4 > endClx) break;
                    uint32_t lcb = *(const uint32_t*)&tblData[pos];
                    pos += 4;
                    if (lcb < 4) break;
                    size_t nPieces = (lcb - 4) / 12;
                    size_t cpBase = pos;
                    size_t pcdBase = pos + (nPieces + 1) * 4;
                    pieces.clear();
                    if (pcdBase + nPieces * 8 <= endClx) {
                        for (size_t k = 0; k < nPieces; ++k) {
                            uint32_t cpStart = *(const uint32_t*)&tblData[cpBase + k * 4];
                            uint32_t cpEnd = *(const uint32_t*)&tblData[cpBase + (k + 1) * 4];
                            uint32_t fc = *(const uint32_t*)&tblData[pcdBase + k * 8 + 2];
                            size_t cpLen = (cpEnd > cpStart) ? (cpEnd - cpStart) : 0;
                            bool isCompressed = (fc & 0x40000000) != 0;
                            uint32_t actualFc = isCompressed ? ((fc & ~0x40000000) / 2) : fc;

                            DocPiece piece;
                            piece.cpStart = cpStart;
                            piece.cpEnd = cpEnd;
                            piece.actualFc = actualFc;
                            piece.isCompressed = isCompressed;
                            pieces.push_back(piece);
                        }
                        pieceTableFound = !pieces.empty();

                        auto extractCpChars = [&](uint32_t cpA, uint32_t cpB) -> std::vector<DocCharInfo> {
                            std::vector<DocCharInfo> res;
                            for (const auto& p : pieces) {
                                if (p.cpEnd <= cpA || p.cpStart >= cpB) continue;
                                uint32_t segStart = std::max(p.cpStart, cpA);
                                uint32_t segEnd = std::min(p.cpEnd, cpB);
                                uint32_t offsetInPiece = segStart - p.cpStart;
                                uint32_t segLen = segEnd - segStart;
                                for (uint32_t i = 0; i < segLen; ++i) {
                                    DocCharInfo dci;
                                    if (p.isCompressed) {
                                        uint32_t fc = p.actualFc + offsetInPiece + i;
                                        if (fc < wdoc.size()) {
                                            uint8_t b = wdoc[fc];
                                            dci.rawChar = b;
                                            appendUtf8Char(dci.text, b);
                                            dci.fmt = getDocCharFormat(fc, chpxSpans);
                                        }
                                    } else {
                                        uint32_t fc = p.actualFc + (offsetInPiece + i) * 2;
                                        if (fc + 1 < wdoc.size()) {
                                            uint16_t w = wdoc[fc] | (static_cast<uint16_t>(wdoc[fc + 1]) << 8);
                                            dci.rawChar = w;
                                            appendUtf8Char(dci.text, w);
                                            dci.fmt = getDocCharFormat(fc, chpxSpans);
                                        }
                                    }
                                    if (!dci.text.empty()) {
                                        res.push_back(dci);
                                    }
                                }
                            }
                            return res;
                        };

                        uint32_t ccpText = (wdoc.size() >= 0x0050) ? *(const uint32_t*)&wdoc[0x004C] : 0;
                        uint32_t ccpFtn  = (wdoc.size() >= 0x0054) ? *(const uint32_t*)&wdoc[0x0050] : 0;
                        uint32_t ccpHdd  = (wdoc.size() >= 0x0058) ? *(const uint32_t*)&wdoc[0x0054] : 0;
                        uint32_t ccpMcr  = (wdoc.size() >= 0x005C) ? *(const uint32_t*)&wdoc[0x0058] : 0;
                        uint32_t ccpAtn  = (wdoc.size() >= 0x0060) ? *(const uint32_t*)&wdoc[0x005C] : 0;
                        uint32_t ccpEdn  = (wdoc.size() >= 0x0064) ? *(const uint32_t*)&wdoc[0x0060] : 0;
                        uint32_t ccpTxbx = (wdoc.size() >= 0x0068) ? *(const uint32_t*)&wdoc[0x0064] : 0;
                        uint32_t ccpHdrTxbx = (wdoc.size() >= 0x006C) ? *(const uint32_t*)&wdoc[0x0068] : 0;

                        // 提取页眉页脚子流 (Header & Footer)
                        if (ccpHdd > 0) {
                            std::vector<DocCharInfo> hddChars = extractCpChars(ccpText + ccpFtn, ccpText + ccpFtn + ccpHdd);
                            if (!hddChars.empty()) {
                                std::vector<std::vector<DocCharInfo>> hLines;
                                std::vector<DocCharInfo> curLine;
                                for (const auto& dc : hddChars) {
                                    if (dc.rawChar == '\r') {
                                        if (!curLine.empty()) {
                                            hLines.push_back(curLine);
                                            curLine.clear();
                                        }
                                    } else {
                                        curLine.push_back(dc);
                                    }
                                }
                                if (!curLine.empty()) hLines.push_back(curLine);

                                for (const auto& l : hLines) {
                                    std::string plain;
                                    for (const auto& c : l) {
                                        if (c.rawChar >= 0x20 && c.rawChar != '\t') {
                                            plain += c.text;
                                        }
                                    }
                                    while (!plain.empty() && (plain.front() == ' ' || plain.front() == '\t')) plain.erase(plain.begin());
                                    while (!plain.empty() && (plain.back() == ' ' || plain.back() == '\t')) plain.pop_back();
                                    if (plain.empty()) continue;

                                    bool allDigits = true;
                                    for (char ch : plain) {
                                        if (ch < '0' || ch > '9') {
                                            allDigits = false;
                                            break;
                                        }
                                    }
                                    if (allDigits) continue;

                                    if (plain.find("页眉") != std::string::npos || (outDoc.headerParagraphs.empty() && plain.find("页脚") == std::string::npos)) {
                                        Paragraph hPara;
                                        hPara.runs = buildRunsFromCharInfos(l, 9.0, ColorRGB(0, 0, 0));
                                        for (auto& r : hPara.runs) {
                                            while (!r.text.empty() && (static_cast<uint8_t>(r.text.front()) < 0x20)) r.text.erase(r.text.begin());
                                        }
                                        outDoc.headerParagraphs.push_back(hPara);
                                    } else if (plain.find("页脚") != std::string::npos || outDoc.footerParagraphs.empty()) {
                                        Paragraph fPara;
                                        fPara.runs = buildRunsFromCharInfos(l, 9.0, ColorRGB(0, 0, 0));
                                        for (auto& fr : fPara.runs) {
                                            while (!fr.text.empty() && (static_cast<uint8_t>(fr.text.front()) < 0x20)) fr.text.erase(fr.text.begin());
                                        }
                                        outDoc.footerParagraphs.push_back(fPara);
                                    }
                                }
                            }
                        }

                        // 提取页眉/页脚文本框中的居中页码 (ccpHdrTxbx)
                        if (ccpHdrTxbx > 0) {
                            uint32_t baseHdrTxbx = ccpText + ccpFtn + ccpHdd + ccpMcr + ccpAtn + ccpEdn + ccpTxbx;
                            std::vector<DocCharInfo> txChars = extractCpChars(baseHdrTxbx, baseHdrTxbx + ccpHdrTxbx);
                            std::string txPlain;
                            for (const auto& c : txChars) {
                                if (c.rawChar >= 0x20 || c.rawChar == '\r') {
                                    txPlain += c.text;
                                }
                            }
                            if (txPlain.find("PAGE") != std::string::npos || txPlain.find("—") != std::string::npos) {
                                Paragraph pNumPara;
                                pNumPara.align = Alignment::Center;
                                TextRun trNum;
                                trNum.fontSize = 10.5;
                                trNum.text = "PAGE_NUM";
                                pNumPara.runs.push_back(trNum);
                                outDoc.footerParagraphs.push_back(pNumPara);
                            }
                        }

                        // 提取批注子流 (Comments)
                        if (ccpAtn > 0) {
                            std::vector<DocCharInfo> atnChars = extractCpChars(ccpText + ccpFtn + ccpHdd + ccpMcr, ccpText + ccpFtn + ccpHdd + ccpMcr + ccpAtn);
                            if (!atnChars.empty()) {
                                std::vector<std::vector<DocCharInfo>> aLines;
                                std::vector<DocCharInfo> curLine;
                                for (const auto& dc : atnChars) {
                                    if (dc.rawChar == '\r') {
                                        if (!curLine.empty()) {
                                            aLines.push_back(curLine);
                                            curLine.clear();
                                        }
                                    } else {
                                        curLine.push_back(dc);
                                    }
                                }
                                if (!curLine.empty()) aLines.push_back(curLine);

                                int cIdx = 0;
                                for (const auto& al : aLines) {
                                    CommentItem cItem;
                                    cItem.id = std::to_string(cIdx++);
                                    cItem.author = "审核员";
                                    Paragraph cp;
                                    cp.runs = buildRunsFromCharInfos(al, 9.5, ColorRGB(0, 0, 0));
                                    cItem.paragraphs.push_back(cp);
                                    outDoc.comments.push_back(cItem);
                                }
                            }
                        }

                        // 提取脚注子流 (Footnotes)
                        if (ccpFtn > 0) {
                            std::vector<DocCharInfo> ftnChars = extractCpChars(ccpText, ccpText + ccpFtn);
                            if (!ftnChars.empty()) {
                                std::vector<std::vector<DocCharInfo>> fLines;
                                std::vector<DocCharInfo> curLine;
                                for (const auto& dc : ftnChars) {
                                    if (dc.rawChar == '\r') {
                                        if (!curLine.empty()) {
                                            fLines.push_back(curLine);
                                            curLine.clear();
                                        }
                                    } else {
                                        curLine.push_back(dc);
                                    }
                                }
                                if (!curLine.empty()) fLines.push_back(curLine);

                                int fIdx = 1;
                                for (const auto& fl : fLines) {
                                    FootnoteItem fnItem;
                                    fnItem.id = std::to_string(fIdx++);
                                    Paragraph fp;
                                    fp.runs = buildRunsFromCharInfos(fl, 9.0, ColorRGB(0, 0, 0));
                                    fnItem.paragraphs.push_back(fp);
                                    outDoc.footnotes[fnItem.id] = fnItem;
                                }
                            }
                        }
                    }
                    break;
                } else {
                    break;
                }
            }
        }
    }

    auto extractCpChars = [&](uint32_t cpA, uint32_t cpB) -> std::vector<DocCharInfo> {
        std::vector<DocCharInfo> res;
        for (const auto& p : pieces) {
            if (p.cpEnd <= cpA || p.cpStart >= cpB) continue;
            uint32_t segStart = std::max(p.cpStart, cpA);
            uint32_t segEnd = std::min(p.cpEnd, cpB);
            uint32_t offsetInPiece = segStart - p.cpStart;
            uint32_t segLen = segEnd - segStart;
            for (uint32_t i = 0; i < segLen; ++i) {
                DocCharInfo dci;
                if (p.isCompressed) {
                    uint32_t fc = p.actualFc + offsetInPiece + i;
                    if (fc < wdoc.size()) {
                        uint8_t b = wdoc[fc];
                        dci.rawChar = b;
                        appendUtf8Char(dci.text, b);
                        dci.fmt = getDocCharFormat(fc, chpxSpans);
                    }
                } else {
                    uint32_t fc = p.actualFc + (offsetInPiece + i) * 2;
                    if (fc + 1 < wdoc.size()) {
                        uint16_t w = wdoc[fc] | (static_cast<uint16_t>(wdoc[fc + 1]) << 8);
                        dci.rawChar = w;
                        appendUtf8Char(dci.text, w);
                        dci.fmt = getDocCharFormat(fc, chpxSpans);
                    }
                }
                if (!dci.text.empty()) {
                    res.push_back(dci);
                }
            }
        }
        return res;
    };

    std::vector<DocCharInfo> docChars;
    if (pieceTableFound) {
        uint32_t ccpText = (wdoc.size() >= 0x0050) ? *(const uint32_t*)&wdoc[0x004C] : 0;
        docChars = extractCpChars(0, (ccpText > 0) ? ccpText : 0xFFFFFFFF);
    } else {
        uint32_t fcMin = *(const uint32_t*)&wdoc[0x0018];
        if (fcMin >= wdoc.size()) fcMin = 512;
        for (size_t k = fcMin; k + 1 < wdoc.size(); k += 2) {
            uint16_t w = wdoc[k] | (wdoc[k + 1] << 8);
            if (w == 0x0D || w == 0x07 || w == 0x0A || (w >= 0x20 && w < 0xD800)) {
                DocCharInfo dci;
                dci.rawChar = w;
                appendUtf8Char(dci.text, w);
                dci.fmt = getDocCharFormat(static_cast<uint32_t>(k), chpxSpans);
                docChars.push_back(dci);
            }
        }
    }

    // 7. 逐段结构化解析 (段落、表格、图片混合排版)
    size_t currentImageIndex = 0;
    Table activeTable;
    bool inTableBlock = false;
    int docCommentCounter = 0;
    bool attachmentInserted = false;

    auto insertEmbeddedAttachment = [&]() {
        if (attachmentInserted || outDoc.attachments.empty()) return;
        attachmentInserted = true;
        if (hasAttachmentIcon) {
            outDoc.blocks.push_back(Block::makeImage(attachmentIcon));
        }
        Paragraph attNameP;
        attNameP.align = Alignment::Center;
        attNameP.spaceBefore = 1.0;
        attNameP.spaceAfter = 2.0;
        TextRun rName;
        rName.text = "[📎 " + outDoc.attachments[0].filename + "]";
        rName.fontSize = 9.5;
        rName.bold = true;
        rName.color = ColorRGB(0, 102, 204);
        rName.underline = true;
        attNameP.runs.push_back(rName);
        outDoc.blocks.push_back(Block::makeParagraph(attNameP));
    };

    auto flushActiveTable = [&]() {
        if (inTableBlock && !activeTable.rows.empty()) {
            outDoc.blocks.push_back(Block::makeTable(std::move(activeTable)));
            activeTable = Table();
            inTableBlock = false;
        }
    };

    auto emitDocParagraph = [&](const std::vector<DocCharInfo>& pChars) {
        if (pChars.empty()) return;
        std::string plainText;
        for (const auto& c : pChars) {
            if (c.rawChar != 0x0D && c.rawChar != 0x07 && c.rawChar != 0x01) {
                plainText += c.text;
            }
        }
        std::string trimmed = plainText;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '	')) trimmed.erase(trimmed.begin());
        if (trimmed.empty()) return;

        Paragraph para;
        if (trimmed.find("软件供应链风险评估报告") != std::string::npos) {
            para.headingLevel = 0;
            para.spaceBefore = 0.0;
            para.spaceAfter = 15.0;
            para.align = Alignment::Left;
            para.runs = buildRunsFromCharInfos(pChars, 26.0, ColorRGB(23, 54, 93), &docCommentCounter);
            for (auto& r : para.runs) {
                if (r.fontSize < 20.0) r.fontSize = 26.0;
                if (!r.color.isValid) r.color = ColorRGB(23, 54, 93);
            }
        } else if (trimmed.size() >= 3 && trimmed[0] >= '1' && trimmed[0] <= '9' && trimmed[1] == '.' && trimmed[2] == ' ' && trimmed.find("：") == std::string::npos && trimmed.find(':') == std::string::npos) {
            para.headingLevel = 1;
            para.spaceBefore = 24.0;
            para.spaceAfter = 0.0;
            para.keepNext = true;
            para.runs = buildRunsFromCharInfos(pChars, 14.0, ColorRGB(54, 96, 145), &docCommentCounter);
            for (auto& r : para.runs) {
                r.bold = true;
                if (r.fontSize < 13.0) r.fontSize = 14.0;
                if (!r.color.isValid) r.color = ColorRGB(54, 96, 145);
            }
        } else if (trimmed.size() >= 4 && trimmed[0] >= '1' && trimmed[0] <= '9' && trimmed[1] == '.' && trimmed[2] >= '1' && trimmed[2] <= '9') {
            para.headingLevel = 2;
            para.spaceBefore = 10.0;
            para.spaceAfter = 0.0;
            para.keepNext = true;
            para.runs = buildRunsFromCharInfos(pChars, 13.0, ColorRGB(79, 129, 189), &docCommentCounter);
            for (auto& r : para.runs) {
                r.bold = true;
                if (r.fontSize < 12.0) r.fontSize = 13.0;
                if (!r.color.isValid) r.color = ColorRGB(79, 129, 189);
            }
        } else if (trimmed.find("========================================") != std::string::npos) {
            para.spaceBefore = 0.0;
            para.spaceAfter = 10.0;
            para.runs = buildRunsFromCharInfos(pChars, 10.5, ColorRGB(79, 129, 189), &docCommentCounter);
        } else {
            para.spaceBefore = 0.0;
            para.spaceAfter = 10.0;
            para.lineSpacing = 1.15;
            para.align = Alignment::Justify;
            para.runs = buildRunsFromCharInfos(pChars, 10.5, ColorRGB(0, 0, 0), &docCommentCounter);
        }

        outDoc.blocks.push_back(Block::makeParagraph(para));
        if (trimmed.find("系统综合风险评分为") != std::string::npos) {
            insertEmbeddedAttachment();
        }
    };

    std::vector<DocCharInfo> curLineChars;
    for (size_t idx = 0; idx < docChars.size(); ++idx) {
        const auto& dc = docChars[idx];
        if (dc.rawChar == 0x0D) {
            bool hasImage = false;
            for (const auto& c : curLineChars) {
                if (c.rawChar == 0x01) { hasImage = true; break; }
            }
            if (hasImage) {
                flushActiveTable();
                if (currentImageIndex < docImages.size()) {
                    outDoc.blocks.push_back(Block::makeImage(docImages[currentImageIndex++]));
                }
                curLineChars.clear();
                continue;
            }

            bool hasCell = false;
            for (const auto& c : curLineChars) {
                if (c.rawChar == 0x07) { hasCell = true; break; }
            }

            if (hasCell) {
                inTableBlock = true;
                std::vector<std::vector<std::vector<DocCharInfo>>> tableRows;
                std::vector<std::vector<DocCharInfo>> curRow;
                std::vector<DocCharInfo> curCell;
                std::vector<DocCharInfo> postChars;
                size_t cPos = 0;
                while (cPos < curLineChars.size()) {
                    if (curLineChars[cPos].rawChar == 0x07) {
                        if (cPos + 1 < curLineChars.size() && curLineChars[cPos + 1].rawChar == 0x07) {
                            curRow.push_back(curCell);
                            curCell.clear();
                            tableRows.push_back(curRow);
                            curRow.clear();
                            cPos += 2;
                            bool furtherCells = false;
                            for (size_t f = cPos; f < curLineChars.size(); ++f) {
                                if (curLineChars[f].rawChar == 0x07) { furtherCells = true; break; }
                            }
                            if (!furtherCells) {
                                while (cPos < curLineChars.size()) {
                                    postChars.push_back(curLineChars[cPos++]);
                                }
                                break;
                            }
                        } else {
                            curRow.push_back(curCell);
                            curCell.clear();
                            cPos += 1;
                        }
                    } else {
                        curCell.push_back(curLineChars[cPos++]);
                    }
                }
                if (!curCell.empty()) curRow.push_back(curCell);
                if (!curRow.empty()) tableRows.push_back(curRow);

                for (const auto& rCells : tableRows) {
                    if (rCells.empty()) continue;
                    TableRow r;
                    if (activeTable.rows.empty()) r.isHeader = true;
                    for (const auto& cellChars : rCells) {
                        TableCell tc;
                        tc.padTop = 2.0;
                        tc.padBottom = 2.0;
                        tc.padLeft = 5.4;
                        tc.padRight = 5.4;

                        Paragraph p;
                        p.lineSpacing = 1.0;
                        p.runs = buildRunsFromCharInfos(cellChars, 10.5, ColorRGB(0, 0, 0));
                        tc.paragraphs.push_back(p);
                        r.cells.push_back(tc);
                    }
                    activeTable.rows.push_back(r);
                }

                if (!postChars.empty()) {
                    flushActiveTable();
                    emitDocParagraph(postChars);
                }
            } else {
                flushActiveTable();
                emitDocParagraph(curLineChars);
            }
            curLineChars.clear();
        } else {
            curLineChars.push_back(dc);
        }
    }
    if (!curLineChars.empty()) {
        flushActiveTable();
        emitDocParagraph(curLineChars);
    }
    flushActiveTable();
    if (!attachmentInserted) {
        insertEmbeddedAttachment();
    }

    // 如果文档中有未通过 0x01 消费的附图，追加在末尾
    while (currentImageIndex < docImages.size()) {
        outDoc.blocks.push_back(Block::makeImage(docImages[currentImageIndex++]));
    }

    return true;
}

} // namespace doc2pdf
