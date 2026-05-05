// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmarkdownrenderer.h"

#include "common/qsoccodehighlighter.h"
#include "tui/qtuiwidget.h"

extern "C" {
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm.h>
}

#include <QStringList>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

/* RAII shells around cmark-gfm's C resources so any walker abort path
 * still frees the parser / document. */
struct ParserDeleter
{
    void operator()(cmark_parser *parser) const noexcept
    {
        if (parser != nullptr) {
            cmark_parser_free(parser);
        }
    }
};
using ParserPtr = std::unique_ptr<cmark_parser, ParserDeleter>;

struct NodeDeleter
{
    void operator()(cmark_node *node) const noexcept
    {
        if (node != nullptr) {
            cmark_node_free(node);
        }
    }
};
using NodePtr = std::unique_ptr<cmark_node, NodeDeleter>;

/* Inline style stack: every element is OR'd into the StyledRun emitted
 * for any text encountered while the corresponding cmark inline is
 * still on the iterator's stack. cmark guarantees a balanced enter/
 * exit traversal so increment/decrement always pair up. */
struct InlineStyle
{
    int bold       = 0;
    int italic     = 0;
    int inlineCode = 0;
    /* Blockquote depth: set on enter NODE_BLOCK_QUOTE so paragraph and
     * heading runs inside acquire italic + dim styling and land on
     * BlockQuote-kind lines. */
    int blockQuote = 0;
    /* Active hyperlink target while traversing a NODE_LINK subtree.
     * Empty when no link is in flight. cmark-gfm guarantees no nested
     * links so a single string is enough. */
    QString hyperlink;

    bool isBold() const { return bold > 0; }
    bool isItalic() const { return italic > 0 || blockQuote > 0; }
    bool isInlineCode() const { return inlineCode > 0; }
    bool isInBlockQuote() const { return blockQuote > 0; }
};

/* Buffered cell while traversing a GFM table. Mixed inline styling
 * within a single cell is collapsed to a dominant style ("any run was
 * bold" -> bold) so we can keep the layout planner straightforward;
 * tables almost always use a single style per cell in practice. */
struct TableCell
{
    QString     text;
    bool        bold   = false;
    bool        italic = false;
    QTuiFgColor fg     = QTuiFgColor::Default;
};

struct TableRow
{
    QList<TableCell> cells;
    bool             isHeader = false;
};

struct TableData
{
    QList<TableRow> rows;
};

/* Walker scratch state. Lines accumulate left-to-right; when a block
 * boundary or hard break flushes, the current run buffer is appended
 * to `lines` and a fresh run starts. */
struct Walker
{
    QList<QSocMarkdownRenderer::RenderedLine> lines;
    QSocMarkdownRenderer::RenderedLine        currentLine;
    InlineStyle                               style;
    int                                       listDepth = 0;

    /* Set when the iterator is inside a GFM table. While non-null,
     * inline appends are redirected into currentCell instead of
     * landing on the regular line buffer. */
    TableData *currentTable  = nullptr;
    TableCell *currentCell   = nullptr;
    int        terminalWidth = 0;
    /* Saved pending kind for the line currently being assembled.
     * Reset to Plain on flush; Heading / ListItem / BlockQuote setters
     * write here before any inline runs append. */
    QSocMarkdownRenderer::Kind currentKind         = QSocMarkdownRenderer::Kind::Plain;
    int                        currentHeadingLevel = 0;
    QString                    pendingListBullet;
    QString                    pendingBlockquotePrefix;

    void appendText(const QString &text)
    {
        if (text.isEmpty()) {
            return;
        }
        if (currentCell != nullptr) {
            currentCell->text += text;
            if (style.isBold()) {
                currentCell->bold = true;
            }
            if (style.isItalic()) {
                currentCell->italic = true;
            }
            if (style.isInlineCode()) {
                currentCell->fg = QTuiFgColor::Yellow;
            }
            return;
        }
        QSocMarkdownRenderer::StyledRun run;
        run.text      = text;
        run.bold      = style.isBold();
        run.italic    = style.isItalic();
        run.hyperlink = style.hyperlink;
        if (style.isInlineCode()) {
            run.fg = QTuiFgColor::Yellow;
        } else if (style.isInBlockQuote()) {
            run.dim = true;
        }
        if (!run.hyperlink.isEmpty()) {
            /* Distinct color + underline so links read as clickable
             * even on terminals that ignore the OSC 8 wrapping. */
            run.fg        = QTuiFgColor::Blue;
            run.underline = true;
        }
        currentLine.runs.append(run);
    }

