#include "doc2pdf/pure_doc2pdf.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: pure_doc2pdf <input.doc|input.docx> <output.pdf|output.md>\n";
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

    PureDocConverter converter;
    bool isMd = (outputPath.size() >= 3 && outputPath.substr(outputPath.size() - 3) == ".md");

    if (isMd) {
        std::cout << "Converting to Markdown (GFM): " << inputPath << " -> " << outputPath << " ...\n";
        if (!converter.convertToMd(inputPath, outputPath)) {
            std::cerr << "Conversion failed: " << converter.getLastError() << "\n";
            return 2;
        }
    } else {
        std::cout << "Converting (Pure C++11): " << inputPath << " -> " << outputPath << " ...\n";
        if (!converter.convert(inputPath, outputPath)) {
            std::cerr << "Conversion failed: " << converter.getLastError() << "\n";
            return 2;
        }
    }

    std::cout << "Success! Output generated: " << outputPath << "\n";
    return 0;
}
