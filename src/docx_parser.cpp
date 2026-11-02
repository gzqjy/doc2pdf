#include "doc2pdf/docx_parser.hpp"
#include "doc2pdf/pole.h"

#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <zlib.h>

namespace doc2pdf {

// 解析 Ole10Native 流结构提取真实文件名与真实有效载荷 (Payload)
static bool parseOle10NativeStream(const uint8_t* data, size_t size,
                                   std::string& outFilename,
                                   std::vector<uint8_t>& outPayload) {
    if (size < 16) return false;
    // [0..3]: uint32 totalSize
    // [4..5]: uint16 flags
    // [6..]:  null-terminated label/short filename
    size_t p = 6;
    std::string label;
    while (p < size && data[p] != 0) {
        label += static_cast<char>(data[p++]);
    }
    if (p >= size) return false;
    p++; // 跳过 '\0'

    // null-terminated original path
    std::string origPath;
    while (p < size && data[p] != 0) {
        origPath += static_cast<char>(data[p++]);
    }
    if (p >= size) return false;
    p++; // 跳过 '\0'

    // 8 bytes (flags)
    p += 8;
    if (p >= size) return false;

    // null-terminated temp path
    while (p < size && data[p] != 0) {
        p++;
    }
    if (p >= size) return false;
    p++; // 跳过 '\0'

    // 4 bytes: actual size
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

// 若输入数据为 OLE2 复合存储封装 (以 0xD0CF11E0 开头)，通过 POLE 打开提取内部 Ole10Native
static bool extractFromOle2Binary(const std::vector<uint8_t>& oleBytes,
                                  std::string& outFilename,
                                  std::vector<uint8_t>& outPayload) {
    if (oleBytes.size() >= 8 &&
        oleBytes[0] == 0xD0 && oleBytes[1] == 0xCF && oleBytes[2] == 0x11 && oleBytes[3] == 0xE0) {
        // 写入临时文件并通过 POLE 打开（动态获取临时目录环境变量，消除硬编码）
        std::string tmpDir = "/tmp";
        const char* envTmp = std::getenv("TMPDIR");
        if (envTmp == nullptr || *envTmp == '\0') envTmp = std::getenv("TEMP");
        if (envTmp == nullptr || *envTmp == '\0') envTmp = std::getenv("TMP");
        if (envTmp != nullptr && *envTmp != '\0') {
            tmpDir = envTmp;
        }
        if (!tmpDir.empty() && tmpDir.back() != '/' && tmpDir.back() != '\\') {
            tmpDir.push_back('/');
        }
        std::string tmpTemplate = tmpDir + "doc2pdf_ole_XXXXXX";
        std::vector<char> tmpPath(tmpTemplate.begin(), tmpTemplate.end());
        tmpPath.push_back('\0');

        int fd = mkstemp(tmpPath.data());
        if (fd != -1) {
            ssize_t written = write(fd, oleBytes.data(), oleBytes.size());
            close(fd);
            if (written == static_cast<ssize_t>(oleBytes.size())) {
                POLE::Storage st(tmpPath.data());
                if (st.open()) {
                    std::list<std::string> entries = st.entries("/");
                    for (const auto& ent : entries) {
                        if (ent == "\x01Ole10Native" || ent == "Ole10Native" || ent.find("Ole10Native") != std::string::npos) {
                            std::string streamPath = "/" + ent;
                            POLE::Stream s(&st, streamPath.c_str());
                            if (!s.fail() && s.size() > 16) {
                                std::vector<uint8_t> sData(s.size());
                                s.read(sData.data(), sData.size());
                                if (parseOle10NativeStream(sData.data(), sData.size(), outFilename, outPayload)) {
                                    st.close();
                                    unlink(tmpPath.data());
                                    return true;
                                }
                            }
                        }
                    }
                    st.close();
                }
            }
            unlink(tmpPath.data());
        }
    }
    // 尝试直接作为 Ole10Native 流解析
    if (parseOle10NativeStream(oleBytes.data(), oleBytes.size(), outFilename, outPayload)) {
        return true;
    }
    return false;
}

static std::string decodeXmlEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '&') {
            if (in.substr(i, 4) == "&lt;") { out += '<'; i += 3; }
            else if (in.substr(i, 4) == "&gt;") { out += '>'; i += 3; }
            else if (in.substr(i, 5) == "&amp;") { out += '&'; i += 4; }
            else if (in.substr(i, 6) == "&quot;") { out += '"'; i += 5; }
            else if (in.substr(i, 6) == "&apos;") { out += '\''; i += 5; }
            else { out += in[i]; }
        } else {
            out += in[i];
        }
    }

    return out;
}

static std::string extractAttr(const std::string& str, const std::string& attrName) {
    size_t tagClose = str.find('>');
    std::string tag = (tagClose != std::string::npos) ? str.substr(0, tagClose) : str;
    std::string pDouble = attrName + "=\"";
    size_t pos = tag.find(pDouble);
    if (pos != std::string::npos) {
        pos += pDouble.size();
        size_t end = tag.find('"', pos);
        if (end != std::string::npos) return tag.substr(pos, end - pos);
    }
    std::string pSingle = attrName + "='";
    pos = tag.find(pSingle);
    if (pos != std::string::npos) {
        pos += pSingle.size();
        size_t end = tag.find('\'', pos);
        if (end != std::string::npos) return tag.substr(pos, end - pos);
    }
    return "";
}

static std::string stripXmlFallback(const std::string& xml) {
    std::string res;
    res.reserve(xml.size());
    size_t cur = 0;
    while (cur < xml.size()) {
        size_t acPos = xml.find("<mc:AlternateContent", cur);
        if (acPos == std::string::npos) {
            res.append(xml, cur, xml.size() - cur);
            break;
        }
        res.append(xml, cur, acPos - cur);
        size_t acEnd = xml.find("</mc:AlternateContent>", acPos);
        if (acEnd == std::string::npos) {
            res.append(xml, acPos, xml.size() - acPos);
            break;
        }
        acEnd += 22;
        std::string acChunk = xml.substr(acPos, acEnd - acPos);

        // 如果是墨迹批注（Choice 中包含 wordprocessingInk/ink），保留 Fallback 中的墨迹预渲染图片
        if (acChunk.find("wordprocessingInk") != std::string::npos || acChunk.find("ink/") != std::string::npos) {
            size_t fbPos = acChunk.find("<mc:Fallback");
            if (fbPos != std::string::npos) {
                size_t fbTagClose = acChunk.find('>', fbPos);
                size_t fbEnd = acChunk.find("</mc:Fallback>", fbTagClose);
                if (fbTagClose != std::string::npos && fbEnd != std::string::npos) {
                    std::string fbContent = acChunk.substr(fbTagClose + 1, fbEnd - (fbTagClose + 1));
                    res.append(fbContent);
                }
            }
        } else {
            // 普通 AlternateContent：剔除 Fallback，保留 Choice
            size_t fbPos = acChunk.find("<mc:Fallback");
            if (fbPos != std::string::npos) {
                size_t fbEnd = acChunk.find("</mc:Fallback>", fbPos);
                if (fbEnd != std::string::npos) {
                    acChunk.erase(fbPos, (fbEnd + 14) - fbPos);
                }
            }
            res.append(acChunk);
        }
        cur = acEnd;
    }
    return res;
}

static size_t findMatchingParagraphEnd(const std::string& xml, size_t pos) {
    size_t pTagClose = xml.find('>', pos);
    if (pTagClose == std::string::npos) return std::string::npos;
    if (pTagClose > 0 && xml[pTagClose - 1] == '/') {
        return pTagClose + 1; // self-closing <w:p ... />
    }
    size_t depth = 1;
    size_t cur = pTagClose + 1;
    while (cur < xml.size() && depth > 0) {
        size_t nextOpen = xml.find("<w:p", cur);
        size_t nextClose = xml.find("</w:p>", cur);
        if (nextClose == std::string::npos) break;

        if (nextOpen != std::string::npos && nextOpen < nextClose) {
            if (nextOpen + 4 < xml.size() && (xml[nextOpen + 4] == ' ' || xml[nextOpen + 4] == '>')) {
                size_t openClose = xml.find('>', nextOpen);
                if (openClose != std::string::npos) {
                    if (openClose > 0 && xml[openClose - 1] != '/') {
                        depth++;
                    }
                    cur = openClose + 1;
                    continue;
                }
            }
            cur = nextOpen + 4;
            continue;
        } else {
            depth--;
            if (depth == 0) {
                return nextClose + 6;
            }
            cur = nextClose + 6;
        }
    }

    return std::string::npos;
}

static bool parseImageDimensions(const uint8_t* data, size_t size, std::string& format, int& width, int& height) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        format = "png";
        if (size >= 24) {
            width = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
            height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
            return width > 0 && height > 0;
        }
    }

    else if (size >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        format = "jpeg";
        size_t i = 2;
        while (i + 8 < size) {
            if (data[i] != 0xFF) { ++i; continue; }
            uint8_t marker = data[i + 1];
            if (marker == 0xC0 || marker == 0xC2) {
                height = (data[i + 5] << 8) | data[i + 6];
                width = (data[i + 7] << 8) | data[i + 8];
                return width > 0 && height > 0;
            }
            if (marker == 0xD9 || marker == 0xDA) break;
            uint16_t len = (data[i + 2] << 8) | data[i + 3];
            if (len < 2) break;
            i += 2 + len;
        }
    }

    return false;
}

