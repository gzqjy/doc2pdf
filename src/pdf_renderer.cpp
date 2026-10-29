#include "doc2pdf/pdf_renderer.hpp"
#include "doc2pdf/image_utils.hpp"
#include "doc2pdf/utf_utils.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#include <cstring>
#include <cstdlib>
#include <zlib.h>
#include <memory>
#include <cstdint>
#include <set>
#include <dirent.h>
#include <sys/stat.h>

#if __has_include(<ft2build.h>)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#define HAVE_FREETYPE 1
#else
#define HAVE_FREETYPE 0
#endif

namespace doc2pdf {

#if HAVE_FREETYPE
static std::string decodeSfntName(const FT_SfntName& sfnt) {
    if (sfnt.platform_id == 3 && sfnt.encoding_id == 1) { // Windows Unicode (UTF-16BE)
        std::string utf8;
        for (unsigned int i = 0; i + 1 < sfnt.string_len; i += 2) {
            uint16_t cp = (static_cast<uint16_t>(sfnt.string[i]) << 8) | static_cast<uint16_t>(sfnt.string[i + 1]);
            if (cp < 0x80) {
                utf8 += static_cast<char>(cp);
            } else if (cp < 0x800) {
                utf8 += static_cast<char>(0xC0 | (cp >> 6));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                utf8 += static_cast<char>(0xE0 | (cp >> 12));
                utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        return utf8;
    } else if (sfnt.platform_id == 1 && sfnt.encoding_id == 0) { // Mac Roman
        return std::string(reinterpret_cast<const char*>(sfnt.string), sfnt.string_len);
    } else if (sfnt.platform_id == 0) { // Unicode
        if (sfnt.string_len >= 2) {
            std::string utf8;
            for (unsigned int i = 0; i + 1 < sfnt.string_len; i += 2) {
                uint16_t cp = (static_cast<uint16_t>(sfnt.string[i]) << 8) | static_cast<uint16_t>(sfnt.string[i + 1]);
                if (cp < 0x80) {
                    utf8 += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    utf8 += static_cast<char>(0xC0 | (cp >> 6));
                    utf8 += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    utf8 += static_cast<char>(0xE0 | (cp >> 12));
                    utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8 += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            return utf8;
        }
    }
    return "";
}

struct SystemFontEntry {
    std::string filePath;
    int faceIndex = 0;
    std::string postscriptName;
    std::string familyName;
    std::vector<std::string> names;
};
#endif

struct FontGlyphRegistry {
    std::map<uint32_t, uint32_t> gidToUnicode; // gid -> codepoint
    std::map<uint32_t, int> gidWidths;         // gid -> 1000 scale width
};

struct PdfFontInfo {
    std::string tag;        // e.g. "F1", "F2", "F3", ...
    std::string baseFont;   // 字体逻辑名称 (与 DOCX 声明一致)
    std::string postscriptName; // 真实 PostScript 字体名
    std::vector<std::string> aliases; // 别名集合 (中英互通)
    std::string filePath;   // 系统物理字体绝对路径
    int faceIndex = 0;
    double fontLineHeightMultiplier = 1.15; // 真实字体固有行高倍率 (face->height / units_per_EM)
    FT_Face face = nullptr; // FreeType 句柄
    FontGlyphRegistry registry;
    std::vector<uint8_t> rawBytes;
    std::vector<uint8_t> compBytes;
    std::vector<uint8_t> compCMapBytes;

    size_t fontObjId = 0;
    size_t cidObjId = 0;
    size_t descObjId = 0;
    size_t fontFileObjId = 0;
    size_t toUnicodeObjId = 0;

    PdfFontInfo() : faceIndex(0), fontLineHeightMultiplier(1.15), face(nullptr), fontObjId(0), cidObjId(0), descObjId(0), fontFileObjId(0), toUnicodeObjId(0) {}
};

static inline std::string normalizeFontName(const std::string& s) {
    std::string res;
    for (char c : s) {
        if (c != ' ' && c != '-' && c != '_' && c != '\t' && c != '\r' && c != '\n') {
            res += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
    }
    return res;
}

static inline std::string sanitizePdfName(const std::string& name) {
    std::string res;
    for (char c : name) {
        if (c == ' ' || c == '#' || c == '/' || c == '(' || c == ')' || c == '[' || c == ']' || c == '<' || c == '>' || c == '{' || c == '}' || c == '%') {
            char buf[4];
            snprintf(buf, sizeof(buf), "#%02X", static_cast<unsigned char>(c));
            res += buf;
        } else if (static_cast<unsigned char>(c) < 33 || static_cast<unsigned char>(c) > 126) {
            char buf[4];
            snprintf(buf, sizeof(buf), "#%02X", static_cast<unsigned char>(c));
            res += buf;
        } else {
            res += c;
        }
    }
    return res.empty() ? "Font" : res;
}

static void scanFontsInDir(const std::string& dirPath, std::vector<std::string>& outFiles, int depth = 0) {
    if (depth > 6) return;
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        std::string fullPath = dirPath + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scanFontsInDir(fullPath, outFiles, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                std::string name = entry->d_name;
                size_t dotPos = name.rfind('.');
                if (dotPos != std::string::npos) {
                    std::string ext = name.substr(dotPos);
                    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    if (ext == ".ttf" || ext == ".ttc" || ext == ".otf") {
                        outFiles.push_back(fullPath);
                    }
                }
            }
        }
    }
    closedir(dir);
}

static inline bool isCjkChar(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

static inline bool isLatinOrDigitChar(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

class TextMetricsEngine {
public:
    TextMetricsEngine() : m_ft(nullptr) {
#if HAVE_FREETYPE
        if (FT_Init_FreeType(&m_ft) == 0) {
            scanSystemFonts();
        }
#endif
    }

    ~TextMetricsEngine() {
#if HAVE_FREETYPE
        for (auto& f : m_allFonts) {
            if (f && f->face) {
                FT_Done_Face(f->face);
                f->face = nullptr;
            }
        }
        if (m_ft) {
            FT_Done_FreeType(m_ft);
            m_ft = nullptr;
        }
#endif
    }

    // 根据传入的 Document 中声明与使用的字体，纯动态加载与匹配物理字体
    void initForDocument(const Document& doc) {
#if HAVE_FREETYPE
        if (!m_ft) return;

        // 1. 动态探测系统最佳的简体中文默认字体与拉丁默认字体
        auto bestCjkEntry = findBestCjkFontEntry();
        auto bestLatinEntry = findBestLatinFontEntry();

        // 优先常驻加载系统最佳简体中文兜底字体与拉丁字体，确保汉字字符库全覆盖
        if (bestCjkEntry) {
            loadFontFile("FallbackCJK", bestCjkEntry);
        }
        if (bestLatinEntry && bestLatinEntry != bestCjkEntry) {
            loadFontFile("FallbackLatin", bestLatinEntry);
        }

        std::set<std::string> requestedFonts;
        std::map<std::string, std::string> altMap;

        // 2. 提取文档在 fontTable.xml 中声明的字体与别名
        for (const auto& decl : doc.fontDeclarations) {
            if (!decl.name.empty()) {
                requestedFonts.insert(decl.name);
                if (!decl.altName.empty()) {
                    altMap[decl.name] = decl.altName;
                    requestedFonts.insert(decl.altName);
                }
            }
        }

        // 3. 收集正文、页眉、页脚中实际出现的 fontFamily
        auto collectFromP = [&](const Paragraph& p) {
            for (const auto& r : p.runs) {
                if (!r.fontFamily.empty()) requestedFonts.insert(r.fontFamily);
            }
        };
        for (const auto& p : doc.headerParagraphs) collectFromP(p);
        for (const auto& p : doc.footerParagraphs) collectFromP(p);
        for (const auto& b : doc.blocks) {
            if (b.type == BlockType::Paragraph) collectFromP(b.para);
            else if (b.type == BlockType::Table) {
                for (const auto& row : b.table.rows) {
                    for (const auto& cell : row.cells) {
                        for (const auto& p : cell.paragraphs) collectFromP(p);
                    }
                }
            }
        }

        // 4. 动态针对所需字体在系统已安装字体中进行匹配并加载
        for (const auto& fontName : requestedFonts) {
            std::string alt = (altMap.count(fontName)) ? altMap[fontName] : "";
            auto entry = findEntryForFont(fontName, alt);
            if (entry) {
                loadFontFile(fontName, entry);
            }
        }

        // 5. 确保默认字体指针明确有效
        if (bestCjkEntry) {
            for (const auto& f : m_allFonts) {
                if (f->filePath == bestCjkEntry->filePath && f->faceIndex == bestCjkEntry->faceIndex) {
                    m_defaultCjkFont = f;
                    break;
                }
            }
        }
        if (bestLatinEntry) {
            for (const auto& f : m_allFonts) {
                if (f->filePath == bestLatinEntry->filePath && f->faceIndex == bestLatinEntry->faceIndex) {
                    m_defaultLatinFont = f;
                    break;
                }
            }
        }
        if (!m_defaultCjkFont && !m_allFonts.empty()) {
            m_defaultCjkFont = m_allFonts[0];
        }
        if (!m_defaultLatinFont && !m_allFonts.empty()) {
            m_defaultLatinFont = m_allFonts[0];
        }
#endif
    }

    const std::vector<std::shared_ptr<PdfFontInfo>>& getAllFonts() const {
        return m_allFonts;
    }

    std::vector<std::shared_ptr<PdfFontInfo>>& getAllFontsMutable() {
        return m_allFonts;
    }

    bool hasCjk() const {
        return !m_allFonts.empty();
    }

    std::shared_ptr<PdfFontInfo> getFontForFamily(const std::string& family, uint32_t cp = 0) const {
        if (!family.empty()) {
            std::string norm = normalizeFontName(family);
            for (const auto& f : m_allFonts) {
                if (normalizeFontName(f->baseFont) == norm) return f;
                for (const auto& al : f->aliases) {
                    if (normalizeFontName(al) == norm) return f;
                }
            }
            // 模糊前缀匹配
            for (const auto& f : m_allFonts) {
                std::string fNorm = normalizeFontName(f->baseFont);
                if (fNorm.find(norm) != std::string::npos || norm.find(fNorm) != std::string::npos) {
                    return f;
                }
                for (const auto& al : f->aliases) {
                    std::string aNorm = normalizeFontName(al);
                    if (aNorm.find(norm) != std::string::npos || norm.find(aNorm) != std::string::npos) {
                        return f;
                    }
                }
            }
        }
        if (cp > 0 && cp < 128 && m_defaultLatinFont) {
            return m_defaultLatinFont;
        }
        if (m_defaultCjkFont) return m_defaultCjkFont;
        if (!m_allFonts.empty()) return m_allFonts[0];
        return nullptr;
    }

    std::pair<uint32_t, std::shared_ptr<PdfFontInfo>> getGlyphIndexAndFont(const std::string& family, uint32_t cp) const {
        auto preferred = getFontForFamily(family, cp);
        if (preferred && preferred->face) {
            FT_UInt gid = FT_Get_Char_Index(preferred->face, cp);
            if (gid != 0) {
                return std::make_pair(static_cast<uint32_t>(gid), preferred);
            }
        }
        // 若首选字体缺少该字符字形，优先尝试系统最佳默认 CJK 字体
        if (m_defaultCjkFont && m_defaultCjkFont != preferred && m_defaultCjkFont->face) {
            FT_UInt gid = FT_Get_Char_Index(m_defaultCjkFont->face, cp);
            if (gid != 0) {
                return std::make_pair(static_cast<uint32_t>(gid), m_defaultCjkFont);
            }
        }
        for (const auto& f : m_allFonts) {
            if (f != preferred && f != m_defaultCjkFont && f && f->face) {
                FT_UInt gid = FT_Get_Char_Index(f->face, cp);
                if (gid != 0) {
                    return std::make_pair(static_cast<uint32_t>(gid), f);
                }
            }
        }
        return std::make_pair(0, preferred ? preferred : m_defaultCjkFont);
    }

    int getGlyphWidth1000(FT_Face face, uint32_t cp, uint32_t gid) const {
#if HAVE_FREETYPE
        if (face && gid != 0) {
            FT_Set_Char_Size(face, 0, 1000 * 64, 72, 72);
            if (FT_Load_Glyph(face, gid, FT_LOAD_DEFAULT) == 0) {
                return static_cast<int>(face->glyph->advance.x >> 6);
            }
        }
#endif
        if (cp < 128) {
            if (cp == ' ' || cp == '\t') return 350;
            if (cp == 'i' || cp == 'l' || cp == '.' || cp == ',' || cp == ':' || cp == ';') return 280;
            if (cp == 'm' || cp == 'w' || cp == 'M' || cp == 'W' || cp == '@') return 820;
            if (cp >= 'A' && cp <= 'Z') return 650;
            return 520;
        }
        return 1000;
    }

    double measureCharWidth(uint32_t cp, double fontSize, const std::string& fontFamily = "") const {
#if HAVE_FREETYPE
        auto res = getGlyphIndexAndFont(fontFamily, cp);
        if (res.second && res.second->face && res.first != 0) {
            FT_Face face = res.second->face;
            FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSize * 64.0), 72, 72);
            if (FT_Load_Glyph(face, res.first, FT_LOAD_DEFAULT) == 0) {
                double adv = static_cast<double>(face->glyph->advance.x) / 64.0;
                // 全角东亚标点符号及字符在方块字排版中占据标准 1.0 em 网格
                if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF)) {
                    if (adv < fontSize * 0.95) adv = fontSize * 1.0;
                }
                return adv;
            }
        }
#endif
        if (cp < 128) {
            if (cp == ' ' || cp == '\t') return fontSize * 0.28;
            if (cp == 'i' || cp == 'l' || cp == '.' || cp == ',' || cp == ':' || cp == ';') return fontSize * 0.28;
            if (cp == 'm' || cp == 'w' || cp == 'M' || cp == 'W' || cp == '@') return fontSize * 0.82;
            if (cp >= 'A' && cp <= 'Z') return fontSize * 0.65;
            return fontSize * 0.52;
        }
        return fontSize * 1.0;
    }

private:
#if HAVE_FREETYPE
    void scanSystemFonts() {
        std::vector<std::string> allFiles;
        std::vector<std::string> searchDirs;
        const char* envPath = std::getenv("DOC2PDF_FONT_PATH");
        if (envPath != nullptr && *envPath != '\0') {
            struct stat st;
            if (stat(envPath, &st) == 0) {
                if (S_ISDIR(st.st_mode)) searchDirs.push_back(envPath);
                else allFiles.push_back(envPath);
            }
        }
        searchDirs.push_back("/usr/share/fonts");
        searchDirs.push_back("/usr/local/share/fonts");

        for (const auto& d : searchDirs) {
            scanFontsInDir(d, allFiles);
        }

        auto indexFace = [&](const std::string& path, int faceIdx, FT_Face face) {
            std::shared_ptr<SystemFontEntry> entry = std::make_shared<SystemFontEntry>();
            entry->filePath = path;
            entry->faceIndex = faceIdx;
            if (face->family_name) {
                entry->familyName = face->family_name;
                entry->names.push_back(face->family_name);
            }

            FT_UInt sfntCount = FT_Get_Sfnt_Name_Count(face);
            for (FT_UInt s = 0; s < sfntCount; ++s) {
                FT_SfntName sfnt;
                if (FT_Get_Sfnt_Name(face, s, &sfnt) == 0) {
                    std::string decoded = decodeSfntName(sfnt);
                    if (!decoded.empty()) {
                        if (sfnt.name_id == 6 && entry->postscriptName.empty()) {
                            entry->postscriptName = decoded;
                        }
                        if (sfnt.name_id == 1 || sfnt.name_id == 4 || sfnt.name_id == 6 || sfnt.name_id == 16 || sfnt.name_id == 21) {
                            entry->names.push_back(decoded);
                        }
                    }
                }
            }

            // 文件名作为别名
            size_t slash = path.rfind('/');
            size_t dot = path.rfind('.');
            if (slash != std::string::npos && dot != std::string::npos && dot > slash) {
                std::string fname = path.substr(slash + 1, dot - slash - 1);
                entry->names.push_back(fname);
            }

            auto isRegularBetter = [](const std::shared_ptr<SystemFontEntry>& newE, const std::shared_ptr<SystemFontEntry>& oldE) -> bool {
                if (!oldE) return true;
                std::string nNorm = normalizeFontName(newE->postscriptName);
                std::string oNorm = normalizeFontName(oldE->postscriptName);
                bool newIsReg = (nNorm.find("regular") != std::string::npos || (nNorm.find("bold") == std::string::npos && nNorm.find("italic") == std::string::npos));
                bool oldIsReg = (oNorm.find("regular") != std::string::npos || (oNorm.find("bold") == std::string::npos && oNorm.find("italic") == std::string::npos));
                return newIsReg && !oldIsReg;
            };

            m_systemFontEntries.push_back(entry);
            for (const auto& n : entry->names) {
                std::string norm = normalizeFontName(n);
                auto it = m_systemFontIndex.find(norm);
                if (it == m_systemFontIndex.end() || isRegularBetter(entry, it->second)) {
                    m_systemFontIndex[norm] = entry;
                }
            }
        };

        for (const auto& path : allFiles) {
            FT_Face face0 = nullptr;
            if (FT_New_Face(m_ft, path.c_str(), 0, &face0) == 0) {
                FT_Long numFaces = face0->num_faces;
                indexFace(path, 0, face0);
                FT_Done_Face(face0);

                for (FT_Long fi = 1; fi < numFaces; ++fi) {
                    FT_Face subFace = nullptr;
                    if (FT_New_Face(m_ft, path.c_str(), static_cast<FT_Long>(fi), &subFace) == 0) {
                        indexFace(path, static_cast<int>(fi), subFace);
                        FT_Done_Face(subFace);
                    }
                }
            }
        }
    }

    std::shared_ptr<SystemFontEntry> findBestCjkFontEntry() const {
        std::shared_ptr<SystemFontEntry> best = nullptr;
        int maxScore = -999999;
        for (const auto& entry : m_systemFontEntries) {
            int score = 0;
            std::string ps = normalizeFontName(entry->postscriptName);
            std::string fp = normalizeFontName(entry->filePath);
            bool isSc = false, isJp = false, isKr = false, isTc = false;

            for (const auto& n : entry->names) {
                std::string nNorm = normalizeFontName(n);
                if (nNorm.find("sc") != std::string::npos || nNorm.find("cn") != std::string::npos ||
                    nNorm.find("hans") != std::string::npos || nNorm.find("simplified") != std::string::npos ||
                    nNorm.find("zenhei") != std::string::npos || nNorm.find("microhei") != std::string::npos ||
                    nNorm.find("simsun") != std::string::npos || nNorm.find("yahei") != std::string::npos ||
                    nNorm.find("dengxian") != std::string::npos || nNorm.find("kai") != std::string::npos ||
                    nNorm.find("fangsong") != std::string::npos || nNorm.find("song") != std::string::npos ||
                    nNorm.find("hei") != std::string::npos) {
                    isSc = true;
                }
                if (nNorm.find("jp") != std::string::npos || nNorm.find("japanese") != std::string::npos ||
                    nNorm.find("mincho") != std::string::npos) {
                    isJp = true;
                }
                if (nNorm.find("kr") != std::string::npos || nNorm.find("korean") != std::string::npos) {
                    isKr = true;
                }
                if (nNorm.find("tc") != std::string::npos || nNorm.find("hk") != std::string::npos ||
                    nNorm.find("tw") != std::string::npos || nNorm.find("hant") != std::string::npos) {
                    isTc = true;
                }
            }
            if (fp.find("zenhei") != std::string::npos || fp.find("microhei") != std::string::npos ||
                fp.find("cjk-regular") != std::string::npos || fp.find("notosanscjk") != std::string::npos ||
                fp.find("notoserifcjk") != std::string::npos) {
                score += 50;
            }
            if (isSc) score += 200;
            else if (isTc) score += 20;
            if (isJp) score -= 300;
            if (isKr) score -= 300;

            bool isRegular = (ps.find("regular") != std::string::npos || ps.find("normal") != std::string::npos ||
                              ps.find("book") != std::string::npos || ps.find("medium") != std::string::npos);
            bool isBold = (ps.find("bold") != std::string::npos || ps.find("black") != std::string::npos ||
                           ps.find("heavy") != std::string::npos || ps.find("extrabold") != std::string::npos);
            bool isThin = (ps.find("light") != std::string::npos || ps.find("thin") != std::string::npos ||
                           ps.find("extralight") != std::string::npos);
            if (isRegular) score += 50;
            if (isBold) score -= 50;
            if (isThin) score -= 30;

            // 对带有中文特征或候选字体，通过 FreeType 实测核心汉字覆盖情况
            if (m_ft && (isSc || score > 0)) {
                FT_Face testFace = nullptr;
                if (FT_New_Face(m_ft, entry->filePath.c_str(), entry->faceIndex, &testFace) == 0) {
                    // “中”(0x4E2D), “软”(0x8F6F), “险”(0x9669)
                    if (FT_Get_Char_Index(testFace, 0x4E2D) != 0) score += 100;
                    if (FT_Get_Char_Index(testFace, 0x8F6F) != 0) score += 200;
                    if (FT_Get_Char_Index(testFace, 0x9669) != 0) score += 100;
                    FT_Done_Face(testFace);
                }
            }

            if (score > maxScore) {
                maxScore = score;
                best = entry;
            }
        }
        return best;
    }

    std::shared_ptr<SystemFontEntry> findBestLatinFontEntry() const {
        std::shared_ptr<SystemFontEntry> best = nullptr;
        int maxScore = -999999;
        for (const auto& entry : m_systemFontEntries) {
            int score = 0;
            std::string ps = normalizeFontName(entry->postscriptName);
            for (const auto& n : entry->names) {
                std::string nNorm = normalizeFontName(n);
                if (nNorm.find("dejavu") != std::string::npos || nNorm.find("liberation") != std::string::npos ||
                    nNorm.find("arial") != std::string::npos || nNorm.find("notosans") != std::string::npos ||
                    nNorm.find("helvetica") != std::string::npos || nNorm.find("times") != std::string::npos) {
                    score += 100;
                }
            }
            bool isRegular = (ps.find("regular") != std::string::npos || ps.find("normal") != std::string::npos ||
                              ps.find("book") != std::string::npos);
            bool isBold = (ps.find("bold") != std::string::npos || ps.find("black") != std::string::npos);
            bool isItalic = (ps.find("italic") != std::string::npos || ps.find("oblique") != std::string::npos);
            if (isRegular) score += 50;
            if (isBold) score -= 50;
            if (isItalic) score -= 50;
            if (score > maxScore) {
                maxScore = score;
                best = entry;
            }
        }
        return best ? best : (!m_systemFontEntries.empty() ? m_systemFontEntries[0] : nullptr);
    }

    std::shared_ptr<SystemFontEntry> findEntryForFont(const std::string& name, const std::string& alt = "") const {
        std::string norm = normalizeFontName(name);
        auto it = m_systemFontIndex.find(norm);
        if (it != m_systemFontIndex.end()) return it->second;

        if (!alt.empty()) {
            std::string normAlt = normalizeFontName(alt);
            auto itAlt = m_systemFontIndex.find(normAlt);
            if (itAlt != m_systemFontIndex.end()) return itAlt->second;
        }

        // 1. 精确别名匹配
        for (const auto& entry : m_systemFontEntries) {
            for (const auto& n : entry->names) {
                if (normalizeFontName(n) == norm) return entry;
            }
        }
        if (!alt.empty()) {
            std::string normAlt = normalizeFontName(alt);
            for (const auto& entry : m_systemFontEntries) {
                for (const auto& n : entry->names) {
                    if (normalizeFontName(n) == normAlt) return entry;
                }
            }
        }

        // 2. 模糊包含匹配 (收集所有候选，优先选择 Regular 常规体，排除 Black/Bold 粗体)
        std::shared_ptr<SystemFontEntry> bestFuzzy = nullptr;
        int bestFuzzyScore = -99999;
        auto evaluateFuzzy = [&](const std::string& target) {
            for (const auto& entry : m_systemFontEntries) {
                for (const auto& n : entry->names) {
                    std::string nNorm = normalizeFontName(n);
                    if (nNorm.find(target) != std::string::npos || target.find(nNorm) != std::string::npos) {
                        int fScore = 0;
                        std::string ps = normalizeFontName(entry->postscriptName);
                        if (ps.find("regular") != std::string::npos || ps.find("normal") != std::string::npos || ps.find("book") != std::string::npos) fScore += 50;
                        if (ps.find("bold") != std::string::npos || ps.find("black") != std::string::npos || ps.find("heavy") != std::string::npos) fScore -= 50;
                        if (ps.find("light") != std::string::npos || ps.find("thin") != std::string::npos) fScore -= 30;
                        if (fScore > bestFuzzyScore) {
                            bestFuzzyScore = fScore;
                            bestFuzzy = entry;
                        }
                    }
                }
            }
        };
        evaluateFuzzy(norm);
        if (!bestFuzzy && !alt.empty()) {
            evaluateFuzzy(normalizeFontName(alt));
        }
        if (bestFuzzy) return bestFuzzy;

        // 3. 语义风格与语言区域（Locale）优先级智能匹配
        enum class FontKind { SansHei, SerifSong, Kai, FangSong, Mono, Other };
        auto detectKind = [](const std::string& text) -> FontKind {
            std::string s = normalizeFontName(text);
            if (s.find("mono") != std::string::npos || s.find("等宽") != std::string::npos || s.find("code") != std::string::npos || s.find("console") != std::string::npos) {
                return FontKind::Mono;
            }
            if (s.find("kai") != std::string::npos || s.find("楷") != std::string::npos) {
                return FontKind::Kai;
            }
            if (s.find("fangsong") != std::string::npos || s.find("仿宋") != std::string::npos) {
                return FontKind::FangSong;
            }
            if (s.find("song") != std::string::npos || s.find("宋") != std::string::npos || s.find("serif") != std::string::npos || s.find("mincho") != std::string::npos || s.find("ming") != std::string::npos || s.find("simsun") != std::string::npos || s.find("stsong") != std::string::npos) {
                return FontKind::SerifSong;
            }
            if (s.find("hei") != std::string::npos || s.find("黑") != std::string::npos || s.find("sans") != std::string::npos || s.find("gothic") != std::string::npos || s.find("xihei") != std::string::npos || s.find("yahei") != std::string::npos || s.find("dengxian") != std::string::npos || s.find("等线") != std::string::npos) {
                return FontKind::SansHei;
            }
            return FontKind::Other;
        };

        FontKind reqKind = detectKind(name);
        if (reqKind == FontKind::Other && !alt.empty()) {
            reqKind = detectKind(alt);
        }

        std::shared_ptr<SystemFontEntry> bestEntry = nullptr;
        int bestScore = -99999;

        for (const auto& entry : m_systemFontEntries) {
            int score = 0;
            bool isSc = false;
            bool isJp = false;
            bool isKr = false;
            bool isTc = false;
            FontKind eKind = FontKind::Other;

            for (const auto& n : entry->names) {
                std::string nNorm = normalizeFontName(n);
                if (eKind == FontKind::Other) eKind = detectKind(nNorm);
                if (nNorm.find("sc") != std::string::npos || nNorm.find("cn") != std::string::npos || nNorm.find("hans") != std::string::npos || nNorm.find("simplified") != std::string::npos || nNorm.find("zenhei") != std::string::npos || nNorm.find("microhei") != std::string::npos) {
                    isSc = true;
                }
                if (nNorm.find("jp") != std::string::npos || nNorm.find("japanese") != std::string::npos) {
                    isJp = true;
                }
                if (nNorm.find("kr") != std::string::npos || nNorm.find("korean") != std::string::npos) {
                    isKr = true;
                }
                if (nNorm.find("tc") != std::string::npos || nNorm.find("hk") != std::string::npos || nNorm.find("tw") != std::string::npos || nNorm.find("hant") != std::string::npos) {
                    isTc = true;
                }
            }

            if (reqKind != FontKind::Other && reqKind == eKind) {
                score += 100;
            }
            // 简体中文环境权重：SC 优先，日韩字体严禁作为中文兜底
            if (isSc) score += 50;
            else if (isTc) score += 10;
            if (isJp) score -= 80;
            if (isKr) score -= 80;

            // 字重选择：常规体（Regular/Normal/Book）优选，粗体/黑体/细体在正文中降权
            std::string psNorm = normalizeFontName(entry->postscriptName);
            bool isRegular = (psNorm.find("regular") != std::string::npos || psNorm.find("normal") != std::string::npos || psNorm.find("book") != std::string::npos);
            bool isBold = (psNorm.find("bold") != std::string::npos || psNorm.find("black") != std::string::npos || psNorm.find("heavy") != std::string::npos || psNorm.find("extrabold") != std::string::npos);
            bool isThin = (psNorm.find("light") != std::string::npos || psNorm.find("thin") != std::string::npos || psNorm.find("extralight") != std::string::npos);
            if (isRegular) score += 30;
            else if (!isBold && !isThin) score += 10;
            if (isBold) score -= 25;
            if (isThin) score -= 15;

            if (score > bestScore) {
                bestScore = score;
                bestEntry = entry;
            }
        }

        if (bestEntry && bestScore > 0) return bestEntry;
        if (reqKind != FontKind::Other) {
            auto bestCjk = findBestCjkFontEntry();
            if (bestCjk) return bestCjk;
        }
        auto bestLatin = findBestLatinFontEntry();
        if (bestLatin) return bestLatin;
        return !m_systemFontEntries.empty() ? m_systemFontEntries[0] : nullptr;
    }

    bool loadFontFile(const std::string& baseFont, const std::shared_ptr<SystemFontEntry>& entry) {
        if (!entry) return false;
        // 防止重复加载相同逻辑字体名称
        for (const auto& f : m_allFonts) {
            if (normalizeFontName(f->baseFont) == normalizeFontName(baseFont)) return true;
            for (const auto& al : f->aliases) {
                if (normalizeFontName(al) == normalizeFontName(baseFont)) return true;
            }
        }

        FT_Face face = nullptr;
        if (FT_New_Face(m_ft, entry->filePath.c_str(), entry->faceIndex, &face) != 0) return false;

        std::shared_ptr<PdfFontInfo> info = std::make_shared<PdfFontInfo>();
        info->baseFont = baseFont;
        info->postscriptName = !entry->postscriptName.empty() ? entry->postscriptName : sanitizePdfName(baseFont);
        info->filePath = entry->filePath;
        info->faceIndex = entry->faceIndex;
        info->face = face;

        if (face->units_per_EM > 0 && face->height > 0) {
            info->fontLineHeightMultiplier = static_cast<double>(face->height) / static_cast<double>(face->units_per_EM);
        } else {
            info->fontLineHeightMultiplier = 1.15;
        }

        // 检查物理文件是否已被其他逻辑字体加载过：若已加载过，直接复用压缩后的数据
        for (const auto& f : m_allFonts) {
            if (f->filePath == entry->filePath) {
                info->rawBytes = f->rawBytes;
                info->compBytes = f->compBytes;
                break;
            }
        }

        if (info->rawBytes.empty()) {
            std::ifstream f(entry->filePath, std::ios::binary | std::ios::ate);
            if (!f.is_open()) {
                FT_Done_Face(face);
                return false;
            }
            std::streamsize sz = f.tellg();
            if (sz <= 0) {
                FT_Done_Face(face);
                return false;
            }
            f.seekg(0, std::ios::beg);
            info->rawBytes.resize(static_cast<size_t>(sz));
            if (!f.read(reinterpret_cast<char*>(info->rawBytes.data()), sz)) {
                FT_Done_Face(face);
                return false;
            }

            uLongf destLen = compressBound(static_cast<uLong>(info->rawBytes.size()));
            info->compBytes.resize(destLen);
            if (compress(info->compBytes.data(), &destLen, info->rawBytes.data(), static_cast<uLong>(info->rawBytes.size())) != Z_OK) {
                FT_Done_Face(face);
                return false;
            }
            info->compBytes.resize(destLen);
        }

        // 登记别名集合 (纯动态从字体属性中提取)
        info->aliases.push_back(baseFont);
        for (const auto& n : entry->names) {
            info->aliases.push_back(n);
        }

        info->tag = "F" + std::to_string(m_allFonts.size() + 1);
        m_allFonts.push_back(info);

        if (m_defaultCjkFont == nullptr) {
            m_defaultCjkFont = info;
        }
        if (m_defaultLatinFont == nullptr) {
            m_defaultLatinFont = info;
        }
        return true;
    }

    FT_Library m_ft;
    std::vector<std::shared_ptr<SystemFontEntry>> m_systemFontEntries;
    std::map<std::string, std::shared_ptr<SystemFontEntry>> m_systemFontIndex;
    std::vector<std::shared_ptr<PdfFontInfo>> m_allFonts;
    std::shared_ptr<PdfFontInfo> m_defaultCjkFont;
    std::shared_ptr<PdfFontInfo> m_defaultLatinFont;
#endif
};



static std::vector<uint32_t> utf8ToCodepoints(const std::string& utf8) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        if (c < 0x80) {
            cps.push_back(c);
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < utf8.size()) {
                cps.push_back(((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F));
                i += 2;
            } else break;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < utf8.size()) {
                cps.push_back(((c & 0x0F) << 12) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(utf8[i + 2]) & 0x3F));
                i += 3;
            } else break;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < utf8.size()) {
                cps.push_back(((c & 0x07) << 18) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(utf8[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(utf8[i + 3]) & 0x3F));
                i += 4;
            } else break;
        } else {
            i += 1;
        }
    }
    return cps;
}

