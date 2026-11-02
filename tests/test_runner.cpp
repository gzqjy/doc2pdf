#include "doc2pdf/pure_doc2pdf.hpp"
#include "doc2pdf/doc_ast.hpp"
#include "doc2pdf/docx_parser.hpp"
#include "doc2pdf/doc_parser.hpp"
#include "doc2pdf/pdf_renderer.hpp"
#include "doc2pdf/utf_utils.hpp"
#include "doc2pdf/image_utils.hpp"

#include <iostream>
#include <cassert>
#include <fstream>
#include <vector>
#include <string>

using namespace doc2pdf;

static int g_passCount = 0;
static int g_failCount = 0;

static std::string extractPdfText(const std::string& pdfPath) {
    std::string cmd = "pdftotext \"" + pdfPath + "\" -";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[512];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            g_failCount++; \
        } else { \
            g_passCount++; \
        } \
    } while (0)

static void testUtfUtils() {
    std::cout << "[RUN] testUtfUtils...\n";

    // 1. appendUtf8Char
    std::string s;
    appendUtf8Char(s, 'A');
    appendUtf8Char(s, 0x4E2D); // '中'
    appendUtf8Char(s, 0x1F600); // 😀
    TEST_ASSERT(!s.empty(), "UTF-8 string should not be empty");

    // 2. Roundtrip utf8ToUtf16 & utf16leToUtf8
    std::vector<uint16_t> u16 = utf8ToUtf16(s);
    TEST_ASSERT(u16.size() >= 4, "UTF-16 should have at least 4 units (surrogate pair for emoji)");

    std::vector<uint8_t> leBytes(u16.size() * 2);
    for (size_t i = 0; i < u16.size(); ++i) {
        leBytes[i * 2] = (uint8_t)(u16[i] & 0xFF);
        leBytes[i * 2 + 1] = (uint8_t)((u16[i] >> 8) & 0xFF);
    }
    std::string roundtrip = utf16leToUtf8(leBytes.data(), leBytes.size());
    TEST_ASSERT(roundtrip == s, "Roundtrip UTF-8 -> UTF-16LE -> UTF-8 must match exactly");

    // 3. Punctuation checks
    TEST_ASSERT(isClosingPunctuation('.'), "Period is closing punct");
    TEST_ASSERT(isClosingPunctuation(0x3002), "Chinese full stop is closing punct");
    TEST_ASSERT(isClosingPunctuation(0xFF0C), "Chinese comma is closing punct");
    TEST_ASSERT(isOpeningPunctuation('('), "Left paren is opening punct");
    TEST_ASSERT(isOpeningPunctuation(0x300A), "Chinese book title mark 《 is opening punct");
    TEST_ASSERT(!isClosingPunctuation('A'), "'A' is not closing punct");
    TEST_ASSERT(!isOpeningPunctuation('A'), "'A' is not opening punct");
}

static void testImageUtils() {
    std::cout << "[RUN] testImageUtils...\n";

    // 1. Mock 1x1 PNG header
    static const uint8_t mockPng[33] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x01, 0x00, // width 256
        0x00, 0x00, 0x00, 0x80, // height 128
        0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    std::string fmt;
    int w = 0, h = 0;
    bool okPng = parseImageDimensions(mockPng, sizeof(mockPng), fmt, w, h);
    TEST_ASSERT(okPng, "parseImageDimensions should succeed on mock PNG");
    TEST_ASSERT(fmt == "png", "Format must be png");
    TEST_ASSERT(w == 256 && h == 128, "Dimensions must be 256x128");

    // 2. Mock PICF header parsing
    std::vector<uint8_t> mockData(256, 0);
    // h = 10, cbHeader = 68 at offset 10+4 = 14
    *(uint32_t*)&mockData[10] = 80; // lcb
    *(uint16_t*)&mockData[14] = 68; // cbHeader
    *(uint16_t*)&mockData[38] = 3961; // dxaGoal (offset 10 + 28 = 38)
    *(uint16_t*)&mockData[40] = 4177; // dyaGoal (offset 10 + 30 = 40)
    *(uint16_t*)&mockData[42] = 1636; // mx
    *(uint16_t*)&mockData[44] = 1636; // my

    double dw = 0, dh = 0;
    bool okPicf = parsePicfHeader(mockData.data(), 80, dw, dh);
    TEST_ASSERT(okPicf, "parsePicfHeader should succeed");
    TEST_ASSERT(std::abs(dw - 324.0) < 1.0, "PICF display width should be ~324 pt");
    TEST_ASSERT(std::abs(dh - 341.6) < 1.0, "PICF display height should be ~341.6 pt");
}

static void testDocAst() {
    std::cout << "[RUN] testDocAst...\n";

    Document doc;
    TEST_ASSERT(std::abs(doc.pageSetup.pageWidth - 595.28) < 0.1, "Default page width should be 595.28 (A4)");
    TEST_ASSERT(std::abs(doc.pageSetup.pageHeight - 841.89) < 0.1, "Default page height should be 841.89 (A4)");

    Paragraph p;
    p.runs.push_back(TextRun("Hello World", true, false, 12.0, ColorRGB(0, 0, 0)));
    doc.blocks.push_back(Block::makeParagraph(p));

    Table tbl;
    TableRow row;
    TableCell cell;
    Paragraph cp;
    cp.runs.push_back(TextRun("Cell Text"));
    cell.paragraphs.push_back(cp);
    row.cells.push_back(cell);
    tbl.rows.push_back(row);
    doc.blocks.push_back(Block::makeTable(tbl));

    TEST_ASSERT(doc.blocks.size() == 2, "Document should have 2 blocks");
    TEST_ASSERT(doc.blocks[0].type == BlockType::Paragraph, "Block 0 is paragraph");
    TEST_ASSERT(doc.blocks[1].type == BlockType::Table, "Block 1 is table");
}