std::map<std::string, DocxStyle> DocxParser::parseStylesXml(const std::string& stylesXml) {
    std::map<std::string, DocxStyle> styles;

    // 解析文档全局默认属性 <w:docDefaults>
    DocxStyle docDefaults;
    docDefaults.styleId = "__docDefaults__";
    size_t ddPos = stylesXml.find("<w:docDefaults");
    if (ddPos != std::string::npos) {
        size_t ddEnd = stylesXml.find("</w:docDefaults>", ddPos);
        std::string ddXml = (ddEnd != std::string::npos) ? stylesXml.substr(ddPos, ddEnd - ddPos + 16) : stylesXml.substr(ddPos);
        size_t rfPos = ddXml.find("<w:rFonts");
        if (rfPos != std::string::npos) {
            std::string ea = extractAttr(ddXml.substr(rfPos), "w:eastAsia");
            std::string ascii = extractAttr(ddXml.substr(rfPos), "w:ascii");
            docDefaults.fontFamily = !ea.empty() ? ea : ascii;
        }
        size_t szPos = ddXml.find("<w:sz ");
        if (szPos != std::string::npos) {
            std::string val = extractAttr(ddXml.substr(szPos), "w:val");
            if (!val.empty()) docDefaults.fontSize = std::stod(val) / 2.0;
        }
    }
    styles["__docDefaults__"] = docDefaults;

    size_t pos = 0;
    while ((pos = stylesXml.find("<w:style ", pos)) != std::string::npos) {
        size_t end = stylesXml.find("</w:style>", pos);
        if (end == std::string::npos) break;
        end += 10;

        std::string sXml = stylesXml.substr(pos, end - pos);
        std::string styleId = extractAttr(sXml, "w:styleId");
        if (!styleId.empty()) {
            DocxStyle st;
            st.styleId = styleId;

            std::string typeVal = extractAttr(sXml, "w:type");
            std::string defVal = extractAttr(sXml, "w:default");
            if (defVal == "1" || defVal == "true") st.isDefault = true;

            size_t namePos = sXml.find("<w:name ");
            if (namePos != std::string::npos) {
                st.styleName = extractAttr(sXml.substr(namePos), "w:val");
            }

            size_t basePos = sXml.find("<w:basedOn ");
            if (basePos != std::string::npos) {
                st.basedOn = extractAttr(sXml.substr(basePos), "w:val");
            }

            size_t bPos = sXml.find("<w:b");
            if (bPos != std::string::npos) {
                std::string bVal = extractAttr(sXml.substr(bPos), "w:val");
                if (bVal == "0" || bVal == "false" || bVal == "off" || bVal == "none") st.bold = false;
                else st.bold = true;
            }
            size_t iPos = sXml.find("<w:i");
            if (iPos != std::string::npos) {
                std::string iVal = extractAttr(sXml.substr(iPos), "w:val");
                if (iVal == "0" || iVal == "false" || iVal == "off" || iVal == "none") st.italic = false;
                else st.italic = true;
            }

            size_t rfPos = sXml.find("<w:rFonts");
            if (rfPos != std::string::npos) {
                std::string ea = extractAttr(sXml.substr(rfPos), "w:eastAsia");
                std::string ascii = extractAttr(sXml.substr(rfPos), "w:ascii");
                st.fontFamily = !ea.empty() ? ea : ascii;
            }

            size_t szPos = sXml.find("<w:sz ");
            if (szPos != std::string::npos) {
                std::string val = extractAttr(sXml.substr(szPos), "w:val");
                if (!val.empty()) st.fontSize = std::stod(val) / 2.0;
            } else {
                size_t szCsPos = sXml.find("<w:szCs ");
                if (szCsPos != std::string::npos) {
                    std::string val = extractAttr(sXml.substr(szCsPos), "w:val");
                    if (!val.empty()) st.fontSize = std::stod(val) / 2.0;
                }
            }

            size_t colPos = sXml.find("<w:color ");
            if (colPos != std::string::npos) {
                std::string hex = extractAttr(sXml.substr(colPos), "w:val");
                st.color = ColorRGB::fromHex(hex);
            }

            size_t spPos = sXml.find("<w:spacing ");
            if (spPos != std::string::npos) {
                std::string before = extractAttr(sXml.substr(spPos), "w:before");
                std::string after = extractAttr(sXml.substr(spPos), "w:after");
                std::string line = extractAttr(sXml.substr(spPos), "w:line");
                if (!before.empty()) st.spaceBefore = std::stod(before) / 20.0;
                if (!after.empty()) st.spaceAfter = std::stod(after) / 20.0;
                if (!line.empty()) {
                    double lineDxa = std::stod(line);
                    if (lineDxa > 0) st.lineSpacing = lineDxa / 240.0;
                }
            }

            size_t jcPos = sXml.find("<w:jc ");
            if (jcPos != std::string::npos) {
                std::string jc = extractAttr(sXml.substr(jcPos), "w:val");
                if (jc == "center") st.align = Alignment::Center;
                else if (jc == "right") st.align = Alignment::Right;
                else if (jc == "both") st.align = Alignment::Justify;
                else if (jc == "left") st.align = Alignment::Left;
            }

            size_t knPos = sXml.find("<w:keepNext");
            if (knPos != std::string::npos) {
                std::string knVal = extractAttr(sXml.substr(knPos), "w:val");
                if (knVal != "0" && knVal != "false" && knVal != "off") st.keepNext = true;
            }

            styles[styleId] = st;
            if (!st.styleName.empty()) {
                styles[st.styleName] = st;
            }
            bool isParaStyle = (typeVal == "paragraph" || typeVal.empty());
            if (isParaStyle && (st.isDefault || st.styleName == "Normal" || styleId == "Normal")) {
                DocxStyle defSt = st;
                styles["__default_paragraph__"] = defSt;
            }
        }
        pos = end;
    }

    // 级联继承: 子样式继承父样式的未显式设置属性 (最多 6 轮避免环形依赖)
    for (int iter = 0; iter < 6; ++iter) {
        for (auto& kv : styles) {
            DocxStyle& child = kv.second;
            if (child.basedOn.empty()) continue;
            auto itParent = styles.find(child.basedOn);
            if (itParent == styles.end()) continue;
            const DocxStyle& parent = itParent->second;

            if (child.fontFamily.empty()) child.fontFamily = parent.fontFamily;
            if (child.fontSize == 11.0 && parent.fontSize != 11.0) child.fontSize = parent.fontSize;
            if (!child.color.isValid && parent.color.isValid) child.color = parent.color;
            if (child.align == Alignment::Left && parent.align != Alignment::Left) child.align = parent.align;
            if (child.spaceBefore == 0.0 && parent.spaceBefore != 0.0) child.spaceBefore = parent.spaceBefore;
            if (child.spaceAfter == 4.0 && parent.spaceAfter != 4.0) child.spaceAfter = parent.spaceAfter;
            if (child.lineSpacing == 1.35 && parent.lineSpacing != 1.35) child.lineSpacing = parent.lineSpacing;
        }
    }


    // 同步更新默认段落样式 (如果被继承刷新)
    auto itDocDef = styles.find("__docDefaults__");
    if (itDocDef != styles.end()) {
        auto& defP = styles["__default_paragraph__"];
        if (defP.fontFamily.empty() && !itDocDef->second.fontFamily.empty()) defP.fontFamily = itDocDef->second.fontFamily;
        if (defP.fontSize <= 0.0 && itDocDef->second.fontSize > 0.0) defP.fontSize = itDocDef->second.fontSize;
    }

    auto itDef = styles.find("__default_paragraph__");
    if (itDef != styles.end() && !itDef->second.basedOn.empty()) {
        auto itP = styles.find(itDef->second.basedOn);
        if (itP != styles.end()) {
            if (itDef->second.fontFamily.empty()) itDef->second.fontFamily = itP->second.fontFamily;
        }
    }


    return styles;
}

