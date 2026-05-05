// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMARKDOWNRENDERER_H
#define QSOCMARKDOWNRENDERER_H

#include "tui/qtuiscreen.h"

#include <QList>
#include <QString>

#include <cstdint>

/**
 * @brief Render Markdown text into a flat sequence of styled rows the
 *        TUI scrollview can paint cell-by-cell.
 * @details Wraps cmark-gfm to parse GFM, walks the AST once, and emits
 *          @ref RenderedLine entries that pair the displayed text with
 *          per-run style flags. The renderer never touches QTuiScreen
 *          itself; it returns layout-ready data so the consumer can
 *          decide where on screen to put it (scrollview body, doc
 *          preview pane, etc.).
 *
 *          Block coverage in this baseline drop:
 *          - Headings (h1 with bold + underline; h2-h6 bold)
 *          - Bold / italic / inline code
 *          - Paragraphs and blank-line separators
 *          - Ordered + unordered lists with nesting indent
 *          - Blockquotes with a dim "│ " gutter and italic content
 *          - Fenced code blocks (raw text with codeBlock=true marker)
 *          - Soft / hard line breaks
 *
 *          Strikethrough is intentionally not enabled because LLMs
 *          frequently produce ` ~100 ` as plain text and would have
 *          chunks turn into rendered strikethrough by accident.
 *
 *          Tables are deferred to the C6b commit which extends the same
 *          AST walker with column-width planning + box-drawing output.
 */
class QSocMarkdownRenderer
{
public:
    /**
     * @brief Visual style flags for a contiguous run of text within a
     *        rendered line. Aliased to the screen-level run type so the
     *        scrollview can ingest renderer output without an adapter
     *        copy. Field semantics: bold/italic/dim/underline map to
     *        SGR attributes; fg/bg are 256-palette indices via the
     *        warm retro color enum.
     */
    using StyledRun = QTuiStyledRun;

    /**
     * @brief One physical line of rendered output.
     * @details A list of styled runs concatenated left-to-right, plus a
     *          flag set the consumer can inspect to paint code blocks
     *          differently (monospace gutter, language label, etc.) or
     *          to apply blockquote indentation in a wrapping pass.
     */
    enum class Kind : std::uint8_t {
        Plain,      /* Default text line */
        Heading,    /* h1-h6; level recorded in headingLevel */
        ListItem,   /* List item with depth-based indent already applied */
        BlockQuote, /* Already prefixed with "│ " gutter */
        CodeBlock,  /* Raw code; codeLanguage filled when known */
        BlankLine,  /* Visual separator emitted between block elements */
        Table,      /* Border or cell row built with box-drawing chars */
    };

    struct RenderedLine
    {
        QList<StyledRun> runs;
        Kind             kind         = Kind::Plain;
        int              headingLevel = 0;
        QString          codeLanguage; /* Lowercased fence info for CodeBlock */
    };

    /**
     * @brief Render the markdown document into a flat list of lines.
     * @details Empty input returns an empty list. The renderer is
     *          stateless; the cmark-gfm parser is constructed and
     *          freed inside the call so callers can re-render the same
     *          string repeatedly during streaming without leaks. Core
     *          GFM extensions are registered exactly once per process
     *          (cmark_gfm_core_extensions_ensure_registered is
     *          idempotent).
     * @param markdown      Source GFM markdown text.
     * @param terminalWidth Visible width in cells used to plan table
     *                      column widths. Pass 0 for "unconstrained";
     *                      tables then render at their ideal width.
     */
    static QList<RenderedLine> render(const QString &markdown, int terminalWidth = 0);
};

#endif // QSOCMARKDOWNRENDERER_H