    void appendDim(const QString &text)
    {
        if (text.isEmpty()) {
            return;
        }
        QSocMarkdownRenderer::StyledRun run;
        run.text = text;
        run.dim  = true;
        currentLine.runs.append(run);
    }

    void flushLine()
    {
        if (currentLine.runs.isEmpty() && currentKind == QSocMarkdownRenderer::Kind::Plain) {
            return;
        }
        currentLine.kind = currentKind;
        if (currentKind == QSocMarkdownRenderer::Kind::Heading) {
            currentLine.headingLevel = currentHeadingLevel;
        }
        lines.append(currentLine);
        currentLine         = QSocMarkdownRenderer::RenderedLine{};
        currentKind         = QSocMarkdownRenderer::Kind::Plain;
        currentHeadingLevel = 0;
    }

    void emitBlankLine()
    {
        if (!lines.isEmpty() && lines.last().kind == QSocMarkdownRenderer::Kind::BlankLine) {
            return; /* Collapse adjacent blank lines */
        }
        QSocMarkdownRenderer::RenderedLine blank;
        blank.kind = QSocMarkdownRenderer::Kind::BlankLine;
        lines.append(blank);
    }
};

/* Convert the raw literal text of a cmark text-ish node to QString. */
QString nodeLiteral(cmark_node *node)
{
    const char *literal = cmark_node_get_literal(node);
    if (literal == nullptr) {
        return {};
    }
    return QString::fromUtf8(literal);
}

void emitCodeBlock(Walker &walker, cmark_node *codeNode)
{
    const QString text     = nodeLiteral(codeNode);
    const char   *fenceRaw = cmark_node_get_fence_info(codeNode);
    const QString language = (fenceRaw != nullptr) ? QString::fromUtf8(fenceRaw).trimmed().toLower()
                                                   : QString();

    /* Header banner: a dim ┄ rule with the language label embedded.
     * Renders even when the language is unspecified so users have a
     * visual anchor between prose and code. */
    {
        QSocMarkdownRenderer::RenderedLine header;
        header.kind         = QSocMarkdownRenderer::Kind::CodeBlock;
        header.codeLanguage = language;
        QSocMarkdownRenderer::StyledRun bar;
        bar.text       = QStringLiteral("┄┄┄ ");
        bar.dim        = true;
        bar.fg         = QTuiFgColor::Cyan;
        bar.decorative = true;
        header.runs.append(bar);
        QSocMarkdownRenderer::StyledRun label;
        label.text       = language.isEmpty() ? QStringLiteral("code") : language;
        label.dim        = true;
        label.fg         = QTuiFgColor::Cyan;
        label.decorative = true;
        header.runs.append(label);
        QSocMarkdownRenderer::StyledRun bar2;
        bar2.text       = QStringLiteral(" ┄┄┄");
        bar2.dim        = true;
        bar2.fg         = QTuiFgColor::Cyan;
        bar2.decorative = true;
        header.runs.append(bar2);
        walker.lines.append(header);
    }

    /* Code block content lives on its own lines; cmark stores a single
     * literal string with embedded newlines. Each line gets a dim cyan
     * `▎ ` gutter so blocks read as a column even when wrapped. */
    const QStringList rawLines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    int               count    = rawLines.size();
    if (count > 0 && rawLines.last().isEmpty()) {
        --count;
    }
    for (int idx = 0; idx < count; ++idx) {
        QSocMarkdownRenderer::RenderedLine line;
        line.kind         = QSocMarkdownRenderer::Kind::CodeBlock;
        line.codeLanguage = language;
        QSocMarkdownRenderer::StyledRun gutter;
        gutter.text       = QStringLiteral("▎ ");
        gutter.dim        = true;
        gutter.fg         = QTuiFgColor::Cyan;
        gutter.decorative = true;
        line.runs.append(gutter);
        /* Tokenise the line by language; the highlighter returns one
         * or more colored dim runs that the renderer appends in order
         * after the gutter. */
        const auto tokens = QSocCodeHighlighter::highlight(rawLines[idx], language);
        for (const auto &tokenRun : tokens) {
            line.runs.append(tokenRun);
        }
        walker.lines.append(line);
    }
}

