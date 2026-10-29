#ifndef DOCX_PARSER_HPP
#define DOCX_PARSER_HPP

#include "idoc_parser.hpp"
#include <map>

namespace doc2pdf {

struct DocxStyle {
    std::string styleId;
    std::string styleName;
    std::string basedOn;
    bool isDefault = false;
    std::string fontFamily;
    double fontSize = 11.0;
    bool bold = false;
    bool italic = false;
    ColorRGB color;
    double spaceBefore = 0.0;
    double spaceAfter = 4.0;
    double lineSpacing = 1.35;
    Alignment align = Alignment::Left;
    bool keepNext = false;
};

class DocxParser : public IDocumentParser {
public:
    bool parse(const std::string& filePath, Document& outDoc, std::string& err) override;

private:
    static bool extractFileFromZip(const std::vector<uint8_t>& zipData, const std::string& targetName, std::vector<uint8_t>& out);
    static std::map<std::string, DocxStyle> parseStylesXml(const std::string& stylesXml);
    static void parseDocumentXmlWithMedia(const std::string& xml,
                                          Document& outDoc,
                                          const std::map<std::string, std::vector<uint8_t>>& imageCache,
                                          const std::map<std::string, DocxStyle>& styles,
                                          const std::map<std::string, std::string>& relsMap = std::map<std::string, std::string>(),
                                          const std::map<std::string, AttachmentItem>& attachmentMap = std::map<std::string, AttachmentItem>());
    static void parseParagraphXml(const std::string& pXml, Paragraph& para, const std::map<std::string, DocxStyle>& styles);
    static void parseTableXml(const std::string& tblXml, Table& table, const std::map<std::string, DocxStyle>& styles);
    static void parseHeaderFooterXml(const std::string& xml, std::vector<Paragraph>& outParas, const std::map<std::string, DocxStyle>& styles);
};

} // namespace doc2pdf

#endif // DOCX_PARSER_HPP