static void writeHexText(std::ostringstream& stream, const std::string& utf8) {
    std::vector<uint16_t> utf16 = utf8ToUtf16(utf8);
    stream << "<";
    for (uint16_t w : utf16) {
        stream << std::hex << std::setw(4) << std::setfill('0') << w;
    }
    stream << std::dec << ">";
}

struct TextAtom {
    std::string text;
    uint32_t firstCp = 0;
    uint32_t lastCp = 0;
    bool isWhitespace = false;
    bool isNewline = false;
    bool isTab = false;
    bool isClosingPunct = false;
    bool isOpeningPunct = false;
    double widthPt = 0.0;

    double fontSize = 11.0;
    std::string fontFamily;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool doubleUnderline = false;
    bool strikethrough = false;
    bool doubleStrikethrough = false;
    bool emphasisMark = false;
    ColorRGB color;
    ColorRGB bgColor;
    std::string linkTarget;
    double fontLineHeightMultiplier = 1.15;
};

static std::vector<TextAtom> tokenizeRun(const TextRun& run, TextMetricsEngine& metrics) {
    std::vector<TextAtom> atoms;
    double fontMultiplier = 1.15;
    auto fInfo = metrics.getFontForFamily(run.fontFamily);
    if (fInfo && fInfo->fontLineHeightMultiplier > 0.0) {
        fontMultiplier = fInfo->fontLineHeightMultiplier;
    }
    size_t i = 0;
    const std::string& s = run.text;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        size_t len = 1;
        uint32_t cp = c;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            if (i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            if (i + 2 < s.size()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            if (i + 3 < s.size()) cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        }
        if (i + len > s.size()) break;

        TextAtom atom;
        atom.fontSize = run.fontSize;
        atom.fontFamily = run.fontFamily;
        atom.bold = run.bold;
        atom.italic = run.italic;
        atom.underline = run.underline;
        atom.doubleUnderline = run.doubleUnderline;
        atom.strikethrough = run.strikethrough;
        atom.doubleStrikethrough = run.doubleStrikethrough;
        atom.emphasisMark = run.emphasisMark;
        atom.color = run.color;
        atom.bgColor = run.bgColor;
        atom.linkTarget = run.linkTarget;
        atom.fontLineHeightMultiplier = fontMultiplier;
        atom.firstCp = cp;
        atom.lastCp = cp;

        if (cp == '\n' || cp == '\r') {
            atom.text = "\n";
            atom.isNewline = true;
            atoms.push_back(atom);
            i += len;
            continue;
        }
        if (cp == '\t') {
            atom.text = "    ";
            atom.isTab = true;
            atom.widthPt = metrics.measureCharWidth(' ', run.fontSize, run.fontFamily) * 4.0;
            atoms.push_back(atom);
            i += len;
            continue;
        }
        if (cp == ' ') {
            atom.text = " ";
            atom.isWhitespace = true;
            atom.widthPt = metrics.measureCharWidth(' ', run.fontSize, run.fontFamily);
            atoms.push_back(atom);
            i += len;
            continue;
        }

        if (isClosingPunctuation(cp)) {
            atom.text = s.substr(i, len);
            atom.isClosingPunct = true;
            atom.widthPt = metrics.measureCharWidth(cp, run.fontSize, run.fontFamily);
            atoms.push_back(atom);
            i += len;
            continue;
        }
        if (isOpeningPunctuation(cp)) {
            atom.text = s.substr(i, len);
            atom.isOpeningPunct = true;
            atom.widthPt = metrics.measureCharWidth(cp, run.fontSize, run.fontFamily);
            atoms.push_back(atom);
            i += len;
            continue;
        }

        // CJK 汉字单独成原子 (可在此断行)
        if (cp >= 0x2E80) {
            atom.text = s.substr(i, len);
            atom.widthPt = metrics.measureCharWidth(cp, run.fontSize, run.fontFamily);
            atoms.push_back(atom);
            i += len;
            continue;
        }

        // 西文/数字/连续单词整体成原子 (禁止词中换行)
        size_t start = i;
        double wordWidth = 0.0;
        uint32_t lastWordCp = cp;
        while (i < s.size()) {
            uint8_t c2 = (uint8_t)s[i];
            size_t len2 = 1;
            uint32_t cp2 = c2;
            if ((c2 & 0xE0) == 0xC0) {
                len2 = 2;
                if (i + 1 < s.size()) cp2 = ((c2 & 0x1F) << 6) | (s[i + 1] & 0x3F);
            } else if ((c2 & 0xF0) == 0xE0) {
                len2 = 3;
                if (i + 2 < s.size()) cp2 = ((c2 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            } else if ((c2 & 0xF8) == 0xF0) {
                len2 = 4;
                if (i + 3 < s.size()) cp2 = ((c2 & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            }
            if (i + len2 > s.size()) break;

            if (cp2 == ' ' || cp2 == '\t' || cp2 == '\n' || cp2 == '\r' ||
                cp2 >= 0x2E80 || isClosingPunctuation(cp2) || isOpeningPunctuation(cp2)) {
                break;
            }
            wordWidth += metrics.measureCharWidth(cp2, run.fontSize, run.fontFamily);
            lastWordCp = cp2;
            i += len2;
        }
        atom.text = s.substr(start, i - start);
        atom.widthPt = wordWidth;
        atom.lastCp = lastWordCp;
        atoms.push_back(atom);
    }
    return atoms;
}

static inline bool isCjkAutoSpace(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

static inline bool isLatinOrDigitAutoSpace(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

static inline bool hasAutoSpaceBetween(uint32_t cp1, uint32_t cp2) {
    return (isCjkAutoSpace(cp1) && isLatinOrDigitAutoSpace(cp2)) || (isLatinOrDigitAutoSpace(cp1) && isCjkAutoSpace(cp2));
}

struct SpanLayout {
    std::string text;
    uint32_t firstCp = 0;
    uint32_t lastCp = 0;
    double autoSpaceBefore = 0.0;
    double widthPt = 0.0;
    double fontSize = 11.0;
    std::string fontFamily;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool doubleUnderline = false;
    bool strikethrough = false;
    bool doubleStrikethrough = false;
    bool emphasisMark = false;
    ColorRGB color;
    ColorRGB bgColor;
    std::string linkTarget;
};

struct LineLayout {
    std::vector<SpanLayout> spans;
    double totalWidthPt = 0.0;
    double maxFontSize = 11.0;
    double maxFontLineHeightMultiplier = 1.15;
    bool endsWithNewline = false;
    bool hasCjk = false;
};

static inline double calcDynamicLineHeight(const LineLayout& line, double lineSpacing, double linePitch) {
    double mult = (line.maxFontLineHeightMultiplier > 0.0) ? line.maxFontLineHeightMultiplier : 1.15;
    double fontH = line.maxFontSize * mult;
    if (linePitch > 0.0) {
        double baseGrid = (fontH <= linePitch * 1.25 || line.maxFontSize <= linePitch)
            ? linePitch
            : (std::ceil(fontH / linePitch) * linePitch);
        if (lineSpacing > 0.0 && lineSpacing != 1.0) {
            return baseGrid * lineSpacing;
        }
        return baseGrid;
    } else {
        return fontH * (lineSpacing > 0.0 ? lineSpacing : 1.0);
    }
}

static void consolidateAtomsIntoLine(const std::vector<TextAtom>& lineAtoms, LineLayout& outLine) {
    outLine.spans.clear();
    outLine.totalWidthPt = 0.0;
    outLine.maxFontSize = 11.0;
    outLine.maxFontLineHeightMultiplier = 1.15;
    outLine.hasCjk = false;

    for (size_t ai = 0; ai < lineAtoms.size(); ++ai) {
        const auto& a = lineAtoms[ai];
        if (a.fontSize > outLine.maxFontSize) outLine.maxFontSize = a.fontSize;
        if (a.fontLineHeightMultiplier > outLine.maxFontLineHeightMultiplier) {
            outLine.maxFontLineHeightMultiplier = a.fontLineHeightMultiplier;
        }

        double spaceBefore = 0.0;
        // 注意：DOCX 默认未显式声明 autoSpaceDE/autoSpaceDN 时不强行插入额外间隙
        // 避免中英数混排时人为撑大行宽导致提前断行
        outLine.totalWidthPt += (a.widthPt + spaceBefore);
        if (isCjkAutoSpace(a.firstCp)) {
            outLine.hasCjk = true;
        }

        if (spaceBefore == 0.0 && !outLine.spans.empty()) {
            auto& last = outLine.spans.back();
            if (std::abs(last.fontSize - a.fontSize) < 0.01 &&
                last.bold == a.bold &&
                last.italic == a.italic &&
                last.underline == a.underline &&
                last.doubleUnderline == a.doubleUnderline &&
                last.strikethrough == a.strikethrough &&
                last.doubleStrikethrough == a.doubleStrikethrough &&
                last.emphasisMark == a.emphasisMark &&
                last.color.r == a.color.r && last.color.g == a.color.g && last.color.b == a.color.b && last.color.isValid == a.color.isValid &&
                last.bgColor.r == a.bgColor.r && last.bgColor.g == a.bgColor.g && last.bgColor.b == a.bgColor.b && last.bgColor.isValid == a.bgColor.isValid &&
                last.linkTarget == a.linkTarget) {
                last.text += a.text;
                last.widthPt += a.widthPt;
                last.lastCp = a.lastCp;
                continue;
            }
        }

        SpanLayout sp;
        sp.text = a.text;
        sp.firstCp = a.firstCp;
        sp.lastCp = a.lastCp;
        sp.autoSpaceBefore = spaceBefore;
        sp.widthPt = a.widthPt;
        sp.fontSize = a.fontSize;
        sp.fontFamily = a.fontFamily;
        sp.bold = a.bold;
        sp.italic = a.italic;
        sp.underline = a.underline;
        sp.doubleUnderline = a.doubleUnderline;
        sp.strikethrough = a.strikethrough;
        sp.doubleStrikethrough = a.doubleStrikethrough;
        sp.emphasisMark = a.emphasisMark;
        sp.color = a.color;
        sp.bgColor = a.bgColor;
        sp.linkTarget = a.linkTarget;
        outLine.spans.push_back(sp);
    }
}

static std::vector<LineLayout> breakIntoLines(const Paragraph& para,
                                              double maxWidth,
                                              TextMetricsEngine& metrics) {
    std::vector<LineLayout> lines;
    if (para.runs.empty()) return lines;

    // 1. 全部 Run 转化为原子流
    std::vector<TextAtom> allAtoms;
    for (const auto& r : para.runs) {
        auto ats = tokenizeRun(r, metrics);
        allAtoms.insert(allAtoms.end(), ats.begin(), ats.end());
    }
    if (allAtoms.empty()) return lines;

    bool isFirstLine = true;
    double currentLineWidth = 0.0;
    std::vector<TextAtom> currentLineAtoms;

    auto flushLine = [&](bool newline) {
        if (!currentLineAtoms.empty()) {
            LineLayout l;
            l.endsWithNewline = newline;
            consolidateAtomsIntoLine(currentLineAtoms, l);
            lines.push_back(l);
            currentLineAtoms.clear();
            currentLineWidth = 0.0;
            isFirstLine = false;
        }
    };

    for (size_t idx = 0; idx < allAtoms.size(); ++idx) {
        const auto& atom = allAtoms[idx];

        if (atom.isNewline) {
            flushLine(true);
            continue;
        }

        double autoSpace = 0.0;
        // 未声明 autoSpace 时不插入额外间隙
        double limit = isFirstLine ? (maxWidth - para.firstLineIndent) : maxWidth;
        if (limit < 50.0) limit = maxWidth;

        if (currentLineWidth + atom.widthPt + autoSpace <= limit) {
            currentLineAtoms.push_back(atom);
            currentLineWidth += (atom.widthPt + autoSpace);
        } else {
            // 需要换行
            if (atom.isClosingPunct) {
                // 标点避头尾 (UAX #14 / Kinsoku Shori): 后置标点绝不能作为新行的开头
                if (!currentLineAtoms.empty() && !currentLineAtoms.back().isWhitespace && currentLineAtoms.size() > 1) {
                    TextAtom prevAtom = currentLineAtoms.back();
                    currentLineAtoms.pop_back();
                    flushLine(false);
                    currentLineAtoms.push_back(prevAtom);
                    currentLineWidth += prevAtom.widthPt;
                } else {
                    currentLineAtoms.push_back(atom);
                    currentLineWidth += (atom.widthPt + autoSpace);
                    flushLine(false);
                    continue;
                }
                currentLineAtoms.push_back(atom);
                currentLineWidth += atom.widthPt;
            } else if (atom.isWhitespace) {
                flushLine(false);
            } else {
                if (!currentLineAtoms.empty() && currentLineAtoms.back().isOpeningPunct && currentLineAtoms.size() > 1) {
                    TextAtom openPunct = currentLineAtoms.back();
                    currentLineAtoms.pop_back();
                    flushLine(false);
                    currentLineAtoms.push_back(openPunct);
                    currentLineWidth += openPunct.widthPt;
                } else {
                    flushLine(false);
                }
                currentLineAtoms.push_back(atom);
                currentLineWidth += atom.widthPt;
            }
        }
    }

    if (!currentLineAtoms.empty()) {
        flushLine(false);
    }

    return lines;
}

struct PdfAnnotRecord {
    size_t pageIndex = 0;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    std::string type; // "Link" or "FileAttachment"
    std::string linkTarget;
    size_t targetPageIndex = 0;
    double targetX = 0.0;
    double targetY = 0.0;
    std::string filename;
    size_t attachmentIndex = 0;
    size_t objId = 0;
    size_t filespecObjId = 0;
};

static void renderLineSpans(std::ostringstream& stream,
                            const LineLayout& line,
                            double startX,
                            double baselineY,
                            double extraSpacePerGap,
                            TextMetricsEngine& metrics,
                            FontGlyphRegistry* glyphRegistry = nullptr,
                            std::vector<PdfAnnotRecord>* annotCollector = nullptr,
                            size_t curPageIndex = 0,
                            std::map<std::string, std::tuple<size_t, double, double>>* namedTargets = nullptr) {
    (void)glyphRegistry;
    double curX = startX;
    for (const auto& span : line.spans) {
        if (span.text.empty()) continue;
        curX += span.autoSpaceBefore;

        // 收集超链接与批注跳转区域
        if (!span.linkTarget.empty() && annotCollector != nullptr) {
            PdfAnnotRecord ar;
            ar.pageIndex = curPageIndex;
            ar.x1 = curX;
            ar.y1 = baselineY - span.fontSize * 0.25;
            ar.x2 = curX + span.widthPt;
            ar.y2 = baselineY + span.fontSize * 0.90;
            ar.type = "Link";
            ar.linkTarget = span.linkTarget;
            annotCollector->push_back(ar);

            // 若本 span 是正文批注引用 (如 "comment_0")，设立反向锚点 "ref_0" 供批注清单回跳
            if (namedTargets != nullptr && span.linkTarget.rfind("comment_", 0) == 0) {
                std::string cId = span.linkTarget.substr(8);
                (*namedTargets)["ref_" + cId] = std::make_tuple(curPageIndex, curX, baselineY);
            }
        }

        // 背景高亮 (包含被批注文本的醒目浅黄底色)
        if (span.bgColor.isValid) {
            const auto& bg = span.bgColor;
            stream << (bg.r / 255.0) << " " << (bg.g / 255.0) << " " << (bg.b / 255.0) << " rg\n";
            stream << curX << " " << (baselineY - span.fontSize * 0.2) << " "
                   << span.widthPt << " " << (span.fontSize * 1.15) << " re f\n";
        }

        // 超链接角标虚线底线辅助
        if (!span.linkTarget.empty() && !span.underline && !span.doubleUnderline) {
            double uY = baselineY - span.fontSize * 0.12;
            stream << "0.5 w [2 2] 0 d 0 0.4 0.8 RG " << curX << " " << uY << " m "
                   << (curX + span.widthPt) << " " << uY << " l S [] 0 d\n";
        }

        // 文字颜色
        if (span.color.isValid) {
            const auto& c = span.color;
            stream << (c.r / 255.0) << " " << (c.g / 255.0) << " " << (c.b / 255.0) << " rg\n";
            stream << (c.r / 255.0) << " " << (c.g / 255.0) << " " << (c.b / 255.0) << " RG\n";
        } else {
            stream << "0 0 0 rg 0 0 0 RG\n";
        }

        // 粗体描边模拟 (Synthetic Bold)
        if (span.bold) {
            double strokeW = span.fontSize * 0.035;
            if (strokeW < 0.25) strokeW = 0.25;
            stream << "2 Tr " << strokeW << " w\n";
        } else {
            stream << "0 Tr\n";
        }

        // 文字矩阵 (斜体支持: Tm 剪切变换)
        stream << "BT\n";
        if (span.italic) {
            stream << "1 0 0.2126 1 " << curX << " " << baselineY << " Tm\n";
        } else {
            stream << curX << " " << baselineY << " Td\n";
        }

        if (metrics.hasCjk()) {
            std::vector<uint32_t> cps = utf8ToCodepoints(span.text);
            std::string currentTfTag;
            bool inHex = false;

            for (uint32_t cp : cps) {
                auto gf = metrics.getGlyphIndexAndFont(span.fontFamily, cp);
                uint32_t gid = gf.first;
                auto font = gf.second;
                std::string fontTag = (font != nullptr) ? font->tag : "F1";

                if (font != nullptr && gid != 0) {
                    font->registry.gidToUnicode[gid] = cp;
                    if (font->registry.gidWidths.find(gid) == font->registry.gidWidths.end()) {
                        font->registry.gidWidths[gid] = metrics.getGlyphWidth1000(font->face, cp, gid);
                    }
                }

                if (fontTag != currentTfTag) {
                    if (inHex) {
                        stream << "> Tj\n";
                        inHex = false;
                    }
                    stream << "/" << fontTag << " " << span.fontSize << " Tf\n";
                    currentTfTag = fontTag;
                }

                if (!inHex) {
                    stream << "<";
                    inHex = true;
                }
                char hexBuf[5];
                snprintf(hexBuf, sizeof(hexBuf), "%04X", gid);
                stream << hexBuf;
            }

            if (inHex) {
                stream << "> Tj\n";
                inHex = false;
            }
            stream << "ET\n";
        } else {
            stream << "/F1 " << span.fontSize << " Tf\n";
            writeHexText(stream, span.text);
            stream << " Tj\nET\n";
        }

        // 下划线
        if (span.doubleUnderline) {
            double uY1 = baselineY - span.fontSize * 0.10;
            double uY2 = baselineY - span.fontSize * 0.18;
            double uW = std::max(0.5, span.fontSize * 0.04);
            stream << uW << " w " << curX << " " << uY1 << " m "
                   << (curX + span.widthPt) << " " << uY1 << " l S\n";
            stream << uW << " w " << curX << " " << uY2 << " m "
                   << (curX + span.widthPt) << " " << uY2 << " l S\n";
        } else if (span.underline) {
            double uY = baselineY - span.fontSize * 0.12;
            double uW = std::max(0.6, span.fontSize * 0.05);
            stream << uW << " w " << curX << " " << uY << " m "
                   << (curX + span.widthPt) << " " << uY << " l S\n";
        }

        // 删除线
        if (span.doubleStrikethrough) {
            double strikeY1 = baselineY + span.fontSize * 0.22;
            double strikeY2 = baselineY + span.fontSize * 0.42;
            stream << "0.6 w " << curX << " " << strikeY1 << " m "
                   << (curX + span.widthPt) << " " << strikeY1 << " l S\n";
            stream << "0.6 w " << curX << " " << strikeY2 << " m "
                   << (curX + span.widthPt) << " " << strikeY2 << " l S\n";
        } else if (span.strikethrough) {
            double strikeY = baselineY + span.fontSize * 0.32;
            double strikeW = span.fontSize * 0.055;
            if (strikeW < 0.6) strikeW = 0.6;
            stream << strikeW << " w " << curX << " " << strikeY << " m "
                   << (curX + span.widthPt) << " " << strikeY << " l S\n";
        }

        // 重点号 (Emphasis mark - dot below baseline)
        if (span.emphasisMark) {
            double dotR = std::max(0.7, span.fontSize * 0.08);
            double dotY = baselineY - span.fontSize * 0.22;
            size_t sIdx = 0;
            double charX = curX;
            while (sIdx < span.text.size()) {
                uint8_t c = (uint8_t)span.text[sIdx];
                size_t cLen = 1;
                uint32_t cp = c;
                if ((c & 0xE0) == 0xC0) {
                    cLen = 2;
                    if (sIdx + 1 < span.text.size()) cp = ((c & 0x1F) << 6) | (span.text[sIdx + 1] & 0x3F);
                } else if ((c & 0xF0) == 0xE0) {
                    cLen = 3;
                    if (sIdx + 2 < span.text.size()) cp = ((c & 0x0F) << 12) | ((span.text[sIdx + 1] & 0x3F) << 6) | (span.text[sIdx + 2] & 0x3F);
                } else if ((c & 0xF8) == 0xF0) {
                    cLen = 4;
                    if (sIdx + 3 < span.text.size()) cp = ((c & 0x07) << 18) | ((span.text[sIdx + 1] & 0x3F) << 12) | ((span.text[sIdx + 2] & 0x3F) << 6) | (span.text[sIdx + 3] & 0x3F);
                }
                if (sIdx + cLen > span.text.size()) break;

                double charW = metrics.measureCharWidth(cp, span.fontSize, span.fontFamily);
                double centerX = charX + charW / 2.0;

                // 绘制实心圆形标记 (Bezier approximation: c x1 y1 x2 y2 x3 y3)
                double kappa = dotR * 0.5522847498;
                stream << (centerX + dotR) << " " << dotY << " m "
                       << (centerX + dotR) << " " << (dotY + kappa) << " "
                       << (centerX + kappa) << " " << (dotY + dotR) << " "
                       << centerX << " " << (dotY + dotR) << " c "
                       << (centerX - kappa) << " " << (dotY + dotR) << " "
                       << (centerX - dotR) << " " << (dotY + kappa) << " "
                       << (centerX - dotR) << " " << dotY << " c "
                       << (centerX - dotR) << " " << (dotY - kappa) << " "
                       << (centerX - kappa) << " " << (dotY - dotR) << " "
                       << centerX << " " << (dotY - dotR) << " c "
                       << (centerX + kappa) << " " << (dotY - dotR) << " "
                       << (centerX + dotR) << " " << (dotY - kappa) << " "
                       << (centerX + dotR) << " " << dotY << " c f\n";

                charX += charW;
                sIdx += cLen;
            }
        }

        curX += span.widthPt;
        if (extraSpacePerGap > 0 && !span.text.empty() && span.text.back() == ' ') {
            curX += extraSpacePerGap;
        }
    }
}

static void renderHeaderFooter(const std::vector<Paragraph>& paras,
                               std::ostringstream& stream,
                               double& curY,
                               double contentWidth,
                               double marginX,
                               size_t pageNum,
                               size_t totalPages,
                               TextMetricsEngine& metrics,
                               FontGlyphRegistry* glyphRegistry,
                               double linePitch = 0.0) {
    if (paras.empty()) return;
    double inlineGroupSavedY = curY;
    double maxGroupDrop = 0.0;
    for (size_t i = 0; i < paras.size(); ++i) {
        const auto& p = paras[i];
        if (p.sameLineAsPrev) {
            curY = inlineGroupSavedY;
        } else {
            inlineGroupSavedY = curY;
            maxGroupDrop = 0.0;
        }
        double beforeY = curY;
        Paragraph resolvedP = p;
        if (resolvedP.runs.empty()) {
            double baseEmpty = (resolvedP.emptyLineHeight > 0.0) ? resolvedP.emptyLineHeight : 10.5;
            if (linePitch > 0.0 && baseEmpty < linePitch) baseEmpty = linePitch;
            else if (resolvedP.lineSpacing > 0.0) baseEmpty *= resolvedP.lineSpacing;
            double emptyH = baseEmpty + resolvedP.spaceAfter;
            curY -= emptyH;
            double thisDrop = beforeY - curY;
            if (thisDrop > maxGroupDrop) maxGroupDrop = thisDrop;
            curY = inlineGroupSavedY - maxGroupDrop;
            continue;
        }
        bool hasTab = false;
        for (auto& r : resolvedP.runs) {
            auto replacePlaceholder = [&](const std::string& token, const std::string& val) {
                size_t pos = 0;
                while ((pos = r.text.find(token, pos)) != std::string::npos) {
                    r.text.replace(pos, token.size(), val);
                    pos += val.size();
                }
            };
            replacePlaceholder("PAGE_NUM", std::to_string(pageNum));
            replacePlaceholder("[PAGE]", std::to_string(pageNum));
            replacePlaceholder("NUM_PAGES", std::to_string(totalPages));
            replacePlaceholder("[NUMPAGES]", std::to_string(totalPages));
            replacePlaceholder("[NUM_PAGES]", std::to_string(totalPages));
            if (r.text.find('\t') != std::string::npos) hasTab = true;
        }

        if (hasTab) {
            // Word 页眉/页脚经典三栏制表符模型：[左栏] <tab> [居中栏] <tab> [右栏]
            std::vector<Paragraph> zones(3);
            size_t currentZone = 0;
            for (const auto& r : resolvedP.runs) {
                size_t start = 0;
                while (start < r.text.size()) {
                    size_t tabPos = r.text.find('\t', start);
                    if (tabPos == std::string::npos) {
                        TextRun subR = r;
                        subR.text = r.text.substr(start);
                        if (!subR.text.empty() && currentZone < 3) zones[currentZone].runs.push_back(subR);
                        break;
                    } else {
                        if (tabPos > start) {
                            TextRun subR = r;
                            subR.text = r.text.substr(start, tabPos - start);
                            if (currentZone < 3) zones[currentZone].runs.push_back(subR);
                        }
                        if (currentZone < 2) currentZone++;
                        start = tabPos + 1;
                    }
                }
            }

            double maxLineHeight = 0.0;
            // 渲染左栏 (Left Zone)
            if (!zones[0].runs.empty()) {
                auto lns = breakIntoLines(zones[0], contentWidth, metrics);
                for (const auto& line : lns) {
                    double baseline = curY - line.maxFontSize * 0.82;
                    double startX = marginX + resolvedP.leftIndent + resolvedP.firstLineIndent;
                    renderLineSpans(stream, line, startX, baseline, 0.0, metrics, glyphRegistry);
                    double lh = calcDynamicLineHeight(line, resolvedP.lineSpacing, linePitch);
                    if (lh > maxLineHeight) maxLineHeight = lh;
                }
            }
            // 渲染中栏 (Center Zone)
            if (!zones[1].runs.empty()) {
                auto lns = breakIntoLines(zones[1], contentWidth, metrics);
                for (const auto& line : lns) {
                    double baseline = curY - line.maxFontSize * 0.82;
                    double startX = marginX + (contentWidth - line.totalWidthPt) / 2.0;
                    renderLineSpans(stream, line, startX, baseline, 0.0, metrics, glyphRegistry);
                    double lh = calcDynamicLineHeight(line, resolvedP.lineSpacing, linePitch);
                    if (lh > maxLineHeight) maxLineHeight = lh;
                }
            }
            // 渲染右栏 (Right Zone)
            if (!zones[2].runs.empty()) {
                auto lns = breakIntoLines(zones[2], contentWidth, metrics);
                for (const auto& line : lns) {
                    double baseline = curY - line.maxFontSize * 0.82;
                    double startX = marginX + (contentWidth - line.totalWidthPt);
                    renderLineSpans(stream, line, startX, baseline, 0.0, metrics, glyphRegistry);
                    double lh = calcDynamicLineHeight(line, resolvedP.lineSpacing, linePitch);
                    if (lh > maxLineHeight) maxLineHeight = lh;
                }
            }
            if (linePitch > 0.0 && maxLineHeight < linePitch) maxLineHeight = linePitch;
            else if (maxLineHeight == 0.0) maxLineHeight = 11.0 * resolvedP.lineSpacing;
            curY -= maxLineHeight;
        } else {
            auto lines = breakIntoLines(resolvedP, contentWidth, metrics);
            for (size_t li = 0; li < lines.size(); ++li) {
                const auto& line = lines[li];
                double startX = marginX + resolvedP.leftIndent;
                if (li == 0) startX += resolvedP.firstLineIndent;
                if (resolvedP.align == Alignment::Center) {
                    startX = marginX + (contentWidth - line.totalWidthPt) / 2.0;
                } else if (resolvedP.align == Alignment::Right) {
                    startX = marginX + (contentWidth - line.totalWidthPt);
                }
                double baseline = curY - line.maxFontSize * 0.82;
                renderLineSpans(stream, line, startX, baseline, 0.0, metrics, glyphRegistry);
                double lh = calcDynamicLineHeight(line, resolvedP.lineSpacing, linePitch);
                curY -= lh;
            }
        }
        curY -= resolvedP.spaceAfter;

        double thisDrop = beforeY - curY;
        if (thisDrop > maxGroupDrop) maxGroupDrop = thisDrop;
        curY = inlineGroupSavedY - maxGroupDrop;
    }
}

static void renderParagraph(const Paragraph& para,
                            std::ostringstream& stream,
                            double& currentY,
                            const std::function<void()>& startNewPage,
                            double contentWidth,
                            double marginX,
                            double marginBottom,
                            TextMetricsEngine& metrics,
                            FontGlyphRegistry* glyphRegistry = nullptr,
                            double linePitch = 0.0,
                            std::vector<PdfAnnotRecord>* annotCollector = nullptr,
                            size_t curPageIndex = 0,
                            std::map<std::string, std::tuple<size_t, double, double>>* namedTargets = nullptr) {
    if (para.pageBreakBefore) {
        startNewPage();
    }

    double availWidth = contentWidth - para.leftIndent - para.rightIndent;
    if (availWidth <= 50.0) availWidth = contentWidth;

    std::vector<LineLayout> lines = breakIntoLines(para, availWidth, metrics);

    // 空行与连续空白处理：根据字号与行距保留名义行高，杜绝空行塌陷
    if (para.runs.empty() || lines.empty()) {
        double maxSz = 0.0;
        for (const auto& r : para.runs) {
            if (r.fontSize > maxSz) maxSz = r.fontSize;
        }
        if (maxSz <= 0.0) maxSz = (para.emptyLineHeight > 0.0 ? para.emptyLineHeight : 10.5);
        if (linePitch > 0.0 && maxSz < linePitch) maxSz = linePitch;
        double ls = (para.lineSpacing > 0.0) ? para.lineSpacing : 1.0;
        double lineHeight = maxSz * ls;
        if (currentY - lineHeight < marginBottom) {
            startNewPage();
        } else {
            currentY -= lineHeight;
        }
        return;
    }

    bool isFirst = true;
    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const auto& line = lines[lineIdx];
        double lineHeight = calcDynamicLineHeight(line, para.lineSpacing, linePitch);

        // 孤行控制 (Widow/Orphan control) 与行高空间检查
        if (lineIdx == 0) {
            if (lines.size() >= 2) {
                double nextLineInk = lines[1].maxFontSize;
                if (currentY - lineHeight - nextLineInk < marginBottom) {
                    startNewPage();
                }
            } else {
                if (currentY - lineHeight < marginBottom) {
                    startNewPage();
                }
            }
        } else {
            // 段落内部续行
            if (currentY - lineHeight < marginBottom) {
                startNewPage();
            }
        }

        double startX = marginX + para.leftIndent;
        if (isFirst) startX += para.firstLineIndent;

        double lineAvail = isFirst ? (availWidth - para.firstLineIndent) : availWidth;
        double extraSpace = 0.0;

        if (para.align == Alignment::Center) {
            startX = marginX + para.leftIndent + (lineAvail - line.totalWidthPt) / 2.0;
        } else if (para.align == Alignment::Right) {
            startX = marginX + para.leftIndent + (lineAvail - line.totalWidthPt);
        } else if (para.align == Alignment::Justify && !line.endsWithNewline && lineIdx + 1 < lines.size()) {
            double slack = lineAvail - line.totalWidthPt;
            if (slack > 0 && slack < lineAvail * 0.25) {
                size_t numSpaces = 0;
                for (const auto& sp : line.spans) {
                    for (char ch : sp.text) if (ch == ' ') numSpaces++;
                }
                if (numSpaces > 0) {
                    extraSpace = slack / numSpaces;
                }
            }
        }

        double maxAscender = line.maxFontSize * 0.82;
        double textBaselineY = currentY - maxAscender;

        renderLineSpans(stream, line, startX, textBaselineY, extraSpace, metrics, glyphRegistry, annotCollector, curPageIndex, namedTargets);

        currentY -= lineHeight;
        isFirst = false;
    }
}

static void renderTable(const Table& table,
                        std::ostringstream& stream,
                        double& currentY,
                        const std::function<void()>& startNewPage,
                        double contentWidth,
                        double marginX,
                        double marginBottom,
                        TextMetricsEngine& metrics,
                        FontGlyphRegistry* glyphRegistry,
                        std::vector<PdfAnnotRecord>* annotCollector = nullptr,
                        size_t curPageIndex = 0,
                        std::map<std::string, std::tuple<size_t, double, double>>* namedTargets = nullptr) {
    if (table.rows.empty()) return;

    size_t maxCols = 0;
    for (const auto& r : table.rows) {
        if (r.cells.size() > maxCols) maxCols = r.cells.size();
    }
    if (maxCols == 0) return;

    std::vector<double> colWidths(maxCols, contentWidth / maxCols);
    if (table.gridColWidthsPt.size() >= maxCols) {
        double sumDxa = 0;
        for (size_t c = 0; c < maxCols; ++c) sumDxa += table.gridColWidthsPt[c];
        if (sumDxa > 0) {
            for (size_t c = 0; c < maxCols; ++c) {
                colWidths[c] = (table.gridColWidthsPt[c] / sumDxa) * contentWidth;
            }
        }
    }

    for (const auto& row : table.rows) {
        std::vector<std::vector<LineLayout>> cellLines(row.cells.size());
        double maxCellHeight = 0.0;

        for (size_t c = 0; c < row.cells.size(); ++c) {
            double cellW = (c < colWidths.size()) ? colWidths[c] : (contentWidth / maxCols);
            double padH = row.cells[c].padLeft + row.cells[c].padRight;
            double cellContentHeight = row.cells[c].padTop + row.cells[c].padBottom;

            for (const auto& p : row.cells[c].paragraphs) {
                auto lns = breakIntoLines(p, cellW - padH, metrics);
                for (const auto& l : lns) {
                    double mult = 1.15;
                    cellContentHeight += (l.maxFontSize * mult * (p.lineSpacing > 0.0 ? p.lineSpacing : 1.0));
                }
                cellLines[c].insert(cellLines[c].end(), lns.begin(), lns.end());
            }
            if (cellContentHeight > maxCellHeight) {
                maxCellHeight = cellContentHeight;
            }
        }
        if (maxCellHeight < 15.0) {
            maxCellHeight = 15.0;
        }

        if (currentY - maxCellHeight < marginBottom) {
            startNewPage();
        }

        double currentX = marginX + table.tableIndent;
        for (size_t c = 0; c < row.cells.size(); ++c) {
            double w = (c < colWidths.size()) ? colWidths[c] : (contentWidth / maxCols);
            double y = currentY - maxCellHeight;

            // 背景底色填充 (仅在明确指定底色时填充)
            if (row.cells[c].bgColor.isValid) {
                const auto& bg = row.cells[c].bgColor;
                stream << (bg.r / 255.0) << " " << (bg.g / 255.0) << " " << (bg.b / 255.0) << " rg\n";
                stream << currentX << " " << y << " " << w << " " << maxCellHeight << " re f\n";
            }

            // 边框绘制 (细黑实线)
            stream << "0.5 w 0 0 0 RG\n";
            stream << currentX << " " << y << " " << w << " " << maxCellHeight << " re S\n";

            // 绘制单元格文本
            double cellTextY = currentY - row.cells[c].padTop;
            for (const auto& line : cellLines[c]) {
                double mult = 1.15;
                double lineHeight = line.maxFontSize * mult;
                double maxAscender = line.maxFontSize * 0.82;
                double baselineY = cellTextY - maxAscender;

                renderLineSpans(stream, line, currentX + row.cells[c].padLeft, baselineY, 0.0, metrics, glyphRegistry, annotCollector, curPageIndex, namedTargets);

                cellTextY -= lineHeight;
            }

            currentX += w;
        }

        currentY -= maxCellHeight;
    }
}

static const char g_toUnicodeCMap[] =
    "/CIDInit /ProcSet findresource begin\n"
    "12 dict begin\n"
    "begincmap\n"
    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
    "/CMapName /Custom-ToUnicode def\n"
    "/CMapType 2 def\n"
    "1 begincodespacerange\n"
    "<0000> <FFFF>\n"
    "endcodespacerange\n"
    "1 beginbfrange\n"
    "<0000> <FFFF> <0000>\n"
    "endbfrange\n"
    "endcmap\n"
    "CMapName currentdict /CMap defineresource pop\n"
    "end\n"
    "end";

static bool loadEmbeddedFontData(const std::string& fontPath, std::vector<uint8_t>& fontBytes) {
    std::vector<std::string> candidates;
    if (!fontPath.empty()) {
        candidates.push_back(fontPath);
    }
    const char* envPath = std::getenv("DOC2PDF_FONT_PATH");
    if (envPath != nullptr && *envPath != '\0') {
        candidates.push_back(envPath);
    }
#if defined(_WIN32)
    candidates.push_back("C:\\Windows\\Fonts\\msyh.ttc");
    candidates.push_back("C:\\Windows\\Fonts\\simsun.ttc");
    candidates.push_back("C:\\Windows\\Fonts\\simsun.ttf");
#elif defined(__APPLE__)
    candidates.push_back("/Library/Fonts/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/PingFang.ttc");
    candidates.push_back("/System/Library/Fonts/STHeiti Light.ttc");
#else
    candidates.push_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
    candidates.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
    candidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    candidates.push_back("/usr/share/fonts/truetype/arphic/uming.ttc");
    candidates.push_back("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf");
#endif

    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            std::streamsize size = file.tellg();
            if (size > 0) {
                file.seekg(0, std::ios::beg);
                fontBytes.resize(static_cast<size_t>(size));
                if (file.read(reinterpret_cast<char*>(fontBytes.data()), size)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static std::string escapePdfString(const std::string& s) {
    std::string res;
    res.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            res.push_back('\\');
        }
        res.push_back(c);
    }
    return res;
}

struct AttachmentObjInfo {
    size_t filespecId = 0;
    size_t efId = 0;
    std::vector<uint8_t> compressedData;
};

static void writePdfStructure(std::ofstream& out,
                              const std::vector<std::string>& pages,
                              const std::vector<ImageBlock>& docImages,
                              double w, double h,
                              std::vector<std::shared_ptr<PdfFontInfo>>& fonts,
                              std::vector<std::vector<PdfAnnotRecord>>& pageAnnots,
                              const std::vector<AttachmentItem>& attachments) {
    std::vector<size_t> xref;
    xref.push_back(0);

    auto writeObj = [&out, &xref](const std::string& content) -> size_t {
        xref.push_back(static_cast<size_t>(out.tellp()));
        size_t id = xref.size() - 1;
        out << id << " 0 obj\n" << content << "\nendobj\n";
        return id;
    };

    size_t numPages = pages.size();

    // 预先为字体生成 ToUnicode CMap 并压缩
    for (auto& font : fonts) {
        if (!font) continue;
        if (font->registry.gidToUnicode.empty()) {
            font->registry.gidToUnicode[1] = 32;
            font->registry.gidWidths[1] = 500;
        }

        std::vector<std::string> blocks;
        std::string curBlock;
        size_t countInBlock = 0;

        for (const auto& kv : font->registry.gidToUnicode) {
            char buf[64];
            snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", kv.first, kv.second);
            curBlock += buf;
            countInBlock++;

            if (countInBlock == 100) {
                blocks.push_back(std::to_string(countInBlock) + " beginbfchar\n" + curBlock + "endbfchar\n");
                curBlock.clear();
                countInBlock = 0;
            }
        }
        if (countInBlock > 0) {
            blocks.push_back(std::to_string(countInBlock) + " beginbfchar\n" + curBlock + "endbfchar\n");
        }

        std::string allBlocks;
        for (const auto& b : blocks) allBlocks += b;

        std::string toUnicodeCMap =
            "/CIDInit /ProcSet findresource begin\n"
            "12 dict begin\n"
            "begincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Custom-ToUnicode def\n"
            "/CMapType 2 def\n"
            "1 begincodespacerange\n"
            "<0000> <FFFF>\n"
            "endcodespacerange\n"
            + allBlocks +
            "endcmap\n"
            "CMapName currentdict /CMap defineresource pop\n"
            "end\n"
            "end";

        size_t cmapLen = toUnicodeCMap.size();
        uLongf destCMapLen = compressBound(static_cast<uLong>(cmapLen));
        font->compCMapBytes.resize(destCMapLen);
        if (compress(font->compCMapBytes.data(), &destCMapLen, reinterpret_cast<const Bytef*>(toUnicodeCMap.data()), static_cast<uLong>(cmapLen)) == Z_OK) {
            font->compCMapBytes.resize(destCMapLen);
        } else {
            font->compCMapBytes.clear();
        }
    }

    // 确定性规划所有对象的 ID
    size_t nextObjId = 3 + numPages * 2;
    if (!fonts.empty()) {
        for (auto& font : fonts) {
            if (!font) continue;
            font->fontObjId = nextObjId++;
            font->cidObjId = nextObjId++;
            font->descObjId = nextObjId++;
            font->fontFileObjId = nextObjId++;
            font->toUnicodeObjId = nextObjId++;
        }
    } else {
        nextObjId += 2; // STSong-Light
    }

    size_t firstImageId = nextObjId;
    nextObjId += docImages.size();

    // 为附件分配 ID 并完成 zlib 压缩
    std::vector<AttachmentObjInfo> attObjs(attachments.size());
    for (size_t k = 0; k < attachments.size(); ++k) {
        const auto& att = attachments[k];
        attObjs[k].efId = nextObjId++;
        attObjs[k].filespecId = nextObjId++;

        if (!att.data.empty()) {
            uLongf destLen = compressBound(static_cast<uLong>(att.data.size()));
            attObjs[k].compressedData.resize(destLen);
            if (compress(attObjs[k].compressedData.data(), &destLen, reinterpret_cast<const Bytef*>(att.data.data()), static_cast<uLong>(att.data.size())) == Z_OK) {
                attObjs[k].compressedData.resize(destLen);
            } else {
                attObjs[k].compressedData = att.data;
            }
        }
    }

    // 为页面注记分配 ID
    for (size_t p = 0; p < numPages; ++p) {
        if (p >= pageAnnots.size()) continue;
        for (auto& ar : pageAnnots[p]) {
            ar.objId = nextObjId++;
            if (ar.type == "FileAttachment" && ar.attachmentIndex < attObjs.size()) {
                ar.filespecObjId = attObjs[ar.attachmentIndex].filespecId;
            }
        }
    }

    out << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    // 1 0 obj: Catalog (若有附件，嵌入 /Names << /EmbeddedFiles ... >>)
    std::string namesDict;
    if (!attachments.empty()) {
        std::string namesEntries;
        for (size_t k = 0; k < attachments.size(); ++k) {
            std::string safeFn = escapePdfString(attachments[k].filename);
            namesEntries += "(" + safeFn + ") " + std::to_string(attObjs[k].filespecId) + " 0 R ";
        }
        namesDict = "\n   /Names << /EmbeddedFiles << /Names [ " + namesEntries + "] >> >>";
    }
    writeObj("<< /Type /Catalog /Pages 2 0 R" + namesDict + " >>");

    // 2 0 obj: Pages
    std::string kids = "[ ";
    for (size_t i = 0; i < numPages; ++i) {
        kids += std::to_string(3 + i * 2) + " 0 R ";
    }
    kids += "]";
    writeObj("<< /Type /Pages /Kids " + kids + " /Count " + std::to_string(numPages) + " >>");

    std::string xobjDict;
    if (!docImages.empty()) {
        xobjDict = " /XObject <<";
        for (size_t m = 0; m < docImages.size(); ++m) {
            xobjDict += " /Im" + std::to_string(m + 1) + " " + std::to_string(firstImageId + m) + " 0 R";
        }
        xobjDict += " >>";
    }

    std::string fontResDict = " /Font <<";
    for (const auto& font : fonts) {
        if (!font) continue;
        fontResDict += " /" + font->tag + " " + std::to_string(font->fontObjId) + " 0 R";
    }
    fontResDict += " >>";

    // 页面对象与内容流
    for (size_t i = 0; i < numPages; ++i) {
        size_t contentId = 3 + i * 2 + 1;
        std::string annotsArrayStr;
        if (i < pageAnnots.size() && !pageAnnots[i].empty()) {
            annotsArrayStr = "   /Annots [ ";
            for (const auto& ar : pageAnnots[i]) {
                annotsArrayStr += std::to_string(ar.objId) + " 0 R ";
            }
            annotsArrayStr += "]\n";
        }

        std::string pageDict = "<< /Type /Page /Parent 2 0 R\n"
                               "   /MediaBox [ 0 0 " + std::to_string(w) + " " + std::to_string(h) + " ]\n"
                               "   /Contents " + std::to_string(contentId) + " 0 R\n"
                               "   /Resources <<" + fontResDict + xobjDict + " >>\n"
                               + annotsArrayStr +
                               ">>";
        writeObj(pageDict);

        const std::string& s = pages[i];
        std::string contentObj = "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream";
        writeObj(contentObj);
    }

    // 写入字体对象
    if (!fonts.empty()) {
        for (const auto& font : fonts) {
            if (!font) continue;
            std::string safeName = !font->postscriptName.empty() ? sanitizePdfName(font->postscriptName) : sanitizePdfName(font->baseFont);
            std::string fontDict = "<< /Type /Font\n"
                                   "   /Subtype /Type0\n"
                                   "   /BaseFont /" + safeName + "\n"
                                   "   /Encoding /Identity-H\n"
                                   "   /DescendantFonts [ " + std::to_string(font->cidObjId) + " 0 R ]\n"
                                   "   /ToUnicode " + std::to_string(font->toUnicodeObjId) + " 0 R\n"
                                   ">>";
            writeObj(fontDict);

            std::string wArrayStr;
            for (const auto& kv : font->registry.gidWidths) {
                wArrayStr += std::to_string(kv.first) + " [ " + std::to_string(kv.second) + " ] ";
            }

            std::string cidDict = "<< /Type /Font\n"
                                  "   /Subtype /CIDFontType2\n"
                                  "   /BaseFont /" + safeName + "\n"
                                  "   /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>\n"
                                  "   /FontDescriptor " + std::to_string(font->descObjId) + " 0 R\n"
                                  "   /CIDToGIDMap /Identity\n"
                                  "   /DW 1000\n"
                                  "   /W [ " + wArrayStr + "]\n"
                                  ">>";
            writeObj(cidDict);

            std::string descDict = "<< /Type /FontDescriptor\n"
                                   "   /FontName /" + safeName + "\n"
                                   "   /Flags 4\n"
                                   "   /FontBBox [ -1000 -500 1000 1000 ]\n"
                                   "   /ItalicAngle 0\n"
                                   "   /Ascent 880\n"
                                   "   /Descent -120\n"
                                   "   /CapHeight 700\n"
                                   "   /StemV 80\n"
                                   "   /FontFile2 " + std::to_string(font->fontFileObjId) + " 0 R\n"
                                   ">>";
            writeObj(descDict);

            xref.push_back(static_cast<size_t>(out.tellp()));
            size_t ffId = xref.size() - 1;
            (void)ffId;
            out << font->fontFileObjId << " 0 obj\n"
                << "<< /Length " << font->compBytes.size()
                << " /Length1 " << font->rawBytes.size()
                << " /Filter /FlateDecode >>\nstream\n";
            out.write(reinterpret_cast<const char*>(font->compBytes.data()), font->compBytes.size());
            out << "\nendstream\nendobj\n";

            xref.push_back(static_cast<size_t>(out.tellp()));
            size_t tuId = xref.size() - 1;
            (void)tuId;
            out << font->toUnicodeObjId << " 0 obj\n"
                << "<< /Length " << font->compCMapBytes.size()
                << " /Filter /FlateDecode >>\nstream\n";
            out.write(reinterpret_cast<const char*>(font->compCMapBytes.data()), font->compCMapBytes.size());
            out << "\nendstream\nendobj\n";
        }
    } else {
        size_t fontObjId = 3 + numPages * 2;
        size_t cidObjId = fontObjId + 1;
        writeObj("<< /Type /Font\n"
                 "   /Subtype /Type0\n"
                 "   /BaseFont /STSong-Light\n"
                 "   /Encoding /UniGB-UCS2-H\n"
                 "   /DescendantFonts [ " + std::to_string(cidObjId) + " 0 R ]\n"
                 ">>");

        writeObj("<< /Type /Font\n"
                 "   /Subtype /CIDFontType0\n"
                 "   /BaseFont /STSong-Light\n"
                 "   /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >>\n"
                 "   /DW 1000\n"
                 "   /W [ 1 127 500 ]\n"
                 ">>");
    }

    // 写入图片对象
    for (size_t m = 0; m < docImages.size(); ++m) {
        const auto& img = docImages[m];
        xref.push_back(static_cast<size_t>(out.tellp()));
        size_t id = xref.size() - 1;

        if (img.format == "jpeg") {
            out << id << " 0 obj\n"
                << "<< /Type /XObject /Subtype /Image\n"
                << "   /Width " << img.pixelWidth << " /Height " << img.pixelHeight << "\n"
                << "   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
                << "   /Filter /DCTDecode\n"
                << "   /Length " << img.data.size() << " >>\n"
                << "stream\n";
            out.write(reinterpret_cast<const char*>(img.data.data()), img.data.size());
            out << "\nendstream\nendobj\n";
        } else {
            int pw = img.pixelWidth, ph = img.pixelHeight;
            std::vector<uint8_t> compressedRgb;
            if (decodePngToRgbStream(img.data.data(), img.data.size(), pw, ph, compressedRgb)) {
                out << id << " 0 obj\n"
                    << "<< /Type /XObject /Subtype /Image\n"
                    << "   /Width " << pw << " /Height " << ph << "\n"
                    << "   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
                    << "   /Filter /FlateDecode\n"
                    << "   /Length " << compressedRgb.size() << " >>\n"
                    << "stream\n";
                out.write(reinterpret_cast<const char*>(compressedRgb.data()), compressedRgb.size());
                out << "\nendstream\nendobj\n";
            }
        }
    }

    // 写入附件对象（EmbeddedFile 压缩流与 Filespec 字典）
    for (size_t k = 0; k < attachments.size(); ++k) {
        const auto& att = attachments[k];
        const auto& aInfo = attObjs[k];

        xref.push_back(static_cast<size_t>(out.tellp()));
        out << aInfo.efId << " 0 obj\n"
            << "<< /Type /EmbeddedFile\n"
            << "   /Subtype /application#2Foctet-stream\n"
            << "   /Length " << aInfo.compressedData.size() << "\n"
            << "   /Params << /Size " << att.data.size() << " >>\n"
            << "   /Filter /FlateDecode\n"
            << ">>\nstream\n";
        out.write(reinterpret_cast<const char*>(aInfo.compressedData.data()), aInfo.compressedData.size());
        out << "\nendstream\nendobj\n";

        std::string safeFn = escapePdfString(att.filename);
        std::string safeDesc = escapePdfString(att.description.empty() ? att.filename : att.description);
        std::string fsContent = "<< /Type /Filespec\n"
                                "   /F (" + safeFn + ")\n"
                                "   /UF (" + safeFn + ")\n"
                                "   /EF << /F " + std::to_string(aInfo.efId) + " 0 R >>\n"
                                "   /Desc (" + safeDesc + ")\n"
                                ">>";
        writeObj(fsContent);
    }

    // 写入页面注记（/Subtype /Link 与 /Subtype /FileAttachment）
    for (size_t p = 0; p < numPages; ++p) {
        if (p >= pageAnnots.size()) continue;
        for (const auto& ar : pageAnnots[p]) {
            if (ar.type == "Link") {
                size_t targetPageObjId = 3 + ar.targetPageIndex * 2;
                std::string linkContent =
                    "<< /Type /Annot\n"
                    "   /Subtype /Link\n"
                    "   /Rect [ " + std::to_string(ar.x1) + " " + std::to_string(ar.y1) + " "
                                  + std::to_string(ar.x2) + " " + std::to_string(ar.y2) + " ]\n"
                    "   /Border [ 0 0 0 ]\n"
                    "   /A << /S /GoTo /D [ " + std::to_string(targetPageObjId) + " 0 R /XYZ "
                                              + std::to_string(ar.targetX) + " "
                                              + std::to_string(ar.targetY) + " null ] >>\n"
                    ">>";
                writeObj(linkContent);
            } else if (ar.type == "FileAttachment") {
                std::string safeFn = escapePdfString(ar.filename);
                std::string faContent =
                    "<< /Type /Annot\n"
                    "   /Subtype /FileAttachment\n"
                    "   /Rect [ " + std::to_string(ar.x1) + " " + std::to_string(ar.y1) + " "
                                  + std::to_string(ar.x2) + " " + std::to_string(ar.y2) + " ]\n"
                    "   /FS " + std::to_string(ar.filespecObjId) + " 0 R\n"
                    "   /Name /Paperclip\n"
                    "   /C [ 0.1 0.4 0.8 ]\n"
                    "   /T (" + safeFn + ")\n"
                    "   /Contents (双击打开或保存附件)\n"
                    ">>";
                writeObj(faContent);
            }
        }
    }

    for (size_t m = 0; m < docImages.size(); ++m) {
        const auto& img = docImages[m];
        xref.push_back((size_t)out.tellp());
        size_t id = xref.size() - 1;

        if (img.format == "jpeg") {
            out << id << " 0 obj\n"
                << "<< /Type /XObject /Subtype /Image\n"
                << "   /Width " << img.pixelWidth << " /Height " << img.pixelHeight << "\n"
                << "   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
                << "   /Filter /DCTDecode\n"
                << "   /Length " << img.data.size() << " >>\n"
                << "stream\n";
            out.write((const char*)img.data.data(), img.data.size());
            out << "\nendstream\nendobj\n";
        } else {
            int pw = img.pixelWidth, ph = img.pixelHeight;
            std::vector<uint8_t> compressedRgb;
            if (decodePngToRgbStream(img.data.data(), img.data.size(), pw, ph, compressedRgb)) {
                out << id << " 0 obj\n"
                    << "<< /Type /XObject /Subtype /Image\n"
                    << "   /Width " << pw << " /Height " << ph << "\n"
                    << "   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
                    << "   /Filter /FlateDecode\n"
                    << "   /Length " << compressedRgb.size() << " >>\n"
                    << "stream\n";
                out.write((const char*)compressedRgb.data(), compressedRgb.size());
                out << "\nendstream\nendobj\n";
            }
        }
    }

    size_t startXref = (size_t)out.tellp();
    out << "xref\n";
    out << "0 " << xref.size() << "\n";
    out << "0000000000 65535 f \n";
    for (size_t i = 1; i < xref.size(); ++i) {
        out << std::setw(10) << std::setfill('0') << xref[i] << " 00000 n \n";
    }
    out << "trailer\n";
    out << "<< /Size " << xref.size() << " /Root 1 0 R >>\n";
    out << "startxref\n";
    out << startXref << "\n";
    out << "%%EOF\n";
}

bool PdfRenderer::render(const Document& doc, const std::string& outputPath, std::string& err) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        err = "Cannot open output file: " + outputPath;
        return false;
    }

    TextMetricsEngine metrics;
    metrics.initForDocument(doc);
    FontGlyphRegistry glyphRegistry;

    const double pageWidth = doc.pageSetup.pageWidth;
    const double pageHeight = doc.pageSetup.pageHeight;
    const double marginX = doc.pageSetup.marginLeft;

    // Word 页眉页脚尺寸与有效版心边距计算
    double totalHeaderHeight = 0.0;
    double groupH = 0.0;
    for (size_t i = 0; i < doc.headerParagraphs.size(); ++i) {
        const auto& p = doc.headerParagraphs[i];
        double maxSz = 0.0;
        for (const auto& r : p.runs) {
            if (r.fontSize > maxSz) maxSz = r.fontSize;
        }
        if (p.runs.empty() && p.emptyLineHeight > 0.0) maxSz = p.emptyLineHeight;
        if (maxSz <= 0.0) maxSz = 10.5;
        if (doc.pageSetup.linePitch > 0.0 && maxSz < doc.pageSetup.linePitch) maxSz = doc.pageSetup.linePitch;
        double ls = (p.lineSpacing > 0.0) ? p.lineSpacing : 1.0;
        double pH = (maxSz * ls) + p.spaceAfter;
        if (p.sameLineAsPrev) {
            if (pH > groupH) {
                totalHeaderHeight += (pH - groupH);
                groupH = pH;
            }
        } else {
            totalHeaderHeight += pH;
            groupH = pH;
        }
    }

    double totalFooterHeight = 0.0;
    groupH = 0.0;
    for (size_t i = 0; i < doc.footerParagraphs.size(); ++i) {
        const auto& p = doc.footerParagraphs[i];
        double maxSz = 0.0;
        for (const auto& r : p.runs) {
            if (r.fontSize > maxSz) maxSz = r.fontSize;
        }
        if (p.runs.empty() && p.emptyLineHeight > 0.0) maxSz = p.emptyLineHeight;
        if (maxSz <= 0.0) maxSz = 10.5;
        if (doc.pageSetup.linePitch > 0.0 && maxSz < doc.pageSetup.linePitch) maxSz = doc.pageSetup.linePitch;
        double ls = (p.lineSpacing > 0.0) ? p.lineSpacing : 1.0;
        double pH = (maxSz * ls) + p.spaceAfter;
        if (p.sameLineAsPrev) {
            if (pH > groupH) {
                totalFooterHeight += (pH - groupH);
                groupH = pH;
            }
        } else {
            totalFooterHeight += pH;
            groupH = pH;
        }
    }

    // 动态根据页眉/页脚在页面上的实际占用边界进行计算：
    // 根据 Word / ECMA-376 规范：若页眉或页脚占用高度超出边距，正文区域自动自适应避让，确保绝不发生文字踩踏重叠
    const double safetyGap = (doc.pageSetup.linePitch > 0.0) ? (doc.pageSetup.linePitch * 0.6) : 10.0;
    const double dynMarginTop = doc.headerParagraphs.empty()
        ? doc.pageSetup.marginTop
        : std::max(doc.pageSetup.marginTop, doc.pageSetup.headerMargin + totalHeaderHeight + safetyGap);

    const double dynMarginBottom = doc.footerParagraphs.empty()
        ? doc.pageSetup.marginBottom
        : std::max(doc.pageSetup.marginBottom, doc.pageSetup.footerMargin + totalFooterHeight + safetyGap);

    const double marginTop = dynMarginTop;
    const double marginBottom = dynMarginBottom;
    const double contentWidth = pageWidth - doc.pageSetup.marginLeft - doc.pageSetup.marginRight;

    std::vector<ImageBlock> allImages;
    for (const auto& b : doc.blocks) {
        if (b.type == BlockType::Image && !b.image.data.empty()) {
            allImages.push_back(b.image);
        }
    }

    std::vector<std::string> pageContents;
    std::ostringstream currentStream;
    double currentY = pageHeight - marginTop;

    double prevSpaceAfter = 0.0;
    bool isPageStart = true;
    size_t curPageIndex = 0;
    std::vector<PdfAnnotRecord> allAnnots;
    std::map<std::string, std::tuple<size_t, double, double>> namedTargets;

    auto startNewPage = [&]() {
        if (!currentStream.str().empty()) {
            pageContents.push_back(currentStream.str());
            currentStream.str("");
            currentStream.clear();
            curPageIndex++;
        }
        currentY = pageHeight - marginTop;
        prevSpaceAfter = 0.0;
        isPageStart = true;
    };

    size_t currentImageIdx = 0;

    for (size_t bIdx = 0; bIdx < doc.blocks.size(); ++bIdx) {
        const auto& block = doc.blocks[bIdx];
        if (block.type == BlockType::Paragraph) {
            if (block.para.runs.empty() && block.para.pageBreakBefore) {
                startNewPage();
                continue;
            }
            if (isPageStart && block.para.runs.empty() && !block.para.bgColor.isValid) {
                continue;
            }
            double effectiveSpace = isPageStart ? 0.0 : std::max(prevSpaceAfter, block.para.spaceBefore);

            // 标题与下段同页保护 (keepNext): 若段落标记 keepNext，确保本页能容纳本段加上下段（至少前两行或整段），否则提前分页
            if (!isPageStart && block.para.keepNext) {
                double availW = contentWidth - block.para.leftIndent - block.para.rightIndent;
                if (availW <= 50.0) availW = contentWidth;
                auto lns = breakIntoLines(block.para, availW, metrics);
                double pHeight = 0.0;
                for (const auto& l : lns) {
                    pHeight += calcDynamicLineHeight(l, block.para.lineSpacing, doc.pageSetup.linePitch);
                }
                double nextMinHeight = 0.0;
                size_t nextIdx = bIdx + 1;
                while (nextIdx < doc.blocks.size() && doc.blocks[nextIdx].type == BlockType::Paragraph) {
                    const auto& nextP = doc.blocks[nextIdx].para;
                    double nAvailW = contentWidth - nextP.leftIndent - nextP.rightIndent;
                    if (nAvailW <= 50.0) nAvailW = contentWidth;
                    auto nLns = breakIntoLines(nextP, nAvailW, metrics);
                    if (nextP.keepNext) {
                        for (const auto& l : nLns) {
                            nextMinHeight += calcDynamicLineHeight(l, nextP.lineSpacing, doc.pageSetup.linePitch);
                        }
                        nextMinHeight += nextP.spaceBefore + nextP.spaceAfter;
                        ++nextIdx;
                    } else {
                        size_t reqLines = (nLns.size() <= 2) ? nLns.size() : 2;
                        for (size_t li = 0; li < reqLines; ++li) {
                            nextMinHeight += calcDynamicLineHeight(nLns[li], nextP.lineSpacing, doc.pageSetup.linePitch);
                        }
                        nextMinHeight += nextP.spaceBefore;
                        if (reqLines == nLns.size()) {
                            nextMinHeight += nextP.spaceAfter;
                        }
                        break;
                    }
                }
                if (nextMinHeight <= 0.0) {
                    nextMinHeight = (doc.pageSetup.linePitch > 0.0) ? (doc.pageSetup.linePitch * 2.0) : 36.0;
                }

                if (currentY - effectiveSpace - pHeight - nextMinHeight < marginBottom) {
                    startNewPage();
                    effectiveSpace = 0.0;
                }
            }

            currentY -= effectiveSpace;
            renderParagraph(block.para, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);
            prevSpaceAfter = block.para.spaceAfter;
            isPageStart = false;
        } else if (block.type == BlockType::Table) {
            double effectiveSpace = isPageStart ? 0.0 : prevSpaceAfter;
            currentY -= effectiveSpace;
            renderTable(block.table, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, &allAnnots, curPageIndex, &namedTargets);
            prevSpaceAfter = 0.0;
            isPageStart = false;
        } else if (block.type == BlockType::Image && !block.image.data.empty()) {
            const auto& img = block.image;
            double effectiveSpace = isPageStart ? 0.0 : std::max(prevSpaceAfter, 4.0);
            currentY -= effectiveSpace;
            if (currentY - img.displayHeightPt < marginBottom) {
                startNewPage();
            }

            double imgX = marginX + (contentWidth - img.displayWidthPt) / 2.0;
            double imgY = currentY - img.displayHeightPt;

            currentStream << "q "
                          << img.displayWidthPt << " 0 0 " << img.displayHeightPt << " "
                          << imgX << " " << imgY << " cm\n"
                          << "/Im" << (currentImageIdx + 1) << " Do\n"
                          << "Q\n";

            currentY -= img.displayHeightPt;
            prevSpaceAfter = 10.0;
            isPageStart = false;
            ++currentImageIdx;
        }
    }

    // 渲染脚注清单 (Footnotes)
    if (!doc.footnotes.empty()) {
        Paragraph fnTitleP;
        fnTitleP.spaceBefore = 12.0;
        fnTitleP.spaceAfter = 6.0;
        TextRun trFnTitle;
        trFnTitle.text = "脚注 (Footnotes):";
        trFnTitle.fontSize = 11.0;
        trFnTitle.bold = true;
        fnTitleP.runs.push_back(trFnTitle);

        if (currentY - 30.0 < marginBottom) {
            startNewPage();
        }
        currentStream << "0.5 w " << marginX << " " << currentY << " m "
                      << (marginX + 108.0) << " " << currentY << " l S\n";
        currentY -= 6.0;
        renderParagraph(fnTitleP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);

        for (const auto& kv : doc.footnotes) {
            const auto& fn = kv.second;
            Paragraph fnP;
            fnP.spaceBefore = 2.0;
            fnP.spaceAfter = 4.0;
            TextRun rId;
            rId.text = "[" + fn.id + "] ";
            rId.fontSize = 9.0;
            rId.bold = true;
            fnP.runs.push_back(rId);

            for (const auto& p : fn.paragraphs) {
                for (const auto& r : p.runs) {
                    TextRun rBody = r;
                    rBody.fontSize = 9.0;
                    fnP.runs.push_back(rBody);
                }
            }
            if (currentY - 15.0 < marginBottom) {
                startNewPage();
            }
            renderParagraph(fnP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);
        }
    }

    // 渲染批注与备注清单 (Comments & Notes)
    if (!doc.comments.empty()) {
        Paragraph cmTitleP;
        cmTitleP.headingLevel = 2;
        cmTitleP.spaceBefore = 10.0;
        cmTitleP.spaceAfter = 4.0;
        TextRun trCmTitle;
        trCmTitle.text = "批注清单 (Comments)";
        trCmTitle.fontSize = 12.0;
        trCmTitle.bold = true;
        cmTitleP.runs.push_back(trCmTitle);

        if (currentY - 40.0 < marginBottom) {
            startNewPage();
        }
        currentY -= cmTitleP.spaceBefore;
        renderParagraph(cmTitleP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);

        for (const auto& c : doc.comments) {
            // 设立批注在文末清单中的锚点
            namedTargets["comment_" + c.id] = std::make_tuple(curPageIndex, marginX, currentY);

            Paragraph cP;
            cP.spaceBefore = 4.0;
            cP.spaceAfter = 6.0;
            TextRun rPrefix;
            rPrefix.fontSize = 10.0;
            rPrefix.bold = true;
            rPrefix.color = ColorRGB(100, 100, 100);
            rPrefix.text = "[批注 " + c.id + "] ";
            if (!c.author.empty()) rPrefix.text += c.author + " ";
            if (!c.date.empty()) rPrefix.text += "(" + c.date + "): ";
            cP.runs.push_back(rPrefix);

            for (const auto& cp : c.paragraphs) {
                for (const auto& r : cp.runs) {
                    TextRun rBody = r;
                    rBody.fontSize = 10.0;
                    cP.runs.push_back(rBody);
                }
            }

            // 双向跳转支持：在批注末尾添加 [跳回正文] 链接
            TextRun rBack;
            rBack.text = " [跳回正文]";
            rBack.fontSize = 9.0;
            rBack.color = ColorRGB(0, 102, 204);
            rBack.linkTarget = "ref_" + c.id;
            cP.runs.push_back(rBack);

            if (currentY - 20.0 < marginBottom) {
                startNewPage();
            }
            renderParagraph(cP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);
        }
    }

    // 渲染内嵌附件清单 (Attachments)
    if (!doc.attachments.empty()) {
        Paragraph attTitleP;
        attTitleP.headingLevel = 2;
        attTitleP.spaceBefore = 10.0;
        attTitleP.spaceAfter = 4.0;
        TextRun trAttTitle;
        trAttTitle.text = "📎 内嵌附件清单 (Attachments)";
        trAttTitle.fontSize = 12.0;
        trAttTitle.bold = true;
        attTitleP.runs.push_back(trAttTitle);

        if (currentY - 40.0 < marginBottom) {
            startNewPage();
        }
        currentY -= attTitleP.spaceBefore;
        renderParagraph(attTitleP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);

        for (size_t aIdx = 0; aIdx < doc.attachments.size(); ++aIdx) {
            const auto& att = doc.attachments[aIdx];
            Paragraph attP;
            attP.spaceBefore = 4.0;
            attP.spaceAfter = 6.0;

            TextRun rIcon;
            rIcon.text = "[📎 附件 " + std::to_string(aIdx + 1) + "] " + att.filename;
            rIcon.fontSize = 10.5;
            rIcon.bold = true;
            rIcon.color = ColorRGB(0, 102, 204);
            rIcon.underline = true;
            attP.runs.push_back(rIcon);

            if (!att.description.empty() || !att.data.empty()) {
                TextRun rInfo;
                rInfo.fontSize = 9.5;
                rInfo.color = ColorRGB(100, 100, 100);
                std::string infoStr = " (";
                if (!att.description.empty()) infoStr += att.description + ", ";
                double szKb = static_cast<double>(att.data.size()) / 1024.0;
                char buf[32];
                if (szKb < 1024.0) {
                    snprintf(buf, sizeof(buf), "%.1f KB", szKb);
                } else {
                    snprintf(buf, sizeof(buf), "%.2f MB", szKb / 1024.0);
                }
                infoStr += buf;
                infoStr += ")";
                rInfo.text = infoStr;
                attP.runs.push_back(rInfo);
            }

            if (currentY - 24.0 < marginBottom) {
                startNewPage();
            }

            // 注册针对此附件的 FileAttachment 页面注释（点击或阅读器面板皆可查看下载）
            PdfAnnotRecord ar;
            ar.pageIndex = curPageIndex;
            ar.x1 = marginX;
            ar.y1 = currentY - 16.0;
            ar.x2 = marginX + 260.0;
            ar.y2 = currentY + 4.0;
            ar.type = "FileAttachment";
            ar.filename = att.filename;
            ar.attachmentIndex = aIdx;
            allAnnots.push_back(ar);

            renderParagraph(attP, currentStream, currentY, startNewPage, contentWidth, marginX, marginBottom, metrics, &glyphRegistry, doc.pageSetup.linePitch, &allAnnots, curPageIndex, &namedTargets);
        }
    }

    if (!currentStream.str().empty()) {
        pageContents.push_back(currentStream.str());
    }
    if (pageContents.empty()) {
        pageContents.push_back("");
    }

    // 页眉页脚注入与页码动态计算
    if (!doc.headerParagraphs.empty() || !doc.footerParagraphs.empty()) {
        size_t totalP = pageContents.size();
        // Word 页眉位置：由页面顶端向下偏移 headerMargin
        double headerY = pageHeight - doc.pageSetup.headerMargin;

        // Word 页脚位置：footerMargin 是页面底端到页脚底部的距离，从顶部向下排版
        double footerTopY = doc.pageSetup.footerMargin + (totalFooterHeight > 0.0 ? totalFooterHeight : 12.0);

        for (size_t p = 0; p < totalP; ++p) {
            std::ostringstream hfStream;
            if (!doc.headerParagraphs.empty()) {
                double curHeaderY = headerY;
                renderHeaderFooter(doc.headerParagraphs, hfStream, curHeaderY, contentWidth, marginX, p + 1, totalP, metrics, &glyphRegistry, doc.pageSetup.linePitch);
            }
            if (!doc.footerParagraphs.empty()) {
                double curFooterY = footerTopY;
                renderHeaderFooter(doc.footerParagraphs, hfStream, curFooterY, contentWidth, marginX, p + 1, totalP, metrics, &glyphRegistry, doc.pageSetup.linePitch);
            }
            pageContents[p] = hfStream.str() + pageContents[p];
        }
    }

    // 整理页面注记：解析超链接目的地的页码与坐标
    std::vector<std::vector<PdfAnnotRecord>> pageAnnots(pageContents.size());
    for (auto& ar : allAnnots) {
        if (ar.type == "Link") {
            auto it = namedTargets.find(ar.linkTarget);
            if (it != namedTargets.end()) {
                ar.targetPageIndex = std::get<0>(it->second);
                ar.targetX = std::get<1>(it->second);
                ar.targetY = std::get<2>(it->second);
            } else {
                ar.targetPageIndex = 0;
                ar.targetX = marginX;
                ar.targetY = pageHeight - marginTop;
            }
        }
        if (ar.pageIndex < pageAnnots.size()) {
            pageAnnots[ar.pageIndex].push_back(ar);
        }
    }

    writePdfStructure(out, pageContents, allImages, pageWidth, pageHeight, metrics.getAllFontsMutable(), pageAnnots, doc.attachments);
    return true;
}

} // namespace doc2pdf