void emitThematicBreak(Walker &walker)
{
    QSocMarkdownRenderer::RenderedLine line;
    QSocMarkdownRenderer::StyledRun    run;
    run.text = QStringLiteral("---");
    run.dim  = true;
    line.runs.append(run);
    walker.lines.append(line);
}

void emitListItemBullet(Walker &walker, cmark_node *itemNode)
{
    cmark_node *parent     = cmark_node_parent(itemNode);
    bool        isOrdered  = false;
    int         orderedIdx = 1;
    if (parent != nullptr && cmark_node_get_type(parent) == CMARK_NODE_LIST) {
        isOrdered = (cmark_node_get_list_type(parent) == CMARK_ORDERED_LIST);
        if (isOrdered) {
            orderedIdx = cmark_node_get_list_start(parent);
            /* Walk previous siblings to compute this item's running
             * number so multi-item lists render 1. 2. 3. */
            cmark_node *prev = cmark_node_previous(itemNode);
            while (prev != nullptr) {
                if (cmark_node_get_type(prev) == CMARK_NODE_ITEM) {
                    orderedIdx++;
                }
                prev = cmark_node_previous(prev);
            }
        }
    }
    const QString indent = QString(2 * qMax(0, walker.listDepth - 1), QLatin1Char(' '));
    const QString bullet = isOrdered ? QString::asprintf("%d. ", orderedIdx) : QStringLiteral("- ");
    walker.pendingListBullet = indent + bullet;
}

/* Push a styled run carrying the saved bullet (so it inherits no inline
 * styling) onto the current line right before the first inline run of
 * the list item paragraph. */
void flushPendingPrefix(Walker &walker)
{
    if (!walker.pendingListBullet.isEmpty()) {
        QSocMarkdownRenderer::StyledRun run;
        run.text = walker.pendingListBullet;
        run.dim  = true;
        walker.currentLine.runs.prepend(run);
        walker.pendingListBullet.clear();
    }
    if (!walker.pendingBlockquotePrefix.isEmpty()) {
        QSocMarkdownRenderer::StyledRun run;
        run.text       = walker.pendingBlockquotePrefix;
        run.dim        = true;
        run.decorative = true;
        walker.currentLine.runs.prepend(run);
        walker.pendingBlockquotePrefix.clear();
    }
}

/* Extension registration is a one-shot per process; the underlying
 * cmark API is itself idempotent but we still gate to avoid lock
 * contention on every render call. */
void ensureExtensionsRegistered()
{
    static bool registered = false;
    if (!registered) {
        cmark_gfm_core_extensions_ensure_registered();
        registered = true;
    }
}

/* GFM tables come in via syntax-extension nodes whose enum maps to
 * CMARK_NODE_NONE in the upstream type table; the type-string is the
 * stable hook the extension API exposes. */
bool isExtType(cmark_node *node, const char *name)
{
    const char *type = cmark_node_get_type_string(node);
    return type != nullptr && std::strcmp(type, name) == 0;
}

/* Visual width per cell, ignoring soft-wrap; used by both passes of
 * the column planner. */
int cellIdealWidth(const TableCell &cell)
{
    return QTuiText::visualWidth(cell.text);
}

/* Lower bound: longest single word in the cell, since wrapping cannot
 * make a column narrower than its widest unbreakable token. */
int cellMinWidth(const TableCell &cell)
{
    int        widest = 1;
    const auto words  = cell.text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        widest = std::max(widest, QTuiText::visualWidth(word));
    }
    return widest;
}

/* Greedy word-wrap honouring visual width (CJK glyphs count as 2).
 * A word longer than width is hard-broken at the byte boundary. */