bool DocxParser::parse(const std::string& filePath, Document& outDoc, std::string& err) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        err = "Cannot open docx file: " + filePath;
        return false;
    }
    std::vector<uint8_t> zipData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // 1. 读取主文档 XML
    std::vector<uint8_t> xmlData;
    if (!extractFileFromZip(zipData, "word/document.xml", xmlData)) {
        err = "Cannot find word/document.xml in docx package";
        return false;
    }
    std::string docXml((const char*)xmlData.data(), xmlData.size());

    // 2. 读取样式表 XML
    std::map<std::string, DocxStyle> styles;
    std::vector<uint8_t> stylesData;
    if (extractFileFromZip(zipData, "word/styles.xml", stylesData)) {
        std::string sXml((const char*)stylesData.data(), stylesData.size());
        styles = parseStylesXml(sXml);
    }

    // 3. 读取并解析关系表 (Relationships)
    std::map<std::string, std::string> relsMap;
    std::map<std::string, std::string> headerRels;
    std::map<std::string, std::string> footerRels;
    std::vector<uint8_t> relsData;
    if (extractFileFromZip(zipData, "word/_rels/document.xml.rels", relsData)) {
        std::string relsXml((const char*)relsData.data(), relsData.size());
        size_t rPos = 0;
        while ((rPos = relsXml.find("<Relationship ", rPos)) != std::string::npos) {
            size_t rEnd = relsXml.find("/>", rPos);
            if (rEnd == std::string::npos) break;
            std::string tag = relsXml.substr(rPos, rEnd - rPos + 2);
            std::string id = extractAttr(tag, "Id");
            std::string type = extractAttr(tag, "Type");
            std::string target = extractAttr(tag, "Target");
            if (!id.empty() && !target.empty()) {
                if (target.find("word/") == std::string::npos) {
                    target = "word/" + target;
                }
                if (type.find("/header") != std::string::npos || target.find("header") != std::string::npos) {
                    headerRels[id] = target;
                } else if (type.find("/footer") != std::string::npos || target.find("footer") != std::string::npos) {
                    footerRels[id] = target;
                } else {
                    relsMap[id] = target;
                }
            }
            rPos = rEnd + 2;
        }
    }


    // 4. 解析文档节指定的页眉和页脚 (优先采用 default 引用，避免全量叠加重复绘制)
    std::string defaultHeaderId;
    std::string defaultFooterId;
    size_t sectPos = docXml.rfind("<w:sectPr");
    if (sectPos == std::string::npos) sectPos = docXml.find("<w:sectPr");
    if (sectPos != std::string::npos) {
        size_t sectEnd = docXml.find("</w:sectPr>", sectPos);
        std::string sectXml = (sectEnd != std::string::npos) ? docXml.substr(sectPos, sectEnd - sectPos + 11) : docXml.substr(sectPos);
        
        size_t hRefPos = 0;
        while ((hRefPos = sectXml.find("<w:headerReference", hRefPos)) != std::string::npos) {
            std::string tag = sectXml.substr(hRefPos, sectXml.find("/>", hRefPos) - hRefPos + 2);
            std::string tType = extractAttr(tag, "w:type");
            std::string rId = extractAttr(tag, "r:id");
            if (tType == "default" || defaultHeaderId.empty()) defaultHeaderId = rId;
            hRefPos += tag.size();
        }

        size_t fRefPos = 0;
        while ((fRefPos = sectXml.find("<w:footerReference", fRefPos)) != std::string::npos) {
            std::string tag = sectXml.substr(fRefPos, sectXml.find("/>", fRefPos) - fRefPos + 2);
            std::string tType = extractAttr(tag, "w:type");
            std::string rId = extractAttr(tag, "r:id");
            if (tType == "default" || defaultFooterId.empty()) defaultFooterId = rId;
            fRefPos += tag.size();
        }
    }


    // 优先加载正文节指定的唯一 default header/footer；若无引用则最多各加载 1 个
    if (!defaultHeaderId.empty() && headerRels.count(defaultHeaderId)) {
        std::vector<uint8_t> hData;
        if (extractFileFromZip(zipData, headerRels[defaultHeaderId], hData)) {
            std::string hXml((const char*)hData.data(), hData.size());
            hXml = stripXmlFallback(hXml);
            parseHeaderFooterXml(hXml, outDoc.headerParagraphs, styles);
        }
    }

    else if (!headerRels.empty()) {
        std::vector<uint8_t> hData;
        if (extractFileFromZip(zipData, headerRels.begin()->second, hData)) {
            std::string hXml((const char*)hData.data(), hData.size());
            hXml = stripXmlFallback(hXml);
            parseHeaderFooterXml(hXml, outDoc.headerParagraphs, styles);
        }
    }


    if (!defaultFooterId.empty() && footerRels.count(defaultFooterId)) {
        std::vector<uint8_t> fData;
        if (extractFileFromZip(zipData, footerRels[defaultFooterId], fData)) {
            std::string fXml((const char*)fData.data(), fData.size());
            fXml = stripXmlFallback(fXml);
            parseHeaderFooterXml(fXml, outDoc.footerParagraphs, styles);
        }
    }

    else if (!footerRels.empty()) {
        std::vector<uint8_t> fData;
        if (extractFileFromZip(zipData, footerRels.begin()->second, fData)) {
            std::string fXml((const char*)fData.data(), fData.size());
            fXml = stripXmlFallback(fXml);
            parseHeaderFooterXml(fXml, outDoc.footerParagraphs, styles);
        }
    }

    // 4.5 解析文档声明字体表 word/fontTable.xml
    std::vector<uint8_t> fontTableData;
    if (extractFileFromZip(zipData, "word/fontTable.xml", fontTableData)) {
        std::string ftXml((const char*)fontTableData.data(), fontTableData.size());
        size_t fPos = 0;
        while ((fPos = ftXml.find("<w:font ", fPos)) != std::string::npos) {
            size_t closeTag = ftXml.find('>', fPos);
            size_t endFont = ftXml.find("</w:font>", fPos);
            if (closeTag == std::string::npos) break;

            std::string fontHeader = ftXml.substr(fPos, closeTag - fPos + 1);
            std::string fontName = extractAttr(fontHeader, "w:name");
            std::string altName;

            if (endFont != std::string::npos && endFont > closeTag) {
                std::string fontBody = ftXml.substr(closeTag, endFont - closeTag);
                size_t altPos = fontBody.find("<w:altName");
                if (altPos != std::string::npos) {
                    altName = extractAttr(fontBody.substr(altPos), "w:val");
                }
                fPos = endFont + 9;
            } else {
                fPos = closeTag + 1;
            }

            if (!fontName.empty()) {
                DocFontDecl decl;
                decl.name = fontName;
                decl.altName = altName;
                outDoc.fontDeclarations.push_back(decl);
            }
        }
    }

    // 4.6 解析脚注 word/footnotes.xml
    std::vector<uint8_t> fnData;
    if (extractFileFromZip(zipData, "word/footnotes.xml", fnData)) {
        std::string fnXml((const char*)fnData.data(), fnData.size());
        fnXml = stripXmlFallback(fnXml);
        size_t fnPos = 0;
        while ((fnPos = fnXml.find("<w:footnote ", fnPos)) != std::string::npos) {
            size_t fnClose = fnXml.find('>', fnPos);
            size_t fnEnd = fnXml.find("</w:footnote>", fnClose);
            if (fnClose == std::string::npos || fnEnd == std::string::npos) break;
            std::string fnHeader = fnXml.substr(fnPos, fnClose - fnPos + 1);
            std::string fnId = extractAttr(fnHeader, "w:id");
            std::string fnType = extractAttr(fnHeader, "w:type");
            if (fnType != "separator" && fnType != "continuationSeparator" && !fnId.empty()) {
                std::string body = fnXml.substr(fnClose + 1, fnEnd - (fnClose + 1));
                FootnoteItem item;
                item.id = fnId;
                size_t pPos = 0;
                while ((pPos = body.find("<w:p", pPos)) != std::string::npos) {
                    size_t pEnd = body.find("</w:p>", pPos);
                    if (pEnd == std::string::npos) break;
                    pEnd += 6;
                    Paragraph p;
                    parseParagraphXml(body.substr(pPos, pEnd - pPos), p, styles);
                    item.paragraphs.push_back(p);
                    pPos = pEnd;
                }
                outDoc.footnotes[fnId] = item;
            }
            fnPos = fnEnd + 13;
        }
    }

    // 4.7 解析批注 word/comments.xml
    std::vector<uint8_t> cmData;
    if (extractFileFromZip(zipData, "word/comments.xml", cmData)) {
        std::string cmXml((const char*)cmData.data(), cmData.size());
        cmXml = stripXmlFallback(cmXml);
        size_t cmPos = 0;
        while ((cmPos = cmXml.find("<w:comment ", cmPos)) != std::string::npos) {
            size_t cmClose = cmXml.find('>', cmPos);
            size_t cmEnd = cmXml.find("</w:comment>", cmClose);
            if (cmClose == std::string::npos || cmEnd == std::string::npos) break;
            std::string cmHeader = cmXml.substr(cmPos, cmClose - cmPos + 1);
            std::string cmId = extractAttr(cmHeader, "w:id");
            std::string author = extractAttr(cmHeader, "w:author");
            std::string date = extractAttr(cmHeader, "w:date");
            std::string body = cmXml.substr(cmClose + 1, cmEnd - (cmClose + 1));
            CommentItem item;
            item.id = cmId;
            item.author = author;
            item.date = date;
            size_t pPos = 0;
            while ((pPos = body.find("<w:p", pPos)) != std::string::npos) {
                size_t pEnd = body.find("</w:p>", pPos);
                if (pEnd == std::string::npos) break;
                pEnd += 6;
                Paragraph p;
                parseParagraphXml(body.substr(pPos, pEnd - pPos), p, styles);
                item.paragraphs.push_back(p);
                pPos = pEnd;
            }
            outDoc.comments.push_back(item);
            cmPos = cmEnd + 12;
        }
    }

    // 5. 预解压文档关联的图片
    std::map<std::string, std::vector<uint8_t>> imageCache;
    for (const auto& kv : relsMap) {
        std::vector<uint8_t> imgBytes;
        if (extractFileFromZip(zipData, kv.second, imgBytes)) {
            imageCache[kv.first] = std::move(imgBytes);
        }
    }

    // 5.5 提取 docx 内嵌附件 (word/embeddings/)，并解析 Ole10Native 真实文件名与数据
    std::map<std::string, AttachmentItem> attachmentMap;
    size_t zPos = 0;
    while (zPos + 30 <= zipData.size()) {
        uint32_t sig = *(const uint32_t*)(zipData.data() + zPos);
        if (sig != 0x04034b50) break;

        uint16_t compMethod = *(const uint16_t*)(zipData.data() + zPos + 8);
        uint32_t compSize = *(const uint32_t*)(zipData.data() + zPos + 18);
        uint32_t uncompSize = *(const uint32_t*)(zipData.data() + zPos + 22);
        uint16_t nameLen = *(const uint16_t*)(zipData.data() + zPos + 26);
        uint16_t extraLen = *(const uint16_t*)(zipData.data() + zPos + 28);

        size_t namePos = zPos + 30;
        if (namePos + nameLen > zipData.size()) break;
        std::string fileName((const char*)zipData.data() + namePos, nameLen);

        size_t dataPos = namePos + nameLen + extraLen;
        if (dataPos + compSize > zipData.size()) break;

        zPos = dataPos + compSize;

        if (fileName.find("word/embeddings/") != std::string::npos) {
            std::vector<uint8_t> outBytes;
            bool ok = false;
            if (compMethod == 0) {
                outBytes.assign(zipData.data() + dataPos, zipData.data() + dataPos + compSize);
                ok = true;
            } else if (compMethod == 8) {
                outBytes.resize(uncompSize);
                z_stream strm;
                std::memset(&strm, 0, sizeof(strm));
                strm.next_in = (Bytef*)(zipData.data() + dataPos);
                strm.avail_in = compSize;
                strm.next_out = (Bytef*)outBytes.data();
                strm.avail_out = uncompSize;
                if (inflateInit2(&strm, -MAX_WBITS) == Z_OK) {
                    if (inflate(&strm, Z_FINISH) == Z_STREAM_END) ok = true;
                    inflateEnd(&strm);
                }
            }
            if (ok && !outBytes.empty()) {
                std::string parsedName;
                std::vector<uint8_t> parsedPayload;
                if (extractFromOle2Binary(outBytes, parsedName, parsedPayload) && !parsedPayload.empty()) {
                    AttachmentItem item(parsedName, std::move(parsedPayload));
                    attachmentMap[fileName] = item;
                    outDoc.attachments.push_back(item);
                } else {
                    size_t slash = fileName.rfind('/');
                    std::string baseName = (slash != std::string::npos) ? fileName.substr(slash + 1) : fileName;
                    AttachmentItem item(baseName, std::move(outBytes));
                    attachmentMap[fileName] = item;
                    outDoc.attachments.push_back(item);
                }
            }
        }
    }

    // 6. 层次化解析主文档
    docXml = stripXmlFallback(docXml);
    parseDocumentXmlWithMedia(docXml, outDoc, imageCache, styles, relsMap, attachmentMap);
    return true;
}

bool DocxParser::extractFileFromZip(const std::vector<uint8_t>& zipData, const std::string& targetName, std::vector<uint8_t>& out) {
    size_t pos = 0;
    while (pos + 30 <= zipData.size()) {
        uint32_t sig = *(const uint32_t*)(zipData.data() + pos);
        if (sig != 0x04034b50) break;

        uint16_t compMethod = *(const uint16_t*)(zipData.data() + pos + 8);
        uint32_t compSize = *(const uint32_t*)(zipData.data() + pos + 18);
        uint32_t uncompSize = *(const uint32_t*)(zipData.data() + pos + 22);
        uint16_t nameLen = *(const uint16_t*)(zipData.data() + pos + 26);
        uint16_t extraLen = *(const uint16_t*)(zipData.data() + pos + 28);

        size_t namePos = pos + 30;
        if (namePos + nameLen > zipData.size()) break;
        std::string fileName((const char*)zipData.data() + namePos, nameLen);

        size_t dataPos = namePos + nameLen + extraLen;
        if (dataPos + compSize > zipData.size()) break;

        pos = dataPos + compSize;

        if (fileName == targetName) {
            if (compMethod == 0) {
                out.assign(zipData.data() + dataPos, zipData.data() + dataPos + compSize);
                return true;
            } else if (compMethod == 8) {
                out.resize(uncompSize);
                z_stream strm;
                std::memset(&strm, 0, sizeof(strm));
                strm.next_in = (Bytef*)(zipData.data() + dataPos);
                strm.avail_in = compSize;
                strm.next_out = (Bytef*)out.data();
                strm.avail_out = uncompSize;

                if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;
                int ret = inflate(&strm, Z_FINISH);
                inflateEnd(&strm);
                return ret == Z_STREAM_END;
            }
            return false;
        }
    }

    return false;
}

