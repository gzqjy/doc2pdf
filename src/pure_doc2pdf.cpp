#include "doc2pdf/pure_doc2pdf.hpp"
#include "doc2pdf/docx_parser.hpp"
#include "doc2pdf/doc_parser.hpp"
#include "doc2pdf/pdf_renderer.hpp"
#include "doc2pdf/md_renderer.hpp"

#include <fstream>
#include <memory>

PureDocConverter::PureDocConverter() = default;
PureDocConverter::~PureDocConverter() = default;

static std::unique_ptr<doc2pdf::IDocumentParser> createParser(const std::string& inputPath, std::string& err) {
    std::ifstream in(inputPath, std::ios::binary);
    if (!in) {
        err = "Cannot open input file: " + inputPath;
        return nullptr;
    }

    uint8_t magic[8] = { 0 };
    in.read((char*)magic, 8);
    size_t count = in.gcount();
    in.close();

    if (count < 8) {
        err = "Input file is too small";
        return nullptr;
    }

    if (magic[0] == 0x50 && magic[1] == 0x4B && magic[2] == 0x03 && magic[3] == 0x04) {
        return std::unique_ptr<doc2pdf::IDocumentParser>(new doc2pdf::DocxParser());
    } else if (magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 && magic[3] == 0xE0) {
        return std::unique_ptr<doc2pdf::IDocumentParser>(new doc2pdf::DocParser());
    } else {
        err = "Unsupported file format. Must be .doc (OLE2) or .docx (OOXML)";
        return nullptr;
    }
}

bool PureDocConverter::convert(const std::string& inputPath, const std::string& outputPath) {
    auto parser = createParser(inputPath, m_lastError);
    if (!parser) return false;

    // 1. 适配器统一填充 Document 抽象语法树 (AST / DOM)
    doc2pdf::Document doc;
    if (!parser->parse(inputPath, doc, m_lastError)) {
        return false;
    }

    // 2. 自绘排版引擎与源格式完全解耦，仅接收 Document AST
    return doc2pdf::PdfRenderer::render(doc, outputPath, m_lastError);
}

bool PureDocConverter::convertToMd(const std::string& inputPath, const std::string& outputMdPath) {
    auto parser = createParser(inputPath, m_lastError);
    if (!parser) return false;

    doc2pdf::Document doc;
    if (!parser->parse(inputPath, doc, m_lastError)) {
        return false;
    }

    return doc2pdf::MdRenderer::render(doc, outputMdPath, m_lastError);
}
