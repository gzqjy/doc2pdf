#include "doc2pdf/md_renderer.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace doc2pdf {

static void ensureDirectory(const std::string& dir) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

bool MdRenderer::render(const Document& doc, const std::string& outputPath, std::string& err) {
    std::ofstream out(outputPath);
    if (!out) {
        err = "Cannot open output markdown file: " + outputPath;
        return false;
    }

    // 图片存储子目录: <base>_images/
    std::string baseName = outputPath;
    size_t dotPos = baseName.find_last_of('.');
    if (dotPos != std::string::npos) baseName = baseName.substr(0, dotPos);
    std::string imgDir = baseName + "_images";
    bool imgDirCreated = false;

    size_t imgIndex = 0;

    // 页眉 (如果存在)
    if (!doc.headerParagraphs.empty()) {
        for (const auto& hp : doc.headerParagraphs) {
            out << "> *";
            for (const auto& r : hp.runs) out << r.text;
            out << "*\n\n";
        }
    }

    for (const auto& block : doc.blocks) {
        if (block.type == BlockType::Paragraph) {
            const auto& p = block.para;
            if (p.runs.empty()) continue;

            if (p.headingLevel > 0) {
                for (int h = 0; h < p.headingLevel; ++h) out << "#";
                out << " ";
            }

            for (const auto& r : p.runs) {
                if (r.text.empty()) continue;
                std::string t = r.text;
                if (r.doubleStrikethrough || r.strikethrough) t = "~~" + t + "~~";
                if (r.emphasisMark) t = "<ins style=\"text-emphasis: filled circle;\">" + t + "</ins>";
                if (r.bold && r.italic) out << "***" << t << "***";
                else if (r.bold) out << "**" << t << "**";
                else if (r.italic) out << "*" << t << "*";
                else out << t;
            }
            out << "\n\n";
        } else if (block.type == BlockType::Table) {
            const auto& tbl = block.table;
            if (tbl.rows.empty()) continue;

            size_t maxCols = 0;
            for (const auto& r : tbl.rows) {
                if (r.cells.size() > maxCols) maxCols = r.cells.size();
            }
            if (maxCols == 0) continue;

            for (size_t rIdx = 0; rIdx < tbl.rows.size(); ++rIdx) {
                const auto& r = tbl.rows[rIdx];
                out << "|";
                for (size_t c = 0; c < maxCols; ++c) {
                    out << " ";
                    if (c < r.cells.size()) {
                        std::string cellTxt;
                        for (const auto& cp : r.cells[c].paragraphs) {
                            for (const auto& cr : cp.runs) {
                                cellTxt += cr.text;
                            }
                        }
                        // 替换换行与竖线
                        for (char ch : cellTxt) {
                            if (ch == '|') out << "\\|";
                            else if (ch == '\n' || ch == '\r') out << " ";
                            else out << ch;
                        }
                    }
                    out << " |";
                }
                out << "\n";

                // 输出表头分割线
                if (rIdx == 0) {
                    out << "|";
                    for (size_t c = 0; c < maxCols; ++c) {
                        out << " --- |";
                    }
                    out << "\n";
                }
            }
            out << "\n";
        } else if (block.type == BlockType::Image && !block.image.data.empty()) {
            if (!imgDirCreated) {
                ensureDirectory(imgDir);
                imgDirCreated = true;
            }

            std::string ext = (block.image.format == "jpeg") ? "jpg" : "png";
            std::string imgFileName = "img_" + std::to_string(++imgIndex) + "." + ext;
            std::string diskPath = imgDir + "/" + imgFileName;

            std::ofstream imgFile(diskPath, std::ios::binary);
            if (imgFile) {
                imgFile.write((const char*)block.image.data.data(), block.image.data.size());
            }

            // 获取相对路径用于 Markdown 引用
            std::string relPath = imgDir;
            size_t slash = relPath.find_last_of("/\\");
            if (slash != std::string::npos) relPath = relPath.substr(slash + 1);

            out << "![" << imgFileName << "](" << relPath << "/" << imgFileName << ")\n\n";
        }
    }

    // 页脚 (如果存在)
    if (!doc.footerParagraphs.empty()) {
        out << "---\n\n";
        for (const auto& fp : doc.footerParagraphs) {
            out << "<div align=\"center\">";
            for (const auto& r : fp.runs) {
                if (r.text == "PAGE_NUM") out << "1";
                else if (r.text == "NUM_PAGES") out << "1";
                else out << r.text;
            }
            out << "</div>\n\n";
        }
    }

    return true;
}

} // namespace doc2pdf