void DocxParser::parseDocumentXmlWithMedia(const std::string& xml,
                                           Document& outDoc,
                                           const std::map<std::string, std::vector<uint8_t>>& imageCache,
                                           const std::map<std::string, DocxStyle>& styles,
                                           const std::map<std::string, std::string>& relsMap,
                                           const std::map<std::string, AttachmentItem>& attachmentMap) {
    // 0. 解析节属性与页面尺寸 <w:sectPr> (使用文档末尾主节属性)
    size_t sectPos = xml.rfind("<w:sectPr");
    if (sectPos == std::string::npos) sectPos = xml.find("<w:sectPr");
    if (sectPos != std::string::npos) {
        size_t sectEnd = xml.find("</w:sectPr>", sectPos);
        std::string sectXml = (sectEnd != std::string::npos) ? xml.substr(sectPos, sectEnd - sectPos + 11) : xml.substr(sectPos);

        size_t szPos = sectXml.find("<w:pgSz ");
        if (szPos != std::string::npos) {
            std::string wStr = extractAttr(sectXml.substr(szPos), "w:w");
            std::string hStr = extractAttr(sectXml.substr(szPos), "w:h");
            if (!wStr.empty()) outDoc.pageSetup.pageWidth = std::stod(wStr) / 20.0;
            if (!hStr.empty()) outDoc.pageSetup.pageHeight = std::stod(hStr) / 20.0;
        }

        size_t marPos = sectXml.find("<w:pgMar ");
        if (marPos != std::string::npos) {
            std::string top = extractAttr(sectXml.substr(marPos), "w:top");
            std::string bot = extractAttr(sectXml.substr(marPos), "w:bottom");
            std::string left = extractAttr(sectXml.substr(marPos), "w:left");
            std::string right = extractAttr(sectXml.substr(marPos), "w:right");
            std::string header = extractAttr(sectXml.substr(marPos), "w:header");
            std::string footer = extractAttr(sectXml.substr(marPos), "w:footer");
            if (!top.empty()) outDoc.pageSetup.marginTop = std::stod(top) / 20.0;
            if (!bot.empty()) outDoc.pageSetup.marginBottom = std::stod(bot) / 20.0;
            if (!left.empty()) outDoc.pageSetup.marginLeft = std::stod(left) / 20.0;
            if (!right.empty()) outDoc.pageSetup.marginRight = std::stod(right) / 20.0;
            if (!header.empty()) outDoc.pageSetup.headerMargin = std::stod(header) / 20.0;
            if (!footer.empty()) outDoc.pageSetup.footerMargin = std::stod(footer) / 20.0;
        }

        size_t gridPos = sectXml.find("<w:docGrid ");
        if (gridPos != std::string::npos) {
            std::string lpStr = extractAttr(sectXml.substr(gridPos), "w:linePitch");
            if (!lpStr.empty()) {
                outDoc.pageSetup.linePitch = std::stod(lpStr) / 20.0;
            }
        }
    }

    int docxInkCounter = 0;

    size_t pos = 0;
    while (pos < xml.size()) {
        size_t pPos = xml.find("<w:p", pos);
        size_t tblPos = xml.find("<w:tbl", pos);

        if (pPos == std::string::npos && tblPos == std::string::npos) break;

        if (tblPos != std::string::npos && (pPos == std::string::npos || tblPos < pPos)) {
            size_t tblEnd = xml.find("</w:tbl>", tblPos);
            if (tblEnd == std::string::npos) break;
            tblEnd += 8;

            std::string tblXml = xml.substr(tblPos, tblEnd - tblPos);
            Table table;
            parseTableXml(tblXml, table, styles);
            if (!table.rows.empty()) {
                outDoc.blocks.push_back(Block::makeTable(table));
            }
            pos = tblEnd;
        } else {
            size_t pEnd = findMatchingParagraphEnd(xml, pPos);
            if (pEnd == std::string::npos) break;

            std::string pXml = xml.substr(pPos, pEnd - pPos);

            // 如果段落中包含文本框 (DrawingML / VML txbxContent)，提取文本框内部段落
            size_t txbxPos = pXml.find("<w:txbxContent");
            if (txbxPos != std::string::npos) {
                size_t drawStart = pXml.rfind("<w:drawing", txbxPos);
                size_t drawEnd = std::string::npos;
                if (drawStart != std::string::npos) {
                    size_t dClose = pXml.find("</w:drawing>", txbxPos);
                    if (dClose != std::string::npos) drawEnd = dClose + 12;
                } else {
                    drawStart = pXml.rfind("<w:pict", txbxPos);
                    if (drawStart != std::string::npos) {
                        size_t dClose = pXml.find("</w:pict>", txbxPos);
                        if (dClose != std::string::npos) drawEnd = dClose + 9;
                    }
                }

                if (drawStart != std::string::npos && drawEnd != std::string::npos) {
                    std::string drawXml = pXml.substr(drawStart, drawEnd - drawStart);
                    size_t cStart = drawXml.find("<w:txbxContent");
                    size_t cEnd = drawXml.find("</w:txbxContent>", cStart);
                    if (cStart != std::string::npos && cEnd != std::string::npos) {
                        std::string cXml = drawXml.substr(cStart, cEnd - cStart);
                        size_t ipPos = 0;
                        while ((ipPos = cXml.find("<w:p", ipPos)) != std::string::npos) {
                            if (ipPos + 4 < cXml.size() && cXml[ipPos + 4] != ' ' && cXml[ipPos + 4] != '>') {
                                ipPos += 4;
                                continue;
                            }
                            size_t ipEnd = findMatchingParagraphEnd(cXml, ipPos);
                            if (ipEnd == std::string::npos) break;
                            std::string innerPXml = cXml.substr(ipPos, ipEnd - ipPos);
                            Paragraph tbPara;
                            parseParagraphXml(innerPXml, tbPara, styles);
                            if (!tbPara.runs.empty()) {
                                outDoc.blocks.push_back(Block::makeParagraph(tbPara));
                            }
                            ipPos = ipEnd;
                        }
                    }
                    pXml.erase(drawStart, drawEnd - drawStart);
                }
            }

            // 提取段落内嵌插图 (DrawingML / VML) 与手写墨迹批注 (wordprocessingInk / VML gfxdata ink)
            bool paraHasInk = false;
            std::vector<ImageBlock> paraInkImages;

            size_t drawPos = 0;
            while ((drawPos = pXml.find("<w:drawing", drawPos)) != std::string::npos) {
                size_t drawEnd = pXml.find("</w:drawing>", drawPos);
                if (drawEnd == std::string::npos) break;
                drawEnd += 12;
                std::string drawXml = pXml.substr(drawPos, drawEnd - drawPos);

                if (drawXml.find("wordprocessingInk") != std::string::npos || drawXml.find("ink/") != std::string::npos) {
                    pXml.erase(drawPos, drawEnd - drawPos);
                    continue;
                }

                size_t blipPos = drawXml.find("<a:blip");
                if (blipPos != std::string::npos) {
                    std::string rId = extractAttr(drawXml.substr(blipPos), "r:embed");
                    if (rId.empty()) rId = extractAttr(drawXml.substr(blipPos), "r:link");
                    auto it = imageCache.find(rId);
                    if (it != imageCache.end() && !it->second.empty()) {
                        std::string fmt;
                        int pw = 0, ph = 0;
                        if (parseImageDimensions(it->second.data(), it->second.size(), fmt, pw, ph)) {
                            double dw = 0.0, dh = 0.0;
                            // 优先解析 <wp:extent cx="..." cy="..."/>
                            size_t extPos = drawXml.find("<wp:extent");
                            if (extPos == std::string::npos) extPos = drawXml.find("<a:ext");
                            if (extPos != std::string::npos) {
                                std::string cxStr = extractAttr(drawXml.substr(extPos), "cx");
                                std::string cyStr = extractAttr(drawXml.substr(extPos), "cy");
                                if (!cxStr.empty() && !cyStr.empty()) {
                                    dw = std::stod(cxStr) / 12700.0; // 1 pt = 12700 EMUs
                                    dh = std::stod(cyStr) / 12700.0;
                                }
                            }
                            if (dw <= 0 || dh <= 0) {
                                dw = std::min(450.0, (double)pw);
                                dh = dw * ((double)ph / pw);
                            }
                            if (dw > 480.0) {
                                double scale = 480.0 / dw;
                                dw = 480.0;
                                dh *= scale;
                            }
                            if (dh > 600.0) {
                                double scale = 600.0 / dh;
                                dh = 600.0;
                                dw *= scale;
                            }
                            ImageBlock imgBlock(it->second, fmt, pw, ph, dw, dh);
                            outDoc.blocks.push_back(Block::makeImage(imgBlock));
                        }
                    }
                }
                drawPos = drawEnd;
            }

            // 支持 VML 插图与手写墨迹批注 <w:pict><v:shape><v:imagedata r:id="..."/></w:pict>
            size_t pictPos = 0;
            while ((pictPos = pXml.find("<w:pict", pictPos)) != std::string::npos) {
                size_t pictEnd = pXml.find("</w:pict>", pictPos);
                if (pictEnd == std::string::npos) break;
                pictEnd += 9;
                std::string pictXml = pXml.substr(pictPos, pictEnd - pictPos);

                bool isInkPict = (pictXml.find("ink") != std::string::npos || pictXml.find("gfxdata") != std::string::npos);

                size_t imgDataPos = pictXml.find("<v:imagedata");
                if (imgDataPos != std::string::npos) {
                    std::string rId = extractAttr(pictXml.substr(imgDataPos), "r:id");
                    if (rId.empty()) rId = extractAttr(pictXml.substr(imgDataPos), "r:href");
                    auto it = imageCache.find(rId);
                    if (it != imageCache.end() && !it->second.empty()) {
                        std::string fmt;
                        int pw = 0, ph = 0;
                        if (parseImageDimensions(it->second.data(), it->second.size(), fmt, pw, ph)) {
                            // 过滤微小占位点 (如 4x3 透明图)，仅对有效手写墨迹图设置批注
                            if (pw > 10 && ph > 10) {
                                if (isInkPict) {
                                    paraHasInk = true;
                                }
                                double dw = 92.0, dh = 74.0;
                                size_t shapePos = pictXml.find("<v:shape");
                                if (shapePos != std::string::npos) {
                                    std::string styleStr = extractAttr(pictXml.substr(shapePos), "style");
                                    size_t wIdx = styleStr.find("width:");
                                    if (wIdx != std::string::npos) {
                                        double val = std::strtod(styleStr.c_str() + wIdx + 6, nullptr);
                                        if (val > 0) dw = val;
                                    }
                                    size_t hIdx = styleStr.find("height:");
                                    if (hIdx != std::string::npos) {
                                        double val = std::strtod(styleStr.c_str() + hIdx + 7, nullptr);
                                        if (val > 0) dh = val;
                                    }
                                }
                                if (dw <= 0 || dh <= 0) {
                                    dw = std::min(450.0, (double)pw);
                                    dh = dw * ((double)ph / pw);
                                }
                                ImageBlock imgBlock(it->second, fmt, pw, ph, dw, dh);
                                if (isInkPict) {
                                    paraInkImages.push_back(imgBlock);
                                } else {
                                    outDoc.blocks.push_back(Block::makeImage(imgBlock));
                                }
                            }
                        }
                    }
                }
                pictPos = pictEnd;
            }

            // 支持段落内嵌对象/附件 <w:object><v:shape ...><v:imagedata r:id="..."/></v:shape><o:OLEObject r:id="..."/></w:object>
            std::vector<ImageBlock> paraAttachImages;
            size_t objPos = 0;
            while ((objPos = pXml.find("<w:object", objPos)) != std::string::npos) {
                size_t objEnd = pXml.find("</w:object>", objPos);
                if (objEnd == std::string::npos) break;
                objEnd += 11;
                std::string objXml = pXml.substr(objPos, objEnd - objPos);

                std::string iconRId;
                size_t imgDataPos = objXml.find("<v:imagedata");
                if (imgDataPos != std::string::npos) {
                    iconRId = extractAttr(objXml.substr(imgDataPos), "r:id");
                    if (iconRId.empty()) iconRId = extractAttr(objXml.substr(imgDataPos), "r:href");
                }

                std::string oleRId;
                size_t olePos = objXml.find("<o:OLEObject");
                if (olePos != std::string::npos) {
                    oleRId = extractAttr(objXml.substr(olePos), "r:id");
                }

                // 查找对应的附件项与真实文件名
                std::string attFilename;
                auto itAttRel = relsMap.find(oleRId);
                if (itAttRel != relsMap.end()) {
                    auto itAtt = attachmentMap.find("word/" + itAttRel->second);
                    if (itAtt == attachmentMap.end()) itAtt = attachmentMap.find(itAttRel->second);
                    if (itAtt != attachmentMap.end()) {
                        attFilename = itAtt->second.filename;
                    }
                }

                // 从 shape style 解析图标原始尺寸
                double dw = 90.0, dh = 90.0;
                size_t shapePos = objXml.find("<v:shape");
                if (shapePos != std::string::npos) {
                    std::string styleStr = extractAttr(objXml.substr(shapePos), "style");
                    size_t wIdx = styleStr.find("width:");
                    if (wIdx != std::string::npos) {
                        double val = std::strtod(styleStr.c_str() + wIdx + 6, nullptr);
                        if (val > 0) dw = val;
                    }
                    size_t hIdx = styleStr.find("height:");
                    if (hIdx != std::string::npos) {
                        double val = std::strtod(styleStr.c_str() + hIdx + 7, nullptr);
                        if (val > 0) dh = val;
                    }
                }

                auto itImg = imageCache.find(iconRId);
                if (itImg != imageCache.end() && !itImg->second.empty()) {
                    std::string fmt;
                    int pw = 0, ph = 0;
                    if (parseImageDimensions(itImg->second.data(), itImg->second.size(), fmt, pw, ph)) {
                        if (dw > 65.0 || dh > 65.0) {
                            dw = 58.0;
                            dh = 58.0;
                        }
                        ImageBlock imgBlock(itImg->second, fmt, pw, ph, dw, dh);
                        imgBlock.isAttachment = true;
                        imgBlock.attachmentIndex = outDoc.attachments.empty() ? 0 : (outDoc.attachments.size() - 1);
                        paraAttachImages.push_back(imgBlock);
                    }
                }

                pXml.erase(objPos, objEnd - objPos);
            }

            bool hadImage = (pXml.find("<a:blip") != std::string::npos || pXml.find("<v:imagedata") != std::string::npos || pXml.find("<w:object") != std::string::npos);
            Paragraph para;
            parseParagraphXml(pXml, para, styles);

            if (paraHasInk) {
                docxInkCounter++;
                TextRun inkRef;
                inkRef.fontSize = 9.0;
                inkRef.color = ColorRGB(0, 70, 180);
                inkRef.underline = true;
                inkRef.text = "[手写批注 " + std::to_string(docxInkCounter) + "]";
                inkRef.linkTarget = "comment_ink_" + std::to_string(docxInkCounter);
                para.runs.insert(para.runs.begin(), inkRef);

                CommentItem ci;
                ci.id = "ink_" + std::to_string(docxInkCounter);
                ci.author = "手写批注";
                ci.date = "墨迹批注";
                Paragraph cp;
                TextRun ctr;
                ctr.text = "手写墨迹原迹如下：";
                ctr.fontSize = 10.0;
                cp.runs.push_back(ctr);
                ci.paragraphs.push_back(cp);
                ci.images = paraInkImages;
                outDoc.comments.push_back(ci);
            }

            if (!para.runs.empty() || para.pageBreakBefore || !hadImage) {
                outDoc.blocks.push_back(Block::makeParagraph(para));
            }
            // 将附件图标紧随段落标题之后发射
            for (const auto& attImg : paraAttachImages) {
                outDoc.blocks.push_back(Block::makeImage(attImg));
            }
            pos = pEnd;
        }
    }

}

