#ifndef PURE_DOC2PDF_HPP
#define PURE_DOC2PDF_HPP

#include <string>
#include <vector>

/**
 * @brief 方案2+方案3：纯源码 C++11 实现的 DOC/DOCX 转 PDF 转换器
 * 特性：
 * 1. 零预编译二进制依赖（不依赖 LibreOffice、soffice、wps 或任何 .so/.dll）
 * 2. 跨架构直接编译（x86_64, aarch64, mips64el, loongarch64）
 * 3. 方案2：内置 ZIP 解包 + OOXML (WordprocessingML) 语义解析
 * 4. 方案3：内置 OLE2 / CFBF (Compound File Binary Format) 复合文档与 FIB Piece Table 解析
 * 5. 内置 PDF 1.4 生成与排版引擎（支持 A4 分页、自动折行、段落标题、表格边框、中英文混合排版）
 */
class PureDocConverter {
public:
    PureDocConverter();
    ~PureDocConverter();

    /**
     * @brief 转换 .doc 或 .docx 文件至 .pdf
     * @param inputPath 输入文件路径 (.doc 或 .docx)
     * @param outputPath 输出 .pdf 文件路径
     * @return 转换成功返回 true，失败返回 false
     */
    bool convert(const std::string& inputPath, const std::string& outputPath);

    /**
     * @brief 转换 .doc 或 .docx 文件至 Markdown (.md)
     * @param inputPath 输入文件路径 (.doc 或 .docx)
     * @param outputMdPath 输出 .md 文件路径
     * @return 转换成功返回 true，失败返回 false
     */
    bool convertToMd(const std::string& inputPath, const std::string& outputMdPath);

    /**
     * @brief 获取最后一次错误信息
     */
    const std::string& getLastError() const { return m_lastError; }

private:
    std::string m_lastError;
};

#endif // PURE_DOC2PDF_HPP