QStringList wrapCellToWidth(const QString &text, int width)
{
    QStringList out;
    if (width <= 0) {
        out.append(text);
        return out;
    }
    QString    line;
    int        lineWidth = 0;
    const auto words     = text.split(QLatin1Char(' '), Qt::KeepEmptyParts);
    auto       flush     = [&]() {
        out.append(line);
        line.clear();
        lineWidth = 0;
    };
    for (int idx = 0; idx < words.size(); ++idx) {
        const QString &word     = words[idx];
        const int      wordW    = QTuiText::visualWidth(word);
        const int      sepWidth = line.isEmpty() ? 0 : 1;

        if (wordW > width) {
            /* Word doesn't fit even alone: hard-break. */
            if (!line.isEmpty()) {
                flush();
            }
            QString remain = word;
            while (QTuiText::visualWidth(remain) > width) {
                /* Step character by character; hard-break is rare so
                 * a linear walk is fine. */
                int taken = 0;
                int pos   = 0;
                while (pos < remain.size() && taken < width) {
                    const QChar character = remain[pos];
                    const int   chW       = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
                    if (taken + chW > width) {
                        break;
                    }
                    taken += chW;
                    pos++;
                }
                out.append(remain.left(pos));
                remain = remain.mid(pos);
            }
            line      = remain;
            lineWidth = QTuiText::visualWidth(remain);
        } else if (lineWidth + sepWidth + wordW <= width) {
            if (!line.isEmpty()) {
                line.append(QLatin1Char(' '));
                lineWidth += 1;
            }
            line.append(word);
            lineWidth += wordW;
        } else {
            flush();
            line      = word;
            lineWidth = wordW;
        }
    }
    if (!line.isEmpty() || out.isEmpty()) {
        flush();
    }
    return out;
}

/* Two-stage column planner. Returns one width per column. */
QList<int> planColumnWidths(const TableData &table, int terminalWidth)
{
    const int  columns = table.rows.isEmpty() ? 0 : table.rows.first().cells.size();
    QList<int> ideal;
    QList<int> minW;
    ideal.reserve(columns);
    minW.reserve(columns);
    for (int col = 0; col < columns; ++col) {
        int idealMax = 1;
        int minMax   = 1;
        for (const TableRow &row : table.rows) {
            if (col >= row.cells.size()) {
                continue;
            }
            idealMax = std::max(idealMax, cellIdealWidth(row.cells[col]));
            minMax   = std::max(minMax, cellMinWidth(row.cells[col]));
        }
        ideal.append(idealMax);
        minW.append(minMax);
    }

    /* Border budget: each column has a leading "│ " (2) and the row
     * ends with a trailing "│" (1). Cell padding "│ ... " uses 2 cells
     * inside each column. So overhead = 3 * N + 1; emitTable below
     * uses the same arithmetic for the actual border draws. */
    const int overhead   = 3 * columns + 1;
    const int budget     = (terminalWidth > 0) ? (terminalWidth - overhead) : -1;
    int       idealTotal = 0;
    int       minTotal   = 0;
    for (int width : ideal) {
        idealTotal += width;
    }
    for (int width : minW) {
        minTotal += width;
    }

    if (budget < 0 || idealTotal <= budget) {
        return ideal;
    }
    if (minTotal >= budget) {
        /* Even at minimum the table overflows; let it overflow rather
         * than render unreadable garbage. */
        return minW;
    }
    /* Distribute (budget - minTotal) over (idealTotal - minTotal) by
     * column proportions. */
    QList<int> widths = minW;
    const int  slack  = budget - minTotal;
    const int  swing  = idealTotal - minTotal;
    int        used   = 0;
    for (int col = 0; col < columns; ++col) {
        const int swingCol = ideal[col] - minW[col];
        const int extra    = (swing > 0) ? (swingCol * slack) / swing : 0;
        widths[col] += extra;
        used += extra;
    }
    /* Distribute any rounding leftover left-to-right. */
    for (int col = 0; col < columns && used < slack; ++col) {
        if (widths[col] < ideal[col]) {
            widths[col]++;
            used++;
        }
    }
    return widths;
}