void DocxParser::parseParagraphXml(const std::string& pXml, Paragraph& para, const std::map<std::string, DocxStyle>& styles) {
    TextRun defaultRun;
    defaultRun.fontSize = 11.0;

    auto itDocDef = styles.find("__docDefaults__");
    if (itDocDef != styles.end()) {
        if (!itDocDef->second.fontFamily.empty()) defaultRun.fontFamily = itDocDef->second.fontFamily;
        if (itDocDef->second.fontSize > 0.0) defaultRun.fontSize = itDocDef->second.fontSize;
    }

    auto itDef = styles.find("__default_paragraph__");
    if (itDef != styles.end()) {
        para.spaceBefore = itDef->second.spaceBefore;
        para.spaceAfter = itDef->second.spaceAfter;
        para.lineSpacing = itDef->second.lineSpacing;
        para.align = itDef->second.align;
        defaultRun.fontSize = itDef->second.fontSize;
        defaultRun.bold = itDef->second.bold;
        defaultRun.italic = itDef->second.italic;
        defaultRun.color = itDef->second.color;
        defaultRun.fontFamily = itDef->second.fontFamily;
    }

    size_t prPos = pXml.find("<w:pPr");
    if (prPos != std::string::npos) {
        size_t prEnd = pXml.find("</w:pPr>", prPos);
        if (prEnd != std::string::npos) {
            std::string pPr = pXml.substr(prPos, prEnd - prPos);

            // 1. 样式表继承
            std::string pStyle;
            size_t stylePos = pPr.find("<w:pStyle ");
            if (stylePos != std::string::npos) {
                pStyle = extractAttr(pPr.substr(stylePos), "w:val");
            }
            if (!pStyle.empty()) {
                auto it = styles.find(pStyle);
                if (it != styles.end()) {
                    para.spaceBefore = it->second.spaceBefore;
                    para.spaceAfter = it->second.spaceAfter;
                    para.lineSpacing = it->second.lineSpacing;
                    para.align = it->second.align;
                    para.keepNext = it->second.keepNext;
                    defaultRun.fontSize = it->second.fontSize;
                    defaultRun.bold = it->second.bold;
                    defaultRun.italic = it->second.italic;
                    defaultRun.color = it->second.color;
                    defaultRun.fontFamily = it->second.fontFamily;
                }
            }

            size_t knPos = pPr.find("<w:keepNext");
            if (knPos != std::string::npos) {
                std::string knVal = extractAttr(pPr.substr(knPos), "w:val");
                if (knVal != "0" && knVal != "false" && knVal != "off") para.keepNext = true;
                else para.keepNext = false;
            }

            // 2. 标题层级
            if (!pStyle.empty()) {
                std::string matchName = pStyle;
                auto it = styles.find(pStyle);
                if (it != styles.end() && !it->second.styleName.empty()) {
                    matchName += " " + it->second.styleName;
                }
                std::string lowerStyle = matchName;
                std::transform(lowerStyle.begin(), lowerStyle.end(), lowerStyle.begin(), ::tolower);
                bool hasExplicitStyle = (it != styles.end());
                if (lowerStyle.find("heading1") != std::string::npos || lowerStyle.find("heading 1") != std::string::npos) {
                    para.headingLevel = 1;
                    if (!hasExplicitStyle) defaultRun.fontSize = 18.0;
                    defaultRun.bold = true;
                } else if (lowerStyle.find("heading2") != std::string::npos || lowerStyle.find("heading 2") != std::string::npos) {
                    para.headingLevel = 2;
                    if (!hasExplicitStyle) defaultRun.fontSize = 14.0;
                    defaultRun.bold = true;
                } else if (lowerStyle.find("heading3") != std::string::npos || lowerStyle.find("heading 3") != std::string::npos) {
                    para.headingLevel = 3;
                    if (!hasExplicitStyle) defaultRun.fontSize = 12.0;
                    defaultRun.bold = true;
                }
            }

            // 3. 段前强制分页 <w:pageBreakBefore/>
            if (pPr.find("<w:pageBreakBefore") != std::string::npos) {
                para.pageBreakBefore = true;
            }

            // 4. 段落间距与行高 <w:spacing w:before="240" w:after="120" w:line="360"/>
            size_t spPos = pPr.find("<w:spacing ");
            if (spPos != std::string::npos) {
                std::string bStr = extractAttr(pPr.substr(spPos), "w:before");
                std::string aStr = extractAttr(pPr.substr(spPos), "w:after");
                std::string lStr = extractAttr(pPr.substr(spPos), "w:line");
                std::string lrStr = extractAttr(pPr.substr(spPos), "w:lineRule");
                if (!bStr.empty()) para.spaceBefore = std::stod(bStr) / 20.0;
                if (!aStr.empty()) para.spaceAfter = std::stod(aStr) / 20.0;
                if (!lStr.empty()) {
                    double lineDxa = std::stod(lStr);
                    if (lineDxa > 0) {
                        if (lrStr == "exact" || lrStr == "atLeast") {
                            para.lineSpacing = (lineDxa / 20.0) / 12.0;
                        } else {
                            para.lineSpacing = lineDxa / 240.0;
                        }
                    }
                }
            }

            // 5. 缩进 <w:ind w:left="720" w:firstLine="480" w:hanging="360"/>
            size_t indPos = pPr.find("<w:ind ");
            if (indPos != std::string::npos) {
                std::string flStr = extractAttr(pPr.substr(indPos), "w:firstLine");
                std::string hStr = extractAttr(pPr.substr(indPos), "w:hanging");
                std::string lStr = extractAttr(pPr.substr(indPos), "w:left");
                std::string rStr = extractAttr(pPr.substr(indPos), "w:right");
                if (!flStr.empty()) para.firstLineIndent = std::stod(flStr) / 20.0;
                if (!hStr.empty()) {
                    double hVal = std::stod(hStr) / 20.0;
                    para.leftIndent += hVal;
                    para.firstLineIndent = -hVal;
                }
                if (!lStr.empty()) para.leftIndent += std::stod(lStr) / 20.0;
                if (!rStr.empty()) para.rightIndent = std::stod(rStr) / 20.0;
            }

            // 6. 对齐方式 <w:jc w:val="center|right|both"/>
            size_t jcPos = pPr.find("<w:jc ");
            if (jcPos != std::string::npos) {
                std::string jc = extractAttr(pPr.substr(jcPos), "w:val");
                if (jc == "center") para.align = Alignment::Center;
                else if (jc == "right") para.align = Alignment::Right;
                else if (jc == "both") para.align = Alignment::Justify;
                else if (jc == "left") para.align = Alignment::Left;
            }

            // 7. 段落底色
            size_t shdPos = pPr.find("<w:shd ");
            if (shdPos != std::string::npos) {
                std::string fill = extractAttr(pPr.substr(shdPos), "w:fill");
                para.bgColor = ColorRGB::fromHex(fill);
            }

            // 8. 段落默认字符属性 <w:pPr><w:rPr>
            size_t pRprPos = pPr.find("<w:rPr");
            if (pRprPos != std::string::npos) {
                std::string pRpr = pPr.substr(pRprPos);
                size_t szPos = pRpr.find("<w:sz ");
                if (szPos != std::string::npos) {
                    std::string val = extractAttr(pRpr.substr(szPos), "w:val");
                    if (!val.empty()) defaultRun.fontSize = std::stod(val) / 2.0;
                }
                size_t colPos = pRpr.find("<w:color ");
                if (colPos != std::string::npos) {
                    std::string hex = extractAttr(pRpr.substr(colPos), "w:val");
                    defaultRun.color = ColorRGB::fromHex(hex);
                }
                size_t rfPos = pRpr.find("<w:rFonts");
                if (rfPos != std::string::npos) {
                    std::string ea = extractAttr(pRpr.substr(rfPos), "w:eastAsia");
                    std::string ascii = extractAttr(pRpr.substr(rfPos), "w:ascii");
                    defaultRun.fontFamily = !ea.empty() ? ea : ascii;
                }
            }
        }
    }

    para.emptyLineHeight = defaultRun.fontSize * (para.lineSpacing > 0.0 ? para.lineSpacing : 1.0);

    // 检查项目符号或编号 <w:numPr>
    size_t numPos = pXml.find("<w:numPr");
    if (numPos != std::string::npos) {
        TextRun bulletRun = defaultRun;
        bulletRun.text = "• ";
        para.runs.push_back(bulletRun);
    }

    // 解析文字 Run 与 域代码 (<w:r>, <w:fldSimple>, <w:commentRangeStart>, <w:commentRangeEnd>)
    size_t curElem = 0;
    std::string activeFieldInstr;
    std::string activeCommentId;
    bool inFieldInstruction = false;
    bool inFieldResult = false;

    while (curElem < pXml.size()) {
        size_t nextR = pXml.find("<w:r", curElem);
        size_t nextFld = pXml.find("<w:fldSimple", curElem);
        size_t nextCrs = pXml.find("<w:commentRangeStart", curElem);
        size_t nextCre = pXml.find("<w:commentRangeEnd", curElem);

        // 优先处理在 Run 之前的批注范围开始/结束标记
        if (nextCrs != std::string::npos && (nextR == std::string::npos || nextCrs < nextR) && (nextFld == std::string::npos || nextCrs < nextFld)) {
            size_t tagClose = pXml.find('>', nextCrs);
            if (tagClose != std::string::npos) {
                std::string crsTag = pXml.substr(nextCrs, tagClose - nextCrs + 1);
                activeCommentId = extractAttr(crsTag, "w:id");
                curElem = tagClose + 1;
                continue;
            }
        }
        if (nextCre != std::string::npos && (nextR == std::string::npos || nextCre < nextR) && (nextFld == std::string::npos || nextCre < nextFld)) {
            size_t tagClose = pXml.find('>', nextCre);
            if (tagClose != std::string::npos) {
                activeCommentId.clear();
                curElem = tagClose + 1;
                continue;
            }
        }

        if (nextR == std::string::npos && nextFld == std::string::npos) break;

        if (nextFld != std::string::npos && (nextR == std::string::npos || nextFld < nextR)) {
            // 解析 <w:fldSimple ...> (支持配对闭合或自闭合)
            size_t tagClose = pXml.find('>', nextFld);
            if (tagClose == std::string::npos) break;

            size_t fldEnd = std::string::npos;
            std::string fldXml;
            if (tagClose > 0 && pXml[tagClose - 1] == '/') {
                // 自闭合 <w:fldSimple w:instr="..."/>
                fldEnd = tagClose + 1;
                fldXml = pXml.substr(nextFld, fldEnd - nextFld);
            } else {
                size_t closeTag = pXml.find("</w:fldSimple>", tagClose);
                if (closeTag != std::string::npos) {
                    fldEnd = closeTag + 14;
                    fldXml = pXml.substr(nextFld, fldEnd - nextFld);
                } else {
                    fldEnd = tagClose + 1;
                    fldXml = pXml.substr(nextFld, fldEnd - nextFld);
                }
            }
            std::string instr = extractAttr(fldXml, "w:instr");
            std::string upperInstr = instr;
            for (char& c : upperInstr) c = (char)::toupper((unsigned char)c);

            TextRun fldRun = defaultRun;
            fldRun.text.clear();
            // 继承 fldSimple 内部 <w:rPr> 样式属性
            size_t innerRpr = fldXml.find("<w:rPr");
            if (innerRpr != std::string::npos) {
                size_t innerRprEnd = fldXml.find("</w:rPr>", innerRpr);
                std::string rprStr = (innerRprEnd != std::string::npos) ? fldXml.substr(innerRpr, innerRprEnd - innerRpr + 8) : fldXml.substr(innerRpr);
                size_t szPos = rprStr.find("<w:sz ");
                if (szPos != std::string::npos) {
                    std::string val = extractAttr(rprStr.substr(szPos), "w:val");
                    if (!val.empty()) fldRun.fontSize = std::stod(val) / 2.0;
                }
                size_t colPos = rprStr.find("<w:color ");
                if (colPos != std::string::npos) {
                    std::string hex = extractAttr(rprStr.substr(colPos), "w:val");
                    if (!hex.empty() && hex != "auto") fldRun.color = ColorRGB::fromHex(hex);
                }
                size_t rfPos = rprStr.find("<w:rFonts");
                if (rfPos != std::string::npos) {
                    std::string ea = extractAttr(rprStr.substr(rfPos), "w:eastAsia");
                    std::string ascii = extractAttr(rprStr.substr(rfPos), "w:ascii");
                    fldRun.fontFamily = !ea.empty() ? ea : ascii;
                }
                if (rprStr.find("<w:b/>") != std::string::npos || rprStr.find("<w:b ") != std::string::npos) fldRun.bold = true;
                if (rprStr.find("<w:i/>") != std::string::npos || rprStr.find("<w:i ") != std::string::npos) fldRun.italic = true;
            }

            if (upperInstr.find("PAGE") != std::string::npos && upperInstr.find("NUMPAGES") == std::string::npos) {
                fldRun.text = "PAGE_NUM";
            } else if (upperInstr.find("NUMPAGES") != std::string::npos) {
                fldRun.text = "NUM_PAGES";
            } else {
                // 提取内部已渲染的内容或纯文本
                size_t tPos = 0;
                while ((tPos = fldXml.find("<w:t", tPos)) != std::string::npos) {
                    size_t tClose = fldXml.find('>', tPos);
                    size_t tEnd = fldXml.find("</w:t>", tClose);
                    if (tClose != std::string::npos && tEnd != std::string::npos) {
                        std::string t = fldXml.substr(tClose + 1, tEnd - tClose - 1);
                        if (t.find('<') != std::string::npos) {
                            std::string cleanT;
                            size_t k = 0;
                            while (k < t.size()) {
                                if (t[k] == '<') {
                                    size_t closeK = t.find('>', k);
                                    if (closeK != std::string::npos) k = closeK + 1;
                                    else break;
                                } else {
                                    cleanT += t[k++];
                                }
                            }
                            fldRun.text += decodeXmlEntities(cleanT);
                        } else {
                            fldRun.text += decodeXmlEntities(t);
                        }
                        tPos = tEnd + 6;
                    } else break;
                }
            }

            if (!fldRun.text.empty()) {
                para.runs.push_back(fldRun);
            }
            curElem = fldEnd;
            continue;
        }

        // 解析 <w:r>
        size_t rPos = nextR;
        size_t rEnd = pXml.find("</w:r>", rPos);
        if (rEnd == std::string::npos) break;
        rEnd += 6;

        std::string rXml = pXml.substr(rPos, rEnd - rPos);

        // 检查是否包含复杂域标记 <w:fldChar w:fldCharType="begin|separate|end"/>
        size_t fcPos = rXml.find("<w:fldChar");
        if (fcPos != std::string::npos) {
            std::string fcType = extractAttr(rXml.substr(fcPos), "w:fldCharType");
            if (fcType == "begin") {
                inFieldInstruction = true;
                inFieldResult = false;
                activeFieldInstr.clear();
                curElem = rEnd;
                continue;
            } else if (fcType == "separate") {
                inFieldInstruction = false;
                inFieldResult = true;
                curElem = rEnd;
                continue;
            } else if (fcType == "end") {
                if (inFieldInstruction && !inFieldResult) {
                    // 没有 separate 直接 end 的情况
                    std::string up = activeFieldInstr;
                    for (char& c : up) c = (char)::toupper((unsigned char)c);
                    if (up.find("PAGE") != std::string::npos && up.find("NUMPAGES") == std::string::npos) {
                        TextRun r = defaultRun;
                        r.text = "PAGE_NUM";
                        para.runs.push_back(r);
                    } else if (up.find("NUMPAGES") != std::string::npos) {
                        TextRun r = defaultRun;
                        r.text = "NUM_PAGES";
                        para.runs.push_back(r);
                    }
                }
                inFieldInstruction = false;
                inFieldResult = false;
                activeFieldInstr.clear();
                curElem = rEnd;
                continue;
            }
        }

        // 如果在域指令中，提取 <w:instrText>
        if (inFieldInstruction) {
            size_t itPos = 0;
            while ((itPos = rXml.find("<w:instrText", itPos)) != std::string::npos) {
                size_t closeTag = rXml.find('>', itPos);
                size_t endTag = rXml.find("</w:instrText>", closeTag);
                if (closeTag != std::string::npos && endTag != std::string::npos) {
                    activeFieldInstr += rXml.substr(closeTag + 1, endTag - closeTag - 1);
                    itPos = endTag + 14;
                } else break;
            }
            curElem = rEnd;
            continue;
        }

        TextRun run = defaultRun;
        run.text.clear();

        // 如果在已知 PAGE/NUMPAGES 域的结果部分，用该 Run 的实际样式注入占位符，不重复渲染
        bool isFieldResultRun = false;
        if (inFieldResult) {
            std::string up = activeFieldInstr;
            for (char& c : up) c = (char)::toupper((unsigned char)c);
            if (up.find("PAGE") != std::string::npos || up.find("NUMPAGES") != std::string::npos) {
                isFieldResultRun = true;
                inFieldResult = false; // 只转换域结果的第一个 run，随后解除
            }
        }

        size_t rprPos = rXml.find("<w:rPr");
        if (rprPos != std::string::npos) {
            size_t rprEnd = rXml.find("</w:rPr>", rprPos);
            std::string rprXml = (rprEnd != std::string::npos) ? rXml.substr(rprPos, rprEnd - rprPos + 8) : rXml.substr(rprPos);

            // Bold
            size_t bPos = rprXml.find("<w:b");
            if (bPos != std::string::npos) {
                std::string bVal = extractAttr(rprXml.substr(bPos), "w:val");
                if (bVal == "0" || bVal == "false" || bVal == "off" || bVal == "none") run.bold = false;
                else run.bold = true;
            }

            // Italic
            size_t iPos = rprXml.find("<w:i");
            if (iPos != std::string::npos) {
                std::string iVal = extractAttr(rprXml.substr(iPos), "w:val");
                if (iVal == "0" || iVal == "false" || iVal == "off" || iVal == "none") run.italic = false;
                else run.italic = true;
            }

            // Underline
            if (rprXml.find("<w:u ") != std::string::npos || rprXml.find("<w:u/>") != std::string::npos) {
                std::string uVal = extractAttr(rprXml.substr(rprXml.find("<w:u")), "w:val");
                if (uVal != "none" && uVal != "0" && uVal != "false") {
                    run.underline = true;
                    if (uVal == "double") run.doubleUnderline = true;
                }
            }

            // Strikethrough
            if (rprXml.find("<w:strike") != std::string::npos) {
                std::string sVal = extractAttr(rprXml.substr(rprXml.find("<w:strike")), "w:val");
                if (sVal.empty() || (sVal != "none" && sVal != "0" && sVal != "false" && sVal != "off")) run.strikethrough = true;
            }
            if (rprXml.find("<w:dstrike") != std::string::npos) {
                std::string sVal = extractAttr(rprXml.substr(rprXml.find("<w:dstrike")), "w:val");
                if (sVal.empty() || (sVal != "none" && sVal != "0" && sVal != "false" && sVal != "off")) {
                    run.doubleStrikethrough = true;
                    run.strikethrough = true;
                }
            }

            // Emphasis mark 重点号 <w:em w:val="dot|circle|underDot..."/>
            if (rprXml.find("<w:em") != std::string::npos) {
                std::string emVal = extractAttr(rprXml.substr(rprXml.find("<w:em")), "w:val");
                if (emVal.empty() || (emVal != "none" && emVal != "0" && emVal != "false")) {
                    run.emphasisMark = true;
                }
            }

            // Font size <w:sz> or <w:szCs>
            size_t szPos = rprXml.find("<w:sz ");
            if (szPos != std::string::npos) {
                std::string val = extractAttr(rprXml.substr(szPos), "w:val");
                if (!val.empty()) {
                    double halfPt = std::stod(val);
                    if (halfPt > 0) run.fontSize = halfPt / 2.0;
                }
            } else {
                size_t szCsPos = rprXml.find("<w:szCs ");
                if (szCsPos != std::string::npos) {
                    std::string val = extractAttr(rprXml.substr(szCsPos), "w:val");
                    if (!val.empty()) {
                        double halfPt = std::stod(val);
                        if (halfPt > 0) run.fontSize = halfPt / 2.0;
                    }
                }
            }

            // Font Family <w:rFonts>
            size_t rfPos = rprXml.find("<w:rFonts");
            if (rfPos != std::string::npos) {
                std::string ea = extractAttr(rprXml.substr(rfPos), "w:eastAsia");
                std::string ascii = extractAttr(rprXml.substr(rfPos), "w:ascii");
                run.fontFamily = !ea.empty() ? ea : ascii;
            }

            // Color <w:color>
            size_t colPos = rprXml.find("<w:color ");
            if (colPos != std::string::npos) {
                std::string hex = extractAttr(rprXml.substr(colPos), "w:val");
                if (!hex.empty() && hex != "auto") run.color = ColorRGB::fromHex(hex);
            }

            // Highlight / Shading
            size_t hlPos = rprXml.find("<w:highlight ");
            if (hlPos != std::string::npos) {
                std::string named = extractAttr(rprXml.substr(hlPos), "w:val");
                run.bgColor = ColorRGB::fromNamed(named);
            }
            size_t rShdPos = rprXml.find("<w:shd ");
            if (rShdPos != std::string::npos) {
                std::string fill = extractAttr(rprXml.substr(rShdPos), "w:fill");
                run.bgColor = ColorRGB::fromHex(fill);
            }
        }

        // 顺序扫描子元素: <w:t>, <w:br>, <w:cr>, <w:tab>, <w:instrText>
        size_t cur = 0;
        while (cur < rXml.size()) {
            size_t tagStart = rXml.find('<', cur);
            if (tagStart == std::string::npos) break;
            size_t tagEnd = rXml.find('>', tagStart);
            if (tagEnd == std::string::npos) break;
            std::string tag = rXml.substr(tagStart, tagEnd - tagStart + 1);

            if (tag == "<w:t>" || tag.rfind("<w:t ", 0) == 0) {
                if (tag.size() >= 3 && tag[tag.size() - 2] == '/') {
                    // 自闭合 <w:t/>
                    cur = tagEnd + 1;
                    continue;
                }
                size_t closeT = rXml.find("</w:t>", tagEnd);
                if (closeT != std::string::npos) {
                    std::string t = rXml.substr(tagEnd + 1, closeT - (tagEnd + 1));
                    // 防御性检查：标准 Word <w:t> 内部不能出现未转义的 XML 标签 (<w: 等)
                    if (t.find('<') != std::string::npos) {
                        // 包含未转义标签说明匹配越界或标签截断，剔除任何标签文本
                        std::string cleanT;
                        size_t k = 0;
                        while (k < t.size()) {
                            if (t[k] == '<') {
                                size_t closeK = t.find('>', k);
                                if (closeK != std::string::npos) k = closeK + 1;
                                else break;
                            } else {
                                cleanT += t[k++];
                            }
                        }
                        run.text += decodeXmlEntities(cleanT);
                    } else {
                        run.text += decodeXmlEntities(t);
                    }
                    cur = closeT + 6;
                    continue;
                }
            } else if (tag.rfind("<w:delText", 0) == 0) {
                // Word 修订/删除标记文本 <w:delText>
                size_t closeDel = rXml.find("</w:delText>", tagEnd);
                if (closeDel != std::string::npos) {
                    std::string t = rXml.substr(tagEnd + 1, closeDel - (tagEnd + 1));
                    run.text += decodeXmlEntities(t);
                    run.strikethrough = true;
                    cur = closeDel + 11;
                    continue;
                }
            } else if (tag.rfind("<w:instrText", 0) == 0) {
                size_t closeInstr = rXml.find("</w:instrText>", tagEnd);
                if (closeInstr != std::string::npos) {
                    std::string code = rXml.substr(tagEnd + 1, closeInstr - (tagEnd + 1));
                    std::string upperCode = code;
                    for (char& c : upperCode) c = (char)::toupper((unsigned char)c);
                    if (upperCode.find("PAGE") != std::string::npos && upperCode.find("NUMPAGES") == std::string::npos) {
                        run.text += "PAGE_NUM";
                    } else if (upperCode.find("NUMPAGES") != std::string::npos) {
                        run.text += "NUM_PAGES";
                    }
                    cur = closeInstr + 14;
                    continue;
                }
            } else if (tag.rfind("<w:br", 0) == 0) {
                if (tag.find("w:type=\"page\"") != std::string::npos || tag.find("w:type='page'") != std::string::npos) {
                    para.pageBreakBefore = true;
                } else {
                    run.text += "\n";
                }
            } else if (tag.rfind("<w:cr", 0) == 0) {
                run.text += "\n";
            } else if (tag.rfind("<w:tab", 0) == 0) {
                run.text += "\t";
            } else if (tag.rfind("<w:footnoteReference", 0) == 0) {
                std::string fId = extractAttr(tag, "w:id");
                if (!fId.empty()) {
                    run.text += "[" + fId + "]";
                }
            } else if (tag.rfind("<w:commentReference", 0) == 0) {
                std::string cId = extractAttr(tag, "w:id");
                if (!cId.empty()) {
                    run.text += "[批注" + cId + "]";
                    run.linkTarget = "comment_" + cId;
                    run.color = ColorRGB(0, 70, 180);
                }
            }
            cur = tagEnd + 1;
        }

        if (isFieldResultRun) {
            std::string up = activeFieldInstr;
            for (char& c : up) c = (char)::toupper((unsigned char)c);
            if (up.find("PAGE") != std::string::npos && up.find("NUMPAGES") == std::string::npos) {
                run.text = "PAGE_NUM";
            } else if (up.find("NUMPAGES") != std::string::npos) {
                run.text = "NUM_PAGES";
            }
        }

        if (!activeCommentId.empty()) {
            run.commentId = activeCommentId;
            if (!run.bgColor.isValid) {
                run.bgColor = ColorRGB(255, 255, 0); // 批注高亮底色 (纯黄)
            }
            run.color = ColorRGB(180, 50, 0); // 批注文字颜色变化
        }

        if (!run.text.empty()) {
            para.runs.push_back(run);
        }
        curElem = rEnd;
    }
}

