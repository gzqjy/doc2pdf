#ifndef IDOC_PARSER_HPP
#define IDOC_PARSER_HPP

#include "doc_ast.hpp"
#include <string>

namespace doc2pdf {

/**
 * @brief 格式适配器接口 (Adapter Pattern)
 * 屏蔽 .docx 和 .doc 的底层格式差异，统一转译为 Document AST。
 */
class IDocumentParser {
public:
    virtual ~IDocumentParser() = default;

    /**
     * @brief 解析源文档并填充至统一 DOM / AST 树
     * @param filePath 输入文件路径
     * @param outDoc 输出的抽象语法树
     * @param err 错误信息输出
     * @return 成功返回 true，失败返回 false
     */
    virtual bool parse(const std::string& filePath, Document& outDoc, std::string& err) = 0;
};

} // namespace doc2pdf

#endif // IDOC_PARSER_HPP