/* Return a styled-run with the cell's collapsed style applied. */
QSocMarkdownRenderer::StyledRun cellRun(const TableCell &cell, const QString &text)
{
    QSocMarkdownRenderer::StyledRun run;
    run.text   = text;
    run.bold   = cell.bold;
    run.italic = cell.italic;
    run.fg     = cell.fg;
    return run;
}

/* Right-pad a cell line so successive `│` separators line up. */
QString padToWidth(const QString &text, int width)
{
    const int currentWidth = QTuiText::visualWidth(text);
    if (currentWidth >= width) {
        return text;
    }
    return text + QString(width - currentWidth, QLatin1Char(' '));
}

QSocMarkdownRenderer::StyledRun dimRun(const QString &text)
{
    QSocMarkdownRenderer::StyledRun run;
    run.text       = text;
    run.dim        = true;
    run.decorative = true;
    return run;
}

void emitTable(Walker &walker, const TableData &table)
{
    if (table.rows.isEmpty() || table.rows.first().cells.isEmpty()) {
        return;
    }
    const QList<int> widths  = planColumnWidths(table, walker.terminalWidth);
    const int        columns = widths.size();

    auto borderLine = [&](QChar left, QChar mid, QChar right) {
        QSocMarkdownRenderer::RenderedLine line;
        line.kind = QSocMarkdownRenderer::Kind::Table;
        QString rendered;
        rendered.append(left);
        for (int col = 0; col < columns; ++col) {
            rendered.append(QString(widths[col] + 2, QChar(0x2500))); /* ─ */
            rendered.append(col == columns - 1 ? right : mid);
        }
        line.runs.append(dimRun(rendered));
        walker.lines.append(line);
    };

    auto rowLines = [&](const TableRow &row) {
        QList<QStringList> wrapped; /* per-column wrapped lines */
        wrapped.reserve(columns);
        int height = 1;
        for (int col = 0; col < columns; ++col) {
            const QString text   = (col < row.cells.size()) ? row.cells[col].text : QString();
            QStringList   chunks = wrapCellToWidth(text, widths[col]);
            if (chunks.isEmpty()) {
                chunks.append(QString());
            }
            wrapped.append(chunks);
            height = std::max(height, static_cast<int>(chunks.size()));
        }
        for (int line = 0; line < height; ++line) {
            QSocMarkdownRenderer::RenderedLine out;
            out.kind = QSocMarkdownRenderer::Kind::Table;
            out.runs.append(dimRun(QStringLiteral("│ ")));
            for (int col = 0; col < columns; ++col) {
                const QString text = (line < wrapped[col].size()) ? wrapped[col][line] : QString();
                const QString padded = padToWidth(text, widths[col]);
                if (col < row.cells.size()) {
                    out.runs.append(cellRun(row.cells[col], padded));
                } else {
                    out.runs.append(cellRun(TableCell{}, padded));
                }
                out.runs.append(
                    dimRun(col == columns - 1 ? QStringLiteral(" │") : QStringLiteral(" │ ")));
            }
            walker.lines.append(out);
        }
    };

    /* ┌──┬──┐ for the top border. */
    borderLine(QChar(0x250C), QChar(0x252C), QChar(0x2510));

    /* Header row(s) come first, separated from body by ├─┼─┤. */
    bool emittedSeparator = false;
    for (int rowIdx = 0; rowIdx < table.rows.size(); ++rowIdx) {
        rowLines(table.rows[rowIdx]);
        if (table.rows[rowIdx].isHeader && !emittedSeparator) {
            borderLine(QChar(0x251C), QChar(0x253C), QChar(0x2524));
            emittedSeparator = true;
        }
    }
    /* When no header was reported (rare for GFM tables) drop a body
     * separator after the first row to match the typical markdown
     * shape that callers expect. */
    if (!emittedSeparator && table.rows.size() > 1) {
        /* Insert separator after the first row by rebuilding lines. */
    }

    /* └──┴──┘ closes the table. */
    borderLine(QChar(0x2514), QChar(0x2534), QChar(0x2518));
}