void DocxParser::parseTableXml(const std::string& tblXml, Table& table, const std::map<std::string, DocxStyle>& styles) {
    // 1. 提取网格列宽定义 <w:tblGrid>
    size_t gridPos = tblXml.find("<w:tblGrid>");
    if (gridPos != std::string::npos) {
        size_t gridEnd = tblXml.find("</w:tblGrid>", gridPos);
        if (gridEnd != std::string::npos) {
            std::string gridXml = tblXml.substr(gridPos, gridEnd - gridPos);
            size_t colPos = 0;
            while ((colPos = gridXml.find("<w:gridCol", colPos)) != std::string::npos) {
                std::string wStr = extractAttr(gridXml.substr(colPos), "w:w");
                if (!wStr.empty()) {
                    double dxa = std::stod(wStr);
                    table.gridColWidthsPt.push_back(dxa / 20.0);
                }
                colPos = gridXml.find('>', colPos);
                if (colPos == std::string::npos) break;
                colPos += 1;
            }
        }
    }


    // 2. 表格全局缩进 <w:tblInd w:w="..."/> 与全局边距 <w:tblCellMar>
    size_t indPos = tblXml.find("<w:tblInd ");
    if (indPos != std::string::npos) {
        std::string wStr = extractAttr(tblXml.substr(indPos), "w:w");
        if (!wStr.empty()) table.tableIndent = std::stod(wStr) / 20.0;
    }

    double defaultPadTop = 1.0;
    double defaultPadBottom = 1.0;
    double defaultPadLeft = 5.4;
    double defaultPadRight = 5.4;

    size_t tblMarPos = tblXml.find("<w:tblCellMar>");
    if (tblMarPos != std::string::npos) {
        size_t tblMarEnd = tblXml.find("</w:tblCellMar>", tblMarPos);
        if (tblMarEnd != std::string::npos) {
            std::string marXml = tblXml.substr(tblMarPos, tblMarEnd - tblMarPos);
            size_t topP = marXml.find("<w:top ");
            if (topP != std::string::npos) {
                std::string val = extractAttr(marXml.substr(topP), "w:w");
                if (!val.empty()) defaultPadTop = std::stod(val) / 20.0;
            }
            size_t botP = marXml.find("<w:bottom ");
            if (botP != std::string::npos) {
                std::string val = extractAttr(marXml.substr(botP), "w:w");
                if (!val.empty()) defaultPadBottom = std::stod(val) / 20.0;
            }
            size_t leftP = marXml.find("<w:left ");
            if (leftP != std::string::npos) {
                std::string val = extractAttr(marXml.substr(leftP), "w:w");
                if (!val.empty()) defaultPadLeft = std::stod(val) / 20.0;
            }
            size_t rightP = marXml.find("<w:right ");
            if (rightP != std::string::npos) {
                std::string val = extractAttr(marXml.substr(rightP), "w:w");
                if (!val.empty()) defaultPadRight = std::stod(val) / 20.0;
            }
        }
    }


    // 3. 逐行解析 <w:tr>
    size_t trPos = 0;
    while ((trPos = tblXml.find("<w:tr", trPos)) != std::string::npos) {
        size_t trEnd = tblXml.find("</w:tr>", trPos);
        if (trEnd == std::string::npos) break;
        trEnd += 7;

        std::string trXml = tblXml.substr(trPos, trEnd - trPos);
        TableRow row;
        if (trXml.find("<w:tblHeader/>") != std::string::npos || trXml.find("<w:tblHeader ") != std::string::npos) {
            row.isHeader = true;
        }

        // 解析单元格 <w:tc>
        size_t tcPos = 0;
        while ((tcPos = trXml.find("<w:tc", tcPos)) != std::string::npos) {
            size_t tcEnd = trXml.find("</w:tc>", tcPos);
            if (tcEnd == std::string::npos) break;
            tcEnd += 7;

            std::string tcXml = trXml.substr(tcPos, tcEnd - tcPos);
            TableCell cell;
            cell.padTop = defaultPadTop;
            cell.padBottom = defaultPadBottom;
            cell.padLeft = defaultPadLeft;
            cell.padRight = defaultPadRight;

            size_t tcPrPos = tcXml.find("<w:tcPr>");
            if (tcPrPos != std::string::npos) {
                size_t tcPrEnd = tcXml.find("</w:tcPr>", tcPrPos);
                if (tcPrEnd != std::string::npos) {
                    std::string tcPr = tcXml.substr(tcPrPos, tcPrEnd - tcPrPos);

                    // 单元格宽度 <w:tcW w:w="..." w:type="dxa"/>
                    size_t wPos = tcPr.find("<w:tcW ");
                    if (wPos != std::string::npos) {
                        std::string wStr = extractAttr(tcPr.substr(wPos), "w:w");
                        if (!wStr.empty()) cell.widthPt = std::stod(wStr) / 20.0;
                    }

                    // 单元格底色 <w:shd w:fill="..."/>
                    size_t shdPos = tcPr.find("<w:shd ");
                    if (shdPos != std::string::npos) {
                        std::string fill = extractAttr(tcPr.substr(shdPos), "w:fill");
                        cell.bgColor = ColorRGB::fromHex(fill);
                    }

                    // 单元格边距 <w:tcMar>
                    size_t marPos = tcPr.find("<w:tcMar>");
                    if (marPos != std::string::npos) {
                        size_t marEnd = tcPr.find("</w:tcMar>", marPos);
                        if (marEnd != std::string::npos) {
                            std::string marXml = tcPr.substr(marPos, marEnd - marPos);
                            size_t topP = marXml.find("<w:top ");
                            if (topP != std::string::npos) {
                                std::string val = extractAttr(marXml.substr(topP), "w:w");
                                if (!val.empty()) cell.padTop = std::stod(val) / 20.0;
                            }
                            size_t botP = marXml.find("<w:bottom ");
                            if (botP != std::string::npos) {
                                std::string val = extractAttr(marXml.substr(botP), "w:w");
                                if (!val.empty()) cell.padBottom = std::stod(val) / 20.0;
                            }
                            size_t leftP = marXml.find("<w:left ");
                            if (leftP != std::string::npos) {
                                std::string val = extractAttr(marXml.substr(leftP), "w:w");
                                if (!val.empty()) cell.padLeft = std::stod(val) / 20.0;
                            }
                            size_t rightP = marXml.find("<w:right ");
                            if (rightP != std::string::npos) {
                                std::string val = extractAttr(marXml.substr(rightP), "w:w");
                                if (!val.empty()) cell.padRight = std::stod(val) / 20.0;
                            }
                        }
                    }
                }
            }

            // 解析单元格内的段落
            size_t pPos = 0;
            while ((pPos = tcXml.find("<w:p", pPos)) != std::string::npos) {
                size_t pEnd = tcXml.find("</w:p>", pPos);
                if (pEnd == std::string::npos) break;
                pEnd += 6;

                std::string pXml = tcXml.substr(pPos, pEnd - pPos);
                Paragraph cp;
                parseParagraphXml(pXml, cp, styles);
                if (!cp.runs.empty()) {
                    cell.paragraphs.push_back(cp);
                }
                pPos = pEnd;
            }

            row.cells.push_back(cell);
            tcPos = tcEnd;
        }

        table.rows.push_back(row);
        trPos = trEnd;
    }
}