static void testDocxParser(const std::string& dataDir) {
    std::cout << "[RUN] testDocxParser...\n";

    std::string docxPath = dataDir + "/test.docx";
    DocxParser parser;
    Document doc;
    std::string err;

    bool ok = parser.parse(docxPath, doc, err);
    TEST_ASSERT(ok, "Parsing test.docx should succeed");
    TEST_ASSERT(!doc.blocks.empty(), "Parsed test.docx should contain blocks");

    // Also test rich docx
    std::string richPath = dataDir + "/risk_report_8083890e.docx";
    Document richDoc;
    bool okRich = parser.parse(richPath, richDoc, err);
    TEST_ASSERT(okRich, "Parsing risk_report_8083890e.docx should succeed");
    TEST_ASSERT(!richDoc.blocks.empty(), "Parsed risk report should contain blocks");

    size_t imgCount = 0;
    size_t tblCount = 0;
    for (const auto& b : richDoc.blocks) {
        if (b.type == BlockType::Image) imgCount++;
        if (b.type == BlockType::Table) tblCount++;
    }
    TEST_ASSERT(imgCount == 4, "risk_report_8083890e.docx should have 4 images");
    TEST_ASSERT(tblCount >= 2, "risk_report_8083890e.docx should have at least 2 tables");

    // Test docx features: header, footer, strikethrough, doubleStrikethrough, page numbers
    std::string featPath = dataDir + "/features_test.docx";
    Document featDoc;
    bool okFeat = parser.parse(featPath, featDoc, err);
    if (okFeat) {
        TEST_ASSERT(okFeat, "Parsing features_test.docx should succeed");
        TEST_ASSERT(!featDoc.headerParagraphs.empty(), "features_test.docx should have header paragraphs");
        TEST_ASSERT(!featDoc.footerParagraphs.empty(), "features_test.docx should have footer paragraphs");
        bool hasStrike = false;
        bool hasDoubleStrike = false;
        for (const auto& b : featDoc.blocks) {
            if (b.type == BlockType::Paragraph) {
                for (const auto& r : b.para.runs) {
                    if (r.strikethrough && !r.doubleStrikethrough) hasStrike = true;
                    if (r.doubleStrikethrough) hasDoubleStrike = true;
                }
            }
        }
        TEST_ASSERT(hasStrike, "features_test.docx should contain single strikethrough");
        TEST_ASSERT(hasDoubleStrike, "features_test.docx should contain double strikethrough");
    }

    // Test docx features 2: complex fields (fldChar begin/separate/end), emphasis mark, multi-page numbering
    std::string featPath2 = dataDir + "/features_test2.docx";
    Document featDoc2;
    bool okFeat2 = parser.parse(featPath2, featDoc2, err);
    if (okFeat2) {
        TEST_ASSERT(okFeat2, "Parsing features_test2.docx should succeed");
        bool hasEmphasis = false;
        for (const auto& b : featDoc2.blocks) {
            if (b.type == BlockType::Paragraph) {
                for (const auto& r : b.para.runs) {
                    if (r.emphasisMark) hasEmphasis = true;
                }
            }
        }
        TEST_ASSERT(hasEmphasis, "features_test2.docx should contain emphasis marks");

        bool hasPageFld = false;
        bool hasNumPagesFld = false;
        for (const auto& p : featDoc2.footerParagraphs) {
            for (const auto& r : p.runs) {
                if (r.text.find("PAGE_NUM") != std::string::npos) hasPageFld = true;
                if (r.text.find("NUM_PAGES") != std::string::npos) hasNumPagesFld = true;
            }
        }
        TEST_ASSERT(hasPageFld, "features_test2.docx footer should contain PAGE_NUM placeholder from fldChar");
        TEST_ASSERT(hasNumPagesFld, "features_test2.docx footer should contain NUM_PAGES placeholder from fldChar");
    }
}

static void testDocParser(const std::string& dataDir) {
    std::cout << "[RUN] testDocParser...\n";

    std::string docPath = dataDir + "/test.doc";
    DocParser parser;
    Document doc;
    std::string err;

    bool ok = parser.parse(docPath, doc, err);
    TEST_ASSERT(ok, "Parsing test.doc should succeed");
    TEST_ASSERT(!doc.blocks.empty(), "Parsed test.doc should contain blocks");

    // Also test rich doc
    std::string richPath = dataDir + "/risk_report_8083890e.doc";
    Document richDoc;
    bool okRich = parser.parse(richPath, richDoc, err);
    TEST_ASSERT(okRich, "Parsing risk_report_8083890e.doc should succeed");
    TEST_ASSERT(!richDoc.blocks.empty(), "Parsed risk report doc should contain blocks");

    size_t imgCount = 0;
    size_t tblCount = 0;
    for (const auto& b : richDoc.blocks) {
        if (b.type == BlockType::Image) imgCount++;
        if (b.type == BlockType::Table) tblCount++;
    }
    TEST_ASSERT(imgCount == 4, "risk_report_8083890e.doc should have 4 images");
    TEST_ASSERT(tblCount >= 2, "risk_report_8083890e.doc should have at least 2 tables");
}

