#ifndef DOC_AST_HPP
#define DOC_AST_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace doc2pdf {

enum class Alignment { Left, Center, Right, Justify };

struct ColorRGB {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    bool isValid = false;

    ColorRGB() = default;
    ColorRGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_), isValid(true) {}

    bool operator==(const ColorRGB& o) const {
        return isValid == o.isValid && r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const ColorRGB& o) const {
        return !(*this == o);
    }

    static ColorRGB fromHex(const std::string& hex) {
        if (hex == "auto" || hex.empty()) return ColorRGB();
        if (hex.size() >= 6) {
            ColorRGB c;
            c.r = (uint8_t)std::stoul(hex.substr(0, 2), nullptr, 16);
            c.g = (uint8_t)std::stoul(hex.substr(2, 2), nullptr, 16);
            c.b = (uint8_t)std::stoul(hex.substr(4, 2), nullptr, 16);
            c.isValid = true;
            return c;
        }
        return ColorRGB();
    }

    static ColorRGB fromNamed(const std::string& name) {
        if (name == "yellow") return ColorRGB(255, 255, 0);
        if (name == "green") return ColorRGB(0, 255, 0);
        if (name == "cyan") return ColorRGB(0, 255, 255);
        if (name == "magenta") return ColorRGB(255, 0, 255);
        if (name == "blue") return ColorRGB(0, 0, 255);
        if (name == "red") return ColorRGB(255, 0, 0);
        if (name == "darkBlue") return ColorRGB(0, 0, 139);
        if (name == "darkCyan") return ColorRGB(0, 139, 139);
        if (name == "darkGreen") return ColorRGB(0, 100, 0);
        if (name == "darkMagenta") return ColorRGB(139, 0, 139);
        if (name == "darkRed") return ColorRGB(139, 0, 0);
        if (name == "darkYellow") return ColorRGB(128, 128, 0);
        if (name == "darkGray") return ColorRGB(169, 169, 169);
        if (name == "lightGray") return ColorRGB(211, 211, 211);
        if (name == "black") return ColorRGB(0, 0, 0);
        if (name == "white") return ColorRGB(255, 255, 255);
        return ColorRGB();
    }
};

struct TextRun {
    std::string text;
    std::string fontFamily;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool doubleUnderline = false;
    bool strikethrough = false;
    bool doubleStrikethrough = false;
    bool emphasisMark = false; // 重点号 (dot above/below)
    double fontSize = 11.0; // pt
    ColorRGB color;
    ColorRGB bgColor; // 高亮/底色
    std::string commentId;  // 关联的批注 ID (若非空，代表属于被批注范围)
    std::string linkTarget; // 内部跳转目标 (例如 "comment_0" 或外部 URL)

    TextRun() = default;
    TextRun(std::string t, bool b = false, bool i = false, double sz = 11.0, ColorRGB c = ColorRGB(), ColorRGB bg = ColorRGB(), std::string ff = "")
        : text(std::move(t)), fontFamily(std::move(ff)), bold(b), italic(i), fontSize(sz), color(c), bgColor(bg) {}
};

struct Paragraph {
    std::vector<TextRun> runs;
    int headingLevel = 0; // 0=正文, 1=H1, 2=H2, 3=H3
    Alignment align = Alignment::Left;
    double spaceBefore = 0.0;     // pt (来源于 w:spacing w:before)
    double spaceAfter = 4.0;      // pt (来源于 w:spacing w:after)
    double lineSpacing = 1.35;    // 行倍距 (来源于 w:spacing w:line)
    double leftIndent = 0.0;      // pt (来源于 w:ind w:left)
    double rightIndent = 0.0;     // pt (来源于 w:ind w:right)
    double firstLineIndent = 0.0; // pt (来源于 w:ind w:firstLine 首行缩进)
    ColorRGB bgColor;             // 段落背景色
    bool pageBreakBefore = false; // 是否段前强制分页
    bool sameLineAsPrev = false;  // 与前一段落同行并列渲染（如浮动文本框与正文同行）
    bool keepNext = false;        // 与下段同页 (w:keepNext)
    double emptyLineHeight = 0.0; // 空段落的名义行高 (来源于 pPr 字符大小与行距)
};

