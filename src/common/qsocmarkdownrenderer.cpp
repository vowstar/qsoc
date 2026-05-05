// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmarkdownrenderer.h"

extern "C" {
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
}

#include <QStringList>

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

    bool isBold() const { return bold > 0; }
    bool isItalic() const { return italic > 0 || blockQuote > 0; }
    bool isInlineCode() const { return inlineCode > 0; }
    bool isInBlockQuote() const { return blockQuote > 0; }
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
        QSocMarkdownRenderer::StyledRun run;
        run.text   = text;
        run.bold   = style.isBold();
        run.italic = style.isItalic();
        if (style.isInlineCode()) {
            run.fg = QTuiFgColor::Yellow;
        } else if (style.isInBlockQuote()) {
            run.dim = true;
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

    /* Code block content lives on its own lines; cmark stores a single
     * literal string with embedded newlines. Split and emit each line
     * with kind=CodeBlock so the consumer can paint a left gutter or
     * differentiate styling. */
    const QStringList rawLines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    /* Split with KeepEmptyParts on a trailing newline yields a tail
     * empty entry; drop it so we don't emit a phantom blank code line. */
    int count = rawLines.size();
    if (count > 0 && rawLines.last().isEmpty()) {
        --count;
    }
    for (int idx = 0; idx < count; ++idx) {
        QSocMarkdownRenderer::RenderedLine line;
        line.kind         = QSocMarkdownRenderer::Kind::CodeBlock;
        line.codeLanguage = language;
        QSocMarkdownRenderer::StyledRun run;
        run.text = rawLines[idx];
        run.dim  = true;
        line.runs.append(run);
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
        run.text = walker.pendingBlockquotePrefix;
        run.dim  = true;
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
            /* OSC 8 hyperlinks are a future commit; for now flatten the
             * link text and drop the URL to avoid decorating prose with
             * markdown literal `[text](url)`. */
            if (!isEnter) {
                /* No special action on exit. */
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

QList<QSocMarkdownRenderer::RenderedLine> QSocMarkdownRenderer::render(const QString &markdown)
{
    if (markdown.isEmpty()) {
        return {};
    }
    ensureExtensionsRegistered();

    const QByteArray utf8 = markdown.toUtf8();

    /* Build a parser with the GFM autolink + tasklist extensions. The
     * table extension is parsed but rendering tables is C6b's job;
     * until then table tokens fall through to text and look raw; the
     * C6b walker just adds a NODE_TABLE switch. */
    ParserPtr parser(cmark_parser_new(CMARK_OPT_DEFAULT));
    if (parser == nullptr) {
        return {};
    }
    static const char *const kExtensions[] = {"autolink", "tasklist"};
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
    walkDocument(doc.get(), walker);
    return walker.lines;
}