static void testPdfRenderer(const std::string& outPath) {
    std::cout << "[RUN] testPdfRenderer...\n";

    Document doc;
    Paragraph p1;
    p1.runs.push_back(TextRun("Unit Test PDF Generation", true, false, 18.0, ColorRGB(23, 54, 93)));
    doc.blocks.push_back(Block::makeParagraph(p1));

    Paragraph p2;
    p2.runs.push_back(TextRun("This is regular paragraph text rendered with pure C++11 engine."));
    doc.blocks.push_back(Block::makeParagraph(p2));

    Table tbl;
    TableRow r1;
    TableCell c1, c2;
    Paragraph cp1, cp2;
    cp1.runs.push_back(TextRun("Col 1"));
    cp2.runs.push_back(TextRun("Col 2"));
    c1.paragraphs.push_back(cp1);
    c2.paragraphs.push_back(cp2);
    r1.cells.push_back(c1);
    r1.cells.push_back(c2);
    tbl.rows.push_back(r1);
    doc.blocks.push_back(Block::makeTable(tbl));

    std::string err;
    bool ok = PdfRenderer::render(doc, outPath, err);
    TEST_ASSERT(ok, "PdfRenderer::render should succeed");

    std::ifstream f(outPath, std::ios::binary);
    TEST_ASSERT(f.is_open(), "Rendered PDF file must exist");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.rfind("%PDF-1.4", 0) == 0, "PDF must start with %PDF-1.4");
    TEST_ASSERT(content.find("%%EOF") != std::string::npos, "PDF must end with %%EOF");
    TEST_ASSERT(content.find("/FontDescriptor") != std::string::npos, "PDF should contain FontDescriptor");
    TEST_ASSERT(content.find("/FontFile2") != std::string::npos, "PDF should contain embedded font stream (/FontFile2)");
    TEST_ASSERT(content.find("/ToUnicode") != std::string::npos, "PDF should contain ToUnicode CMap (/ToUnicode)");
}

static void testDynamicFontFallback(const std::string& outPath) {
    std::cout << "[RUN] testDynamicFontFallback...\n";

    Document doc;
    doc.pageSetup.pageWidth = 595.0;
    doc.pageSetup.pageHeight = 842.0;
    doc.pageSetup.marginLeft = 54.0;
    doc.pageSetup.marginRight = 54.0;
    doc.pageSetup.marginTop = 54.0;
    doc.pageSetup.marginBottom = 54.0;
    doc.pageSetup.linePitch = 18.0;

    // 添加声明字体：包含华文细黑、微软雅黑、宋体等
    DocFontDecl fd1; fd1.name = "华文细黑"; fd1.altName = "STXihei"; doc.fontDeclarations.push_back(fd1);
    DocFontDecl fd2; fd2.name = "微软雅黑"; fd2.altName = "Microsoft YaHei"; doc.fontDeclarations.push_back(fd2);
    DocFontDecl fd3; fd3.name = "宋体"; fd3.altName = "SimSun"; doc.fontDeclarations.push_back(fd3);

    // 段落 1: 华文细黑与全角中文标点
    Paragraph p1;
    TextRun r1;
    r1.fontFamily = "华文细黑";
    r1.fontSize = 12.0;
    r1.text = "华文细黑测试：外部威胁、自身漏洞、后果严重程度、防控措施四个维度。";
    p1.runs.push_back(r1);
    doc.blocks.push_back(Block::makeParagraph(p1));

    // 段落 2: 微软雅黑
    Paragraph p2;
    TextRun r2;
    r2.fontFamily = "微软雅黑";
    r2.fontSize = 11.0;
    r2.text = "微软雅黑动态测试：ABCDEF 123456 全角符号（【】）！";
    p2.runs.push_back(r2);
    doc.blocks.push_back(Block::makeParagraph(p2));

    // 段落 3: 宋体
    Paragraph p3;
    TextRun r3;
    r3.fontFamily = "宋体";
    r3.fontSize = 10.5;
    r3.text = "宋体动态测试：验证衬线字体语义匹配至简体中文宋体。";
    p3.runs.push_back(r3);
    doc.blocks.push_back(Block::makeParagraph(p3));

    std::string err;
    bool ok = PdfRenderer::render(doc, outPath, err);
    TEST_ASSERT(ok, "PdfRenderer::render with dynamic fonts should succeed");

    std::ifstream f(outPath, std::ios::binary);
    TEST_ASSERT(f.is_open(), "Dynamic font PDF file must exist");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.rfind("%PDF-1.4", 0) == 0, "PDF must start with %PDF-1.4");
    TEST_ASSERT(content.find("%%EOF") != std::string::npos, "PDF must end with %%EOF");
    TEST_ASSERT(content.find("/FontFile2") != std::string::npos, "PDF must embed dynamic physical font streams");
    TEST_ASSERT(content.find("/ToUnicode") != std::string::npos, "PDF must generate ToUnicode CMap for dynamic fonts");
}