struct BorderStyle {
    bool exists = true;
    double widthPt = 0.5;
    ColorRGB color = ColorRGB(76, 76, 76);
};

struct TableCell {
    std::vector<Paragraph> paragraphs;
    double widthPt = 0.0;
    ColorRGB bgColor;
    double padTop = 1.0;
    double padBottom = 1.0;
    double padLeft = 5.4;
    double padRight = 5.4;
    BorderStyle borderTop;
    BorderStyle borderBottom;
    BorderStyle borderLeft;
    BorderStyle borderRight;
};

struct TableRow {
    std::vector<TableCell> cells;
    bool isHeader = false;
};

struct Table {
    std::vector<TableRow> rows;
    std::vector<double> gridColWidthsPt;
    double tableIndent = 0.0;
};

struct ImageBlock {
    std::vector<uint8_t> data;
    std::string format; // "jpeg" or "png"
    int pixelWidth = 0;
    int pixelHeight = 0;
    double displayWidthPt = 0.0;
    double displayHeightPt = 0.0;

    ImageBlock() = default;
    ImageBlock(std::vector<uint8_t> d, std::string fmt, int pw = 0, int ph = 0, double dw = 0.0, double dh = 0.0)
        : data(std::move(d)), format(std::move(fmt)), pixelWidth(pw), pixelHeight(ph), displayWidthPt(dw), displayHeightPt(dh) {}
};

enum class BlockType { Paragraph, Table, Image };

struct Block {
    BlockType type = BlockType::Paragraph;
    Paragraph para;
    Table table;
    ImageBlock image;

    static Block makeParagraph(Paragraph p) {
        Block b;
        b.type = BlockType::Paragraph;
        b.para = std::move(p);
        return b;
    }

    static Block makeTable(Table t) {
        Block b;
        b.type = BlockType::Table;
        b.table = std::move(t);
        return b;
    }

    static Block makeImage(ImageBlock img) {
        Block b;
        b.type = BlockType::Image;
        b.image = std::move(img);
        return b;
    }
};

struct PageSetup {
    double pageWidth = 595.28;
    double pageHeight = 841.89;
    double marginTop = 54.0;
    double marginBottom = 54.0;
    double marginLeft = 54.0;
    double marginRight = 54.0;
    double headerMargin = 36.0;
    double footerMargin = 36.0;
    double linePitch = 0.0;
};

struct DocFontDecl {
    std::string name;
    std::string altName;
};

struct FootnoteItem {
    std::string id;
    std::vector<Paragraph> paragraphs;
};

struct CommentItem {
    std::string id;
    std::string author;
    std::string date;
    std::vector<Paragraph> paragraphs;
};

struct AttachmentItem {
    std::string filename;
    std::string description;
    std::vector<uint8_t> data;
    std::string mimeType;

    AttachmentItem() = default;
    AttachmentItem(std::string fn, std::vector<uint8_t> d, std::string desc = "", std::string mime = "application/octet-stream")
        : filename(std::move(fn)), description(std::move(desc)), data(std::move(d)), mimeType(std::move(mime)) {}
};

struct Document {
    std::string title;
    std::vector<Block> blocks;
    PageSetup pageSetup;
    std::vector<Paragraph> headerParagraphs;
    std::vector<Paragraph> footerParagraphs;
    std::vector<DocFontDecl> fontDeclarations;
    std::map<std::string, FootnoteItem> footnotes;
    std::vector<CommentItem> comments;
    std::vector<AttachmentItem> attachments;
};

} // namespace doc2pdf

#endif // DOC_AST_HPP