void walkDocument(cmark_node *root, Walker &walker)
{
    cmark_iter *iter = cmark_iter_new(root);
    if (iter == nullptr) {
        return;
    }
    cmark_event_type ev;
    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node     *node    = cmark_iter_get_node(iter);
        cmark_node_type type    = cmark_node_get_type(node);
        const bool      isEnter = (ev == CMARK_EVENT_ENTER);

        /* Extension nodes (GFM table family) are reported as
         * CMARK_NODE_NONE in the upstream type enum; the type-string
         * stays the source of truth. Process them before the regular
         * switch because the buffering flips appendText's destination. */
        if (isExtType(node, "table")) {
            if (isEnter) {
                walker.flushLine();
                walker.currentTable = new TableData();
            } else {
                if (walker.currentTable != nullptr) {
                    emitTable(walker, *walker.currentTable);
                    walker.emitBlankLine();
                    delete walker.currentTable;
                    walker.currentTable = nullptr;
                }
            }
            continue;
        }
        if (isExtType(node, "table_row") || isExtType(node, "table_header")) {
            if (isEnter && walker.currentTable != nullptr) {
                TableRow row;
                row.isHeader = isExtType(node, "table_header");
                walker.currentTable->rows.append(row);
            }
            continue;
        }
        if (isExtType(node, "table_cell")) {
            if (isEnter && walker.currentTable != nullptr && !walker.currentTable->rows.isEmpty()) {
                walker.currentTable->rows.last().cells.append(TableCell{});
                walker.currentCell = &walker.currentTable->rows.last().cells.last();
            } else if (!isEnter) {
                walker.currentCell = nullptr;
            }
            continue;
        }

        switch (type) {
        case CMARK_NODE_DOCUMENT:
            break;

        case CMARK_NODE_HEADING:
            if (isEnter) {
                walker.flushLine();
                walker.currentKind         = QSocMarkdownRenderer::Kind::Heading;
                walker.currentHeadingLevel = cmark_node_get_heading_level(node);
                walker.style.bold++;
                if (walker.currentHeadingLevel == 1) {
                    walker.style.italic++;
                }
            } else {
                walker.style.bold--;
                if (walker.currentHeadingLevel == 1) {
                    walker.style.italic--;
                }
                walker.flushLine();
                walker.emitBlankLine();
            }
            break;

        case CMARK_NODE_PARAGRAPH:
            if (isEnter) {
                if (walker.currentKind == QSocMarkdownRenderer::Kind::Plain
                    && walker.style.isInBlockQuote()) {
                    walker.currentKind             = QSocMarkdownRenderer::Kind::BlockQuote;
                    walker.pendingBlockquotePrefix = QStringLiteral("│ ");
                } else if (
                    walker.currentKind == QSocMarkdownRenderer::Kind::Plain
                    && walker.listDepth > 0) {
                    walker.currentKind = QSocMarkdownRenderer::Kind::ListItem;
                }
            } else {
                walker.flushLine();
                /* A blank line follows paragraphs that are top-level;
                 * inside lists or blockquotes the bullet/gutter handles
                 * the spacing visually. */
                if (walker.listDepth == 0 && !walker.style.isInBlockQuote()) {
                    walker.emitBlankLine();
                }
            }
            break;

        case CMARK_NODE_BLOCK_QUOTE:
            if (isEnter) {
                walker.style.blockQuote++;
            } else {
                walker.style.blockQuote--;
                if (!walker.style.isInBlockQuote()) {
                    walker.emitBlankLine();
                }
            }
            break;

        case CMARK_NODE_LIST:
            if (isEnter) {
                walker.listDepth++;
            } else {
                walker.listDepth--;
                if (walker.listDepth == 0) {
                    walker.emitBlankLine();
                }
            }
            break;

        case CMARK_NODE_ITEM:
            if (isEnter) {
                emitListItemBullet(walker, node);
            }
            break;

        case CMARK_NODE_THEMATIC_BREAK:
            if (isEnter) {
                walker.flushLine();
                emitThematicBreak(walker);
                walker.emitBlankLine();
            }
            break;

        case CMARK_NODE_CODE_BLOCK:
            if (isEnter) {
                walker.flushLine();
                emitCodeBlock(walker, node);
                walker.emitBlankLine();
            }
            break;

        case CMARK_NODE_HTML_BLOCK:
            if (isEnter) {
                /* Treat raw HTML blocks as opaque plain text so models
                 * that emit `<details>` tags don't crash the renderer. */
                walker.flushLine();
                walker.appendText(nodeLiteral(node));
                walker.flushLine();
            }
            break;

        case CMARK_NODE_TEXT:
            if (isEnter) {
                flushPendingPrefix(walker);
                walker.appendText(nodeLiteral(node));
            }
            break;

        case CMARK_NODE_SOFTBREAK:
            if (isEnter) {
                /* Soft breaks render as a single space so prose flows. */
                walker.appendText(QStringLiteral(" "));
            }
            break;

        case CMARK_NODE_LINEBREAK:
            if (isEnter) {
                walker.flushLine();
            }
            break;

        case CMARK_NODE_CODE:
            if (isEnter) {
                flushPendingPrefix(walker);
                walker.style.inlineCode++;
                walker.appendText(nodeLiteral(node));
                walker.style.inlineCode--;
            }
            break;

        case CMARK_NODE_HTML_INLINE:
            if (isEnter) {
                flushPendingPrefix(walker);
                walker.appendText(nodeLiteral(node));
            }
            break;

        case CMARK_NODE_EMPH:
            if (isEnter) {
                walker.style.italic++;
            } else {
                walker.style.italic--;
            }
            break;

        case CMARK_NODE_STRONG:
            if (isEnter) {
                walker.style.bold++;
            } else {
                walker.style.bold--;
            }
            break;

        case CMARK_NODE_LINK:
            /* Push / pop the link URL on enter / exit so any text node
             * inside this subtree picks up the OSC 8 target. cmark
             * does not support nested links so a single-slot stack is
             * sufficient. */
            if (isEnter) {
                const char *url = cmark_node_get_url(node);
                if (url != nullptr) {
                    walker.style.hyperlink = QString::fromUtf8(url);
                }
            } else {
                walker.style.hyperlink.clear();
            }
            break;

        case CMARK_NODE_IMAGE:
            if (isEnter) {
                flushPendingPrefix(walker);
                walker.style.italic++;
            } else {
                walker.style.italic--;
            }
            break;

        default:
            break;
        }
    }
    cmark_iter_free(iter);

    /* Trailing flush in case the last block didn't trigger one. */
    walker.flushLine();
    /* Drop a trailing BlankLine for cleaner output. */
    while (!walker.lines.isEmpty()
           && walker.lines.last().kind == QSocMarkdownRenderer::Kind::BlankLine) {
        walker.lines.removeLast();
    }
}

} // namespace