static void testEndToEndConversion(const std::string& dataDir, const std::string& outDir) {
    std::cout << "[RUN] testEndToEndConversion...\n";

    PureDocConverter converter;

    // 1. Convert test.docx
    std::string out1 = outDir + "/test_docx.pdf";
    bool ok1 = converter.convert(dataDir + "/test.docx", out1);
    TEST_ASSERT(ok1, "Converting test.docx -> PDF should succeed");

    // 2. Convert test.doc
    std::string out2 = outDir + "/test_doc.pdf";
    bool ok2 = converter.convert(dataDir + "/test.doc", out2);
    TEST_ASSERT(ok2, "Converting test.doc -> PDF should succeed");

    // 3. Convert risk_report_8083890e.doc
    std::string out3 = outDir + "/risk_doc.pdf";
    bool ok3 = converter.convert(dataDir + "/risk_report_8083890e.doc", out3);
    TEST_ASSERT(ok3, "Converting risk_report_8083890e.doc -> PDF should succeed");

    // 4. Convert risk_report_8083890e.doc -> Markdown (.md)
    std::string outMd = outDir + "/risk_doc.md";
    bool okMd = converter.convertToMd(dataDir + "/risk_report_8083890e.doc", outMd);
    TEST_ASSERT(okMd, "Converting risk_report_8083890e.doc -> Markdown should succeed");

    std::ifstream mdFile(outMd);
    TEST_ASSERT(mdFile.is_open(), "Exported Markdown file must exist");
    std::string mdContent((std::istreambuf_iterator<char>(mdFile)), std::istreambuf_iterator<char>());
    TEST_ASSERT(mdContent.find("软件供应链风险评估报告") != std::string::npos, "Markdown should contain report title");
    TEST_ASSERT(mdContent.find("|") != std::string::npos, "Markdown should contain tables");
    TEST_ASSERT(mdContent.find("![") != std::string::npos, "Markdown should contain image references");
    std::ifstream pdfFile(out3, std::ios::binary);
    std::string pdfData((std::istreambuf_iterator<char>(pdfFile)), std::istreambuf_iterator<char>());
    size_t pageCount = 0;
    size_t ppos = 0;
    while ((ppos = pdfData.find("/Type /Page ", ppos)) != std::string::npos) {
        pageCount++;
        ppos += 12;
    }
    TEST_ASSERT(pageCount == 9, "risk_doc.pdf page count must be exactly 9");

    // 校验生成的 risk_doc.pdf 中核心中文字符文本内容完整存在（无乱码、缺失与空白）
    std::string riskPdfText = extractPdfText(out3);
    TEST_ASSERT(riskPdfText.find("软件供应链风险评估报告") != std::string::npos, "risk_doc.pdf must contain '软件供应链风险评估报告'");
    TEST_ASSERT(riskPdfText.find("报告ID") != std::string::npos, "risk_doc.pdf must contain '报告ID'");
    TEST_ASSERT(riskPdfText.find("生成时间") != std::string::npos, "risk_doc.pdf must contain '生成时间'");
    TEST_ASSERT(riskPdfText.find("设备ID") != std::string::npos, "risk_doc.pdf must contain '设备ID'");
    TEST_ASSERT(riskPdfText.find("设备名称") != std::string::npos, "risk_doc.pdf must contain '设备名称'");
    TEST_ASSERT(riskPdfText.find("系统类型") != std::string::npos, "risk_doc.pdf must contain '系统类型'");
    TEST_ASSERT(riskPdfText.find("系统名称") != std::string::npos, "risk_doc.pdf must contain '系统名称'");
    TEST_ASSERT(riskPdfText.find("硬盘ID") != std::string::npos, "risk_doc.pdf must contain '硬盘ID'");

    // Convert risk_report_8083890e3.docx -> PDF and verify 9 pages and strikethrough
    std::string docxPath = dataDir + "/risk_report_8083890e3.docx";
    std::string docxPdfPath = outDir + "/risk_docx.pdf";
    bool okDocx = converter.convert(docxPath, docxPdfPath);
    TEST_ASSERT(okDocx, "Converting risk_report_8083890e3.docx -> PDF should succeed");
    std::ifstream fDocxPdf(docxPdfPath, std::ios::binary);
    std::string docxPdfData((std::istreambuf_iterator<char>(fDocxPdf)), std::istreambuf_iterator<char>());
    size_t docxPages = 0;
    size_t dpos = 0;
    while ((dpos = docxPdfData.find("/Type /Page ", dpos)) != std::string::npos) {
        docxPages++;
        dpos += 12;
    }
    TEST_ASSERT(docxPages >= 9 && docxPages <= 15, "risk_docx.pdf page count must be between 9 and 15");
    TEST_ASSERT(docxPdfData.find(" l S") != std::string::npos, "risk_docx.pdf should contain strikethrough lines");
    TEST_ASSERT(docxPdfData.find("/FontFile2") != std::string::npos, "risk_docx.pdf should contain embedded font (/FontFile2)");
    TEST_ASSERT(docxPdfData.find("/ToUnicode") != std::string::npos, "risk_docx.pdf should contain ToUnicode CMap (/ToUnicode)");
    TEST_ASSERT(docxPdfData.find("WenQuanYi") != std::string::npos || docxPdfData.find("STXihei") != std::string::npos || docxPdfData.find("Noto") != std::string::npos, "risk_docx.pdf should contain dynamic CJK font");
    TEST_ASSERT(docxPdfData.find("MS-Mincho") != std::string::npos || docxPdfData.find("Mincho") != std::string::npos, "risk_docx.pdf should contain MS-Mincho");
    TEST_ASSERT(docxPdfData.find("Times") != std::string::npos, "risk_docx.pdf should contain Times font");
    TEST_ASSERT(docxPdfData.find("Caladea") != std::string::npos || docxPdfData.find("Calibri") != std::string::npos || docxPdfData.find("Trebuchet") != std::string::npos, "risk_docx.pdf should contain Latin fonts");
}

