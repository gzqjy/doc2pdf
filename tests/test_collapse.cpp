
#include "doc2pdf/docx_parser.hpp"
#include <iostream>

using namespace doc2pdf;

int main() {
    DocxParser parser;
    Document doc;
    std::string err;
    parser.parse("testdata/risk_report_8083890e3.docx", doc, err);

    std::cout << "Total blocks: " << doc.blocks.size() << std::endl;
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const auto& b = doc.blocks[i];
        if (b.type == BlockType::Paragraph) {
            std::string t;
            for (const auto& r : b.para.runs) t += r.text;
            if (t.size() > 40) t = t.substr(0, 40) + "...";
            if (b.para.pageBreakBefore) {
                std::cout << "B" << i << " [PAGE BREAK] Para: \"" << t << "\"" << std::endl;
            } else if (!t.empty()) {
                std::cout << "B" << i << " Para: \"" << t << "\"" << std::endl;
            } else {
                std::cout << "B" << i << " EmptyPara" << std::endl;
            }
        } else if (b.type == BlockType::Table) {
            std::cout << "B" << i << " Table (rows=" << b.table.rows.size() << ")" << std::endl;
        } else if (b.type == BlockType::Image) {
            std::cout << "B" << i << " Image (w=" << b.image.displayWidthPt << ", h=" << b.image.displayHeightPt << ")" << std::endl;
        }
    }
    return 0;
}