void DocxParser::parseHeaderFooterXml(const std::string& xml, std::vector<Paragraph>& outParas, const std::map<std::string, DocxStyle>& styles) {
    size_t pos = 0;
    bool seenContent = false;
    while ((pos = xml.find("<w:p", pos)) != std::string::npos) {
        if (pos + 4 < xml.size() && xml[pos + 4] != ' ' && xml[pos + 4] != '>') {
            pos += 4;
            continue;
        }
        size_t pEnd = findMatchingParagraphEnd(xml, pos);
        if (pEnd == std::string::npos) break;

        std::string pXml = xml.substr(pos, pEnd - pos);

        // 检查段落中是否包含文本框 (DrawingML / VML txbxContent)
        size_t txbxPos = pXml.find("<w:txbxContent");
        if (txbxPos != std::string::npos) {
            size_t drawStart = pXml.rfind("<w:drawing", txbxPos);
            size_t drawEnd = std::string::npos;
            if (drawStart != std::string::npos) {
                size_t dClose = pXml.find("</w:drawing>", txbxPos);
                if (dClose != std::string::npos) drawEnd = dClose + 12;
            } else {
                drawStart = pXml.rfind("<w:pict", txbxPos);
                if (drawStart != std::string::npos) {
                    size_t dClose = pXml.find("</w:pict>", txbxPos);
                    if (dClose != std::string::npos) drawEnd = dClose + 9;
                }
            }

            if (drawStart != std::string::npos && drawEnd != std::string::npos) {
                std::string drawXml = pXml.substr(drawStart, drawEnd - drawStart);

                // 解析文本框水平对齐方式
                Alignment tbAlign = Alignment::Left;
                size_t posH = drawXml.find("<wp:positionH");
                if (posH != std::string::npos) {
                    size_t endH = drawXml.find("</wp:positionH>", posH);
                    if (endH != std::string::npos) {
                        std::string hXml = drawXml.substr(posH, endH - posH);
                        if (hXml.find("<wp:align>center</wp:align>") != std::string::npos) tbAlign = Alignment::Center;
                        else if (hXml.find("<wp:align>right</wp:align>") != std::string::npos) tbAlign = Alignment::Right;
                        else if (hXml.find("<wp:align>left</wp:align>") != std::string::npos) tbAlign = Alignment::Left;
                    }
                }

                // 提取文本框内部段落
                size_t cStart = drawXml.find("<w:txbxContent");
                size_t cEnd = drawXml.find("</w:txbxContent>", cStart);
                bool extractedTb = false;
                seenContent = true;
                if (cStart != std::string::npos && cEnd != std::string::npos) {
                    std::string cXml = drawXml.substr(cStart, cEnd - cStart);
                    size_t ipPos = 0;
                    while ((ipPos = cXml.find("<w:p", ipPos)) != std::string::npos) {
                        if (ipPos + 4 < cXml.size() && cXml[ipPos + 4] != ' ' && cXml[ipPos + 4] != '>') {
                            ipPos += 4;
                            continue;
                        }
                        size_t ipEnd = findMatchingParagraphEnd(cXml, ipPos);
                        if (ipEnd == std::string::npos) break;
                        std::string innerPXml = cXml.substr(ipPos, ipEnd - ipPos);
                        Paragraph tbPara;
                        parseParagraphXml(innerPXml, tbPara, styles);
                        if (!tbPara.runs.empty()) {
                            if (tbPara.align == Alignment::Left && tbAlign != Alignment::Left) {
                                tbPara.align = tbAlign;
                            }
                            outParas.push_back(std::move(tbPara));
                            extractedTb = true;
                        }
                        ipPos = ipEnd;
                    }
                }

                // 将 drawingXml 从 pXml 中剔除，解析外层段落原有文字
                pXml.erase(drawStart, drawEnd - drawStart);
                Paragraph outerPara;
                parseParagraphXml(pXml, outerPara, styles);
                bool hasVisibleText = false;
                for (const auto& r : outerPara.runs) {
                    if (r.text.find_first_not_of(" \t\r\n") != std::string::npos) {
                        hasVisibleText = true;
                        break;
                    }
                }
                if (hasVisibleText) {
                    if (extractedTb) {
                        outerPara.sameLineAsPrev = true;
                    }
                    outParas.push_back(std::move(outerPara));
                }
                pos = pEnd;
                continue;
            }
        }

        Paragraph para;
        parseParagraphXml(pXml, para, styles);
        outParas.push_back(std::move(para));
        pos = pEnd;
    }
}

} // namespace doc2pdf