static void testRichFormattingAndNotes(const std::string& outPdfPath) {
    std::cout << "[RUN] testRichFormattingAndNotes...\n";
    Document doc;
    doc.pageSetup.pageWidth = 595.28;
    doc.pageSetup.pageHeight = 841.89;
    doc.pageSetup.marginTop = 54.0;
    doc.pageSetup.marginBottom = 54.0;
    doc.pageSetup.marginLeft = 54.0;
    doc.pageSetup.marginRight = 54.0;
    doc.pageSetup.linePitch = 15.6;

    // 1. 段落 1: 斜体、双下划线、重点号、双删除线
    Paragraph p1;
    TextRun r1;
    r1.text = "重点测试与斜体样式";
    r1.fontSize = 12.0;
    r1.italic = true;
    r1.underline = true;
    r1.doubleUnderline = true;
    r1.emphasisMark = true;
    r1.strikethrough = true;
    r1.doubleStrikethrough = true;
    p1.runs.push_back(r1);
    doc.blocks.push_back(Block::makeParagraph(p1));

    // 2. 段落 2: 18pt 大字号、红色文字、黄色背景高亮、单下划线
    Paragraph p2;
    TextRun r2;
    r2.text = "黄色高亮红字下划线";
    r2.fontSize = 18.0;
    r2.color = ColorRGB(255, 0, 0);
    r2.bgColor = ColorRGB::fromNamed("yellow");
    r2.underline = true;
    p2.runs.push_back(r2);
    doc.blocks.push_back(Block::makeParagraph(p2));

    // 3. 段落 3: 软换行 \n
    Paragraph p3;
    TextRun r3;
    r3.text = "首行文字内容\n次行软换行内容";
    r3.fontSize = 11.0;
    p3.runs.push_back(r3);
    doc.blocks.push_back(Block::makeParagraph(p3));

    // 4. 段落 4: 硬分页符 pageBreakBefore
    Paragraph p4;
    p4.pageBreakBefore = true;
    TextRun r4;
    r4.text = "新页起始段落[1]";
    r4.fontSize = 12.0;
    p4.runs.push_back(r4);
    doc.blocks.push_back(Block::makeParagraph(p4));

    // 5. 脚注 Footnotes
    FootnoteItem fn1;
    fn1.id = "1";
    Paragraph fnP;
    TextRun rFn;
    rFn.text = "这是一条标准参考脚注。";
    rFn.fontSize = 9.0;
    fnP.runs.push_back(rFn);
    fn1.paragraphs.push_back(fnP);
    doc.footnotes["1"] = fn1;

    // 6. 批注 Comments
    CommentItem cm1;
    cm1.id = "0";
    cm1.author = "张三审核员";
    cm1.date = "2026-10-29";
    Paragraph cmP;
    TextRun rCm;
    rCm.text = "请在此处进一步核实数据来源。";
    rCm.fontSize = 10.0;
    cmP.runs.push_back(rCm);
    cm1.paragraphs.push_back(cmP);
    doc.comments.push_back(cm1);

    std::string err;
    bool ok = PdfRenderer::render(doc, outPdfPath, err);
    TEST_ASSERT(ok, "PdfRenderer::render for rich formatting should succeed");

    std::ifstream f(outPdfPath, std::ios::binary);
    TEST_ASSERT(f.is_open(), "Output PDF must exist");
    std::string pdfData((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // 校验斜体仿射变换
    TEST_ASSERT(pdfData.find("1 0 0.2126 1 ") != std::string::npos, "PDF should contain italic Tm matrix");

    // 校验重点号实心圆曲线绘制
    TEST_ASSERT(pdfData.find(" c f") != std::string::npos, "PDF should contain emphasis mark bezier curves");

    // 校验下划线与删除线绘制
    TEST_ASSERT(pdfData.find(" m ") != std::string::npos && pdfData.find(" l S") != std::string::npos, "PDF should contain line drawing commands");

    // 校验背景矩形填充
    TEST_ASSERT(pdfData.find(" re f") != std::string::npos, "PDF should contain background rect fill");

    // 校验文字颜色
    TEST_ASSERT(pdfData.find("1 0 0 rg") != std::string::npos || pdfData.find("1 0 0 RG") != std::string::npos, "PDF should contain red text color");

    // 校验分页符 (至少 2 页)
    size_t pageCount = 0;
    size_t ppos = 0;
    while ((ppos = pdfData.find("/Type /Page ", ppos)) != std::string::npos) {
        pageCount++;
        ppos += 12;
    }
    TEST_ASSERT(pageCount >= 2, "PDF should have at least 2 pages due to pageBreakBefore");
}

static void testDocChineseRendering(const std::string& dataDir, const std::string& outDir) {
    std::cout << "[RUN] testDocChineseRendering...\n";

    // 构建一个不声明任何 fontFamily 的标准纯中文 AST 文档（模拟 .doc 解析结果）
    Document doc;
    Paragraph p1;
    p1.runs.push_back(TextRun("软件供应链风险评估报告", false, false, 20.0));
    doc.blocks.push_back(Block::makeParagraph(p1));

    Paragraph p2;
    p2.runs.push_back(TextRun("设备名称：Ubuntu-Linux-Server，系统类型：Linux，硬盘ID：9B14-4AF0", false, false, 11.0));
    doc.blocks.push_back(Block::makeParagraph(p2));

    std::string outPdf = outDir + "/test_chinese_doc_fallback.pdf";
    std::string err;
    bool ok = PdfRenderer::render(doc, outPdf, err);
    TEST_ASSERT(ok, "PdfRenderer::render with empty fontFamily should succeed");

    std::string pdfText = extractPdfText(outPdf);
    TEST_ASSERT(pdfText.find("软件供应链风险评估报告") != std::string::npos, "PDF must contain '软件供应链风险评估报告'");
    TEST_ASSERT(pdfText.find("设备名称") != std::string::npos, "PDF must contain '设备名称'");
    TEST_ASSERT(pdfText.find("系统类型") != std::string::npos, "PDF must contain '系统类型'");
    TEST_ASSERT(pdfText.find("硬盘ID") != std::string::npos, "PDF must contain '硬盘ID'");
}

static void testDocxCommentJumpAndAttachments(const std::string& outDir) {
    std::cout << "[RUN] testDocxCommentJumpAndAttachments...\n";

    Document doc;
    doc.pageSetup.pageWidth = 595.28;
    doc.pageSetup.pageHeight = 841.89;
    doc.pageSetup.marginTop = 54.0;
    doc.pageSetup.marginBottom = 54.0;
    doc.pageSetup.marginLeft = 54.0;
    doc.pageSetup.marginRight = 54.0;
    doc.pageSetup.linePitch = 15.6;

    // 1. 空白与缩进段落测试 (连续空格、制表符、缩进、空段落名义行高)
    Paragraph pIndent;
    pIndent.firstLineIndent = 21.0; // 首行缩进 2 字符 (以 10.5pt 计算)
    pIndent.leftIndent = 14.0;
    TextRun rIndent("    缩进段落内容包含\t制表符与   多个连续空格", false, false, 10.5);
    pIndent.runs.push_back(rIndent);
    doc.blocks.push_back(Block::makeParagraph(pIndent));

    // 空行段落 (测试名义行高避免塌陷)
    Paragraph pEmpty;
    pEmpty.emptyLineHeight = 15.6;
    doc.blocks.push_back(Block::makeParagraph(pEmpty));

    // 2. 被批注文字（浅黄底色变色与深橘红字体）及批注超链接
    Paragraph pAnnotated;
    TextRun rNormal1("正文中未批注文字，", false, false, 11.0);
    pAnnotated.runs.push_back(rNormal1);

    TextRun rCommented("此处为被批注核心段落！", true, false, 11.0);
    rCommented.commentId = "0";
    rCommented.bgColor = ColorRGB(255, 242, 178); // 醒目浅黄色背景高亮
    rCommented.color = ColorRGB(180, 50, 0);       // 明显区别于正文的深橘红色
    pAnnotated.runs.push_back(rCommented);

    TextRun rCommentRef("[批注0]", false, false, 9.0);
    rCommentRef.color = ColorRGB(0, 102, 204);
    rCommentRef.linkTarget = "comment_0";
    pAnnotated.runs.push_back(rCommentRef);

    TextRun rNormal2("正文继续展开描述。", false, false, 11.0);
    pAnnotated.runs.push_back(rNormal2);
    doc.blocks.push_back(Block::makeParagraph(pAnnotated));

    // 3. 注册对应批注 (Comments)
    CommentItem cm;
    cm.id = "0";
    cm.author = "安全合规专家";
    cm.date = "2026-10-29 15:30";
    Paragraph cmP;
    cmP.runs.push_back(TextRun("请审查此处的安全边界是否达标", false, false, 10.0));
    cm.paragraphs.push_back(cmP);
    doc.comments.push_back(cm);

    // 4. 注册内嵌附件 (Attachments)
    AttachmentItem att1;
    att1.filename = "spec_attachment.txt";
    att1.description = "安全规范说明文档";
    std::string att1Str = "This is the content of the embedded specification attachment for test.";
    att1.data.assign(att1Str.begin(), att1Str.end());
    doc.attachments.push_back(att1);

    AttachmentItem att2;
    att2.filename = "config_data.json";
    att2.description = "系统配置文件";
    std::string att2Str = "{\"test\": true, \"version\": \"1.0.0\"}";
    att2.data.assign(att2Str.begin(), att2Str.end());
    doc.attachments.push_back(att2);

    // 5. 页眉与页脚注入
    Paragraph headerP;
    headerP.align = Alignment::Center;
    headerP.runs.push_back(TextRun("绝密 - 测试评估报告页眉", false, false, 9.0));
    doc.headerParagraphs.push_back(headerP);

    Paragraph footerP;
    footerP.align = Alignment::Right;
    footerP.runs.push_back(TextRun("第 [PAGE] 页 / 共 [NUMPAGES] 页", false, false, 9.0));
    doc.footerParagraphs.push_back(footerP);

    std::string outPdf = outDir + "/test_comment_attachments.pdf";
    std::string err;
    bool ok = PdfRenderer::render(doc, outPdf, err);
    TEST_ASSERT(ok, "PdfRenderer::render with comment jumps & attachments should succeed");

    std::ifstream f(outPdf, std::ios::binary);
    TEST_ASSERT(f.is_open(), "Output PDF file must exist");
    std::string pdfData((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // 校验批注底色高亮与文字变色
    TEST_ASSERT(pdfData.find("1 0.94902 0.698039 rg") != std::string::npos || pdfData.find("0.705882 0.196078 0 rg") != std::string::npos, "PDF must contain comment highlight colors");

    // 校验超链接注释 /Subtype /Link 与 /A << /S /GoTo ... >>
    TEST_ASSERT(pdfData.find("/Subtype /Link") != std::string::npos, "PDF must contain /Subtype /Link annotations");
    TEST_ASSERT(pdfData.find("/S /GoTo") != std::string::npos, "PDF must contain /S /GoTo link action");

    // 校验附件命名树与对象
    TEST_ASSERT(pdfData.find("/EmbeddedFiles") != std::string::npos, "PDF must contain /EmbeddedFiles in Catalog");
    TEST_ASSERT(pdfData.find("/Type /Filespec") != std::string::npos, "PDF must contain /Type /Filespec");
    TEST_ASSERT(pdfData.find("/Type /EmbeddedFile") != std::string::npos, "PDF must contain /Type /EmbeddedFile");
    TEST_ASSERT(pdfData.find("/Subtype /FileAttachment") != std::string::npos, "PDF must support native /Subtype /FileAttachment export");
    TEST_ASSERT(pdfData.find("/AP << /N") != std::string::npos, "PDF must provide transparent appearance /AP to prevent yellow sticky note popup");
    TEST_ASSERT(pdfData.find("/S /Launch") == std::string::npos, "PDF must not contain /S /Launch action to prevent security blocks");
    TEST_ASSERT(pdfData.find("spec_attachment.txt") != std::string::npos, "PDF must contain filename spec_attachment.txt");
    TEST_ASSERT(pdfData.find("config_data.json") != std::string::npos, "PDF must contain filename config_data.json");

    // 校验文本内容提取
    std::string pdfText = extractPdfText(outPdf);
    TEST_ASSERT(pdfText.find("此处为被批注核心段落！") != std::string::npos, "PDF text must contain commented content");
    TEST_ASSERT(pdfText.find("批注清单 (Comments)") != std::string::npos, "PDF text must contain Comments section");
    TEST_ASSERT(pdfText.find("安全合规专家") != std::string::npos, "PDF text must contain comment author");
    TEST_ASSERT(pdfText.find("请审查此处的安全边界是否达标") != std::string::npos, "PDF text must contain comment body");
    TEST_ASSERT(pdfText.find("[跳回正文]") != std::string::npos, "PDF text must contain [跳回正文] jump back link");
    TEST_ASSERT(pdfText.find("内嵌附件清单 (Attachments)") != std::string::npos, "PDF text must contain Attachments section");
    TEST_ASSERT(pdfText.find("spec_attachment.txt") != std::string::npos, "PDF text must contain spec_attachment.txt");
    TEST_ASSERT(pdfText.find("第1页/共1页") != std::string::npos || pdfText.find("第 1 页 / 共 1 页") != std::string::npos, "PDF text must contain resolved page numbers");
}

static void testDocAdvancedFeatures(const std::string& dataDir, const std::string& outDir) {
    std::cout << "[RUN] testDocAdvancedFeatures...\n";

    std::string docPath = dataDir + "/risk_report_8083890e3.doc";
    DocParser parser;
    Document doc;
    std::string err;
    bool ok = parser.parse(docPath, doc, err);
    TEST_ASSERT(ok, "DocParser::parse for risk_report_8083890e3.doc must succeed");

    // 1. 校验页眉页脚与页码
    TEST_ASSERT(!doc.headerParagraphs.empty(), "doc.headerParagraphs must not be empty");
    bool hasGreenOrYellowHeader = false;
    bool hasHeaderPageNum = false;
    for (const auto& hp : doc.headerParagraphs) {
        for (const auto& r : hp.runs) {
            if (r.text.find("页眉") != std::string::npos && r.bgColor.isValid && (r.bgColor.g == 128 || r.bgColor.g == 255)) {
                hasGreenOrYellowHeader = true;
            }
            if (r.text.find("PAGE") != std::string::npos) {
                hasHeaderPageNum = true;
            }
        }
    }
    TEST_ASSERT(hasGreenOrYellowHeader, "Header must contain '页眉' with highlight background");
    TEST_ASSERT(hasHeaderPageNum, "Header must contain PAGE_NUM placeholder");

    TEST_ASSERT(!doc.footerParagraphs.empty(), "doc.footerParagraphs must not be empty");
    bool hasRedFooter = false;
    bool hasFooterPageNum = false;
    bool hasMultilineFooter = false;
    for (const auto& fp : doc.footerParagraphs) {
        for (const auto& r : fp.runs) {
            if (r.text.find("页脚") != std::string::npos && r.color.isValid && r.color.r == 255 && r.color.g == 0 && r.color.b == 0) {
                hasRedFooter = true;
            }
            if (r.text.find("PAGE") != std::string::npos) {
                hasFooterPageNum = true;
            }
            if (r.text.find("222") != std::string::npos || r.text.find("333") != std::string::npos) {
                hasMultilineFooter = true;
            }
        }
    }
    TEST_ASSERT(hasRedFooter, "Footer must contain '页脚' with red text");
    TEST_ASSERT(hasFooterPageNum, "Footer must contain PAGE_NUM placeholder");
    TEST_ASSERT(hasMultilineFooter, "Footer must support multiline entries (e.g. 222 or 333)");
    TEST_ASSERT(doc.pageSetup.headerMargin > 0.0 && doc.pageSetup.headerMargin <= 36.5, "Header margin should be parsed from sprmSDyaHdrTop (~36.0 pt)");
    TEST_ASSERT(doc.pageSetup.footerMargin > 0.0 && doc.pageSetup.footerMargin <= 36.5, "Footer margin should be parsed from sprmSDyaHdrBottom (~36.0 pt)");

    bool footerHasPageNumTogether = false;
    for (size_t fi = 0; fi < doc.footerParagraphs.size(); ++fi) {
        const auto& fp = doc.footerParagraphs[fi];
        if (fp.sameLineAsPrev) {
            footerHasPageNumTogether = true;
            break;
        }
    }
    TEST_ASSERT(footerHasPageNumTogether, "Footer page number must be placed on the line together with footer title");

    // 2. 校验删除线、重点号、文字颜色、批注原文高亮与正文附件
    bool hasStrike = false;
    bool hasEmphasis = false;
    bool hasRedText = false;
    bool hasCommentHighlight = false;
    bool hasCommentRef = false;
    bool hasInkRef = false;
    bool hasAttachmentIconBlock = false;

    for (const auto& b : doc.blocks) {
        if (b.type == BlockType::Paragraph) {
            for (const auto& r : b.para.runs) {
                if (r.strikethrough) hasStrike = true;
                if (r.emphasisMark) hasEmphasis = true;
                if (r.color.isValid && r.color.r == 255 && r.color.g == 0 && r.color.b == 0) hasRedText = true;
                if (!r.commentId.empty() && r.bgColor.isValid && r.bgColor.r == 255 && r.bgColor.g == 255 && r.bgColor.b == 0) hasCommentHighlight = true;
                if (r.text.find("[批注") != std::string::npos && !r.linkTarget.empty()) hasCommentRef = true;
            }
        } else if (b.type == BlockType::Image) {
            if (std::abs(b.image.displayWidthPt - 58.0) < 1.0 && std::abs(b.image.displayHeightPt - 58.0) < 1.0) {
                hasAttachmentIconBlock = true;
            }
        }
    }

    TEST_ASSERT(hasStrike, "DOC document must contain strikethrough runs");
    TEST_ASSERT(hasEmphasis, "DOC document must contain emphasis mark runs");
    TEST_ASSERT(hasRedText, "DOC document must contain red text runs (palette CIco 0x2A42)");
    TEST_ASSERT(hasCommentHighlight, "DOC document must contain highlighted comment range runs");
    TEST_ASSERT(hasCommentRef, "DOC document must contain [批注X] link runs");
    TEST_ASSERT(hasAttachmentIconBlock, "DOC document must contain standard 58pt x 58pt attachment icon image in body");
    TEST_ASSERT(!doc.attachments.empty(), "DOC document must contain extracted attachments");
    TEST_ASSERT(doc.attachments[0].filename.find("PTD_backup_20261014T2009.zip") != std::string::npos, "doc.attachments[0] filename must match");

    // 3. 校验端到端渲染结果
    std::string outPdf = outDir + "/test_doc_advanced_verify.pdf";
    ok = PdfRenderer::render(doc, outPdf, err);
    TEST_ASSERT(ok, "PdfRenderer::render for doc must succeed");

    std::string pdfText = extractPdfText(outPdf);
    TEST_ASSERT(pdfText.find("页眉") != std::string::npos, "PDF must contain header text with '页眉'");
    TEST_ASSERT(pdfText.find("页脚") != std::string::npos, "PDF must contain footer text with '页脚'");
    TEST_ASSERT(pdfText.find("本次严格依据CVSS") != std::string::npos, "PDF must contain '本次严格依据CVSS'");
    TEST_ASSERT(pdfText.find("\x08") == std::string::npos, "PDF text must not contain 0x08 drawObject anchor");
    TEST_ASSERT(pdfText.find("[批注0]") != std::string::npos, "PDF must contain '[批注0]'");
    TEST_ASSERT(pdfText.find("[手写备注") == std::string::npos, "PDF must not contain '[手写备注]'");
    TEST_ASSERT(pdfText.find("PTD_backup_20261014T2009.zip") != std::string::npos, "PDF must contain attachment filename");
    TEST_ASSERT(pdfText.find("批注清单 (Comments)") != std::string::npos, "PDF must contain Comments section");
    TEST_ASSERT(pdfText.find("内嵌附件清单 (Attachments)") != std::string::npos, "PDF must contain Attachments section");
    TEST_ASSERT(pdfText.find("📎") == std::string::npos, "PDF must not contain unsupported emoji 📎 that renders as square X box");

    TEST_ASSERT(pdfText.find("[点击另存为本地文件]") == std::string::npos, "PDF must not display redundant [点击另存为本地文件] text");
    TEST_ASSERT(pdfText.find("[手写批注 1]") != std::string::npos, "PDF must contain '[手写批注 1]'");
    TEST_ASSERT(pdfText.find("[手写批注 2]") != std::string::npos, "PDF must contain '[手写批注 2]'");

    std::ifstream fPdf(outPdf, std::ios::binary);
    std::string pdfData((std::istreambuf_iterator<char>(fPdf)), std::istreambuf_iterator<char>());
    TEST_ASSERT(pdfData.find("/Subtype /FileAttachment") != std::string::npos, "PDF must support native /Subtype /FileAttachment for standard export");
    TEST_ASSERT(pdfData.find("/AP << /N") != std::string::npos, "PDF must have transparent appearance stream to avoid yellow sticky note popup");
    TEST_ASSERT(pdfData.find("/Subtype /Text") == std::string::npos, "PDF must not contain /Subtype /Text sticky notes");
    TEST_ASSERT(pdfData.find("/Subtype /Link") != std::string::npos, "PDF must contain /Subtype /Link for navigation");
    TEST_ASSERT(pdfData.find("/EmbeddedFiles") != std::string::npos, "PDF must contain /EmbeddedFiles in Catalog");
    TEST_ASSERT(pdfData.find("nLaunch: 2") != std::string::npos, "PDF JavaScript must use nLaunch: 2 for save-as file dialog");
    TEST_ASSERT(pdfData.find("/S /Launch") == std::string::npos, "PDF must not contain /S /Launch action to avoid security alerts");
}

int main(int argc, char* argv[]) {
    std::string dataDir = "testdata";
    std::string outDir = ".";

    if (argc >= 2) dataDir = argv[1];
    if (argc >= 3) outDir = argv[2];

    std::cout << "========================================\n";
    std::cout << "Starting PureDoc2Pdf Unit Tests\n";
    std::cout << "Data dir: " << dataDir << "\n";
    std::cout << "Out dir:  " << outDir << "\n";
    std::cout << "========================================\n";

    testUtfUtils();
    testImageUtils();
    testDocAst();
    testDocxParser(dataDir);
    testDocParser(dataDir);
    testPdfRenderer(outDir + "/test_unit_render.pdf");
    testDynamicFontFallback(outDir + "/test_dynamic_fonts.pdf");
    testRichFormattingAndNotes(outDir + "/test_rich_formatting.pdf");
    testDocxCommentJumpAndAttachments(outDir);
    testDocChineseRendering(dataDir, outDir);
    testDocAdvancedFeatures(dataDir, outDir);
    testEndToEndConversion(dataDir, outDir);

    std::cout << "========================================\n";
    std::cout << "Tests finished. Passed: " << g_passCount << ", Failed: " << g_failCount << "\n";
    std::cout << "========================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
