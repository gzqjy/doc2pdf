#ifndef PDF_RENDERER_HPP
#define PDF_RENDERER_HPP

#include "doc_ast.hpp"
#include <string>

namespace doc2pdf {

/**
 * @brief 核心自绘排版与 PDF 1.4 生成引擎
 * 仅与 Document 抽象语法树耦合，完全脱离具体的 doc / docx 输入源。
 */
class PdfRenderer {
public:
    static bool render(const Document& doc, const std::string& outputPath, std::string& err);
};

} // namespace doc2pdf

#endif // PDF_RENDERER_HPP
