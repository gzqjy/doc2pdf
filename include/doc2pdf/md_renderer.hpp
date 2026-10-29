#pragma once
#include "doc2pdf/doc_ast.hpp"
#include <string>

namespace doc2pdf {

/**
 * @brief Markdown 导出器 (AST -> GFM Markdown)
 * 支持将统一 Document AST 导出为 Markdown 文件并自动将内嵌图片落盘。
 */
class MdRenderer {
public:
    static bool render(const Document& doc, const std::string& outputPath, std::string& err);
};

} // namespace doc2pdf