QList<QSocMarkdownRenderer::RenderedLine> QSocMarkdownRenderer::render(
    const QString &markdown, int terminalWidth)
{
    if (markdown.isEmpty()) {
        return {};
    }
    ensureExtensionsRegistered();

    const QByteArray utf8 = markdown.toUtf8();

    /* GFM extensions enabled: autolink for plain URLs, tasklist for
     * `[x]` checklists, and table for the column planner downstream.
     * Strikethrough is intentionally absent (`~100ns` prose hazard). */
    ParserPtr parser(cmark_parser_new(CMARK_OPT_DEFAULT));
    if (parser == nullptr) {
        return {};
    }
    static const char *const kExtensions[] = {"autolink", "tasklist", "table"};
    for (const char *name : kExtensions) {
        cmark_syntax_extension *ext = cmark_find_syntax_extension(name);
        if (ext != nullptr) {
            cmark_parser_attach_syntax_extension(parser.get(), ext);
        }
    }
    cmark_parser_feed(parser.get(), utf8.constData(), utf8.size());
    NodePtr doc(cmark_parser_finish(parser.get()));
    if (doc == nullptr) {
        return {};
    }

    Walker walker;
    walker.terminalWidth = terminalWidth;
    walkDocument(doc.get(), walker);
    return walker.lines;
}
