#ifndef DOC_PARSER_HPP
#define DOC_PARSER_HPP

#include "idoc_parser.hpp"

namespace doc2pdf {

class DocParser : public IDocumentParser {
public:
    bool parse(const std::string& filePath, Document& outDoc, std::string& err) override;
};

} // namespace doc2pdf

#endif // DOC_PARSER_HPP
