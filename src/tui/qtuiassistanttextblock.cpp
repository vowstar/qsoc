// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiassistanttextblock.h"

#include "common/qsocmarkdownrenderer.h"
#include "tui/qtuiwidget.h"

namespace {

/* Soft-wrap one styled-runs row into several rows of the given visual
 * width, preserving per-character style attribution. Mirrors the
 * scrollview's wrap helper but lives here because the block layout
 * needs to compose multiple rendered lines (headings, list items,
 * blockquote gutters, table borders) into a single cached grid. */
QList<QList<QTuiStyledRun>> wrapRunsToWidth(const QList<QTuiStyledRun> &runs, int width)
{
    QList<QList<QTuiStyledRun>> out;
    if (runs.isEmpty()) {
        out.append(QList<QTuiStyledRun>{});
        return out;
    }
    if (width <= 0) {
        out.append(runs);
        return out;
    }

    struct Cell
    {
        QChar character;
        int   runIdx;
        int   visualWidth;
    };
    QList<Cell> cells;
    cells.reserve(256);
    for (int idx = 0; idx < runs.size(); ++idx) {
        for (const QChar character : runs[idx].text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            cells.append({.character = character, .runIdx = idx, .visualWidth = chW});
        }
    }
    if (cells.isEmpty()) {
        out.append(QList<QTuiStyledRun>{});
        return out;
    }

    auto regroup = [&](int begin, int end) -> QList<QTuiStyledRun> {
        QList<QTuiStyledRun> rowRuns;
        if (begin >= end) {
            return rowRuns;
        }
        int           current = cells[begin].runIdx;
        QTuiStyledRun proto   = runs[current];
        proto.text.clear();
        for (int idx = begin; idx < end; ++idx) {
            if (cells[idx].runIdx != current) {
                if (!proto.text.isEmpty()) {
                    rowRuns.append(proto);
                }
                current    = cells[idx].runIdx;
                proto      = runs[current];
                proto.text = QString();
            }
            proto.text.append(cells[idx].character);
        }
        if (!proto.text.isEmpty()) {
            rowRuns.append(proto);
        }
        return rowRuns;
    };

    int rowStart  = 0;
    int rowWidth  = 0;
    int lastSpace = -1;
    for (int idx = 0; idx < cells.size(); ++idx) {
        const Cell &cell = cells[idx];
        if (rowWidth + cell.visualWidth > width && idx > rowStart) {
            const int breakAt = (lastSpace > rowStart) ? lastSpace : idx;
            out.append(regroup(rowStart, breakAt));
            const int next = (lastSpace > rowStart) ? (breakAt + 1) : breakAt;
            rowStart       = next;
            rowWidth       = 0;
            lastSpace      = -1;
            for (int back = rowStart; back < idx; ++back) {
                rowWidth += cells[back].visualWidth;
            }
        }
        if (cell.character == QLatin1Char(' ')) {
            lastSpace = idx;
        }
        rowWidth += cell.visualWidth;
    }
    if (rowStart < cells.size()) {
        out.append(regroup(rowStart, cells.size()));
    }
    if (out.isEmpty()) {
        out.append(QList<QTuiStyledRun>{});
    }
    return out;
}

} // namespace

QTuiAssistantTextBlock::QTuiAssistantTextBlock(const QString &markdown)
    : source(markdown)
{}

void QTuiAssistantTextBlock::setMarkdown(const QString &markdown)
{
    if (source == markdown) {
        return;
    }
    source = markdown;
    invalidate();
}

void QTuiAssistantTextBlock::appendMarkdown(const QString &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    source.append(chunk);
    invalidate();
}

void QTuiAssistantTextBlock::layout(int width)
{
    if (!layoutDirty && layoutWidth == width) {
        return;
    }
    rows.clear();
    layoutWidth = width;
    layoutDirty = false;

    if (source.isEmpty()) {
        return;
    }

    /* Render markdown at the target width so table column planning
     * sees the same budget the scrollview will paint into. */
    const auto rendered = QSocMarkdownRenderer::render(source, width);

    /* Total row count of the unfolded layout, retained for the fold
     * summary even after we throw the row buffer away. */
    QList<Row> fullRows;
    for (const auto &line : rendered) {
        QList<QTuiStyledRun> runs;
        if (line.runs.isEmpty()) {
            runs.append(QTuiStyledRun{});
        } else {
            runs = line.runs;
        }
        /* Tables and fenced code keep box-drawing / gutter alignment
         * intact across rows, so let them overflow horizontally and
         * rely on the block's xOffset for scroll instead of breaking
         * structure with soft-wrap. */
        const bool noWrap = line.kind == QSocMarkdownRenderer::Kind::Table
                            || line.kind == QSocMarkdownRenderer::Kind::CodeBlock;
        if (noWrap) {
            fullRows.append({.runs = runs, .noWrap = true});
        } else {
            for (const auto &wrappedRow : wrapRunsToWidth(runs, width)) {
                fullRows.append({.runs = wrappedRow, .noWrap = false});
            }
        }
    }

    if (folded) {
        /* Summary row: ▸ N lines (folded). Plain styled run, dim italic
         * to match the fold-control aesthetic shared with tool boxes. */
        QTuiStyledRun summary;
        summary.text   = QStringLiteral("▸ %1 lines (folded)").arg(fullRows.size());
        summary.dim    = true;
        summary.italic = true;
        rows.append({.runs = QList<QTuiStyledRun>{summary}, .noWrap = false});
    } else {
        rows = std::move(fullRows);
    }
}

int QTuiAssistantTextBlock::rowCount() const
{
    return static_cast<int>(rows.size());
}

void QTuiAssistantTextBlock::paintRow(
    QTuiScreen &screen,
    int         screenRow,
    int         viewportRow,
    int         xOffset,
    int         width,
    bool        focused,
    bool        selected) const
{
    Q_UNUSED(focused);
    Q_UNUSED(selected);
    if (viewportRow < 0 || viewportRow >= rows.size()) {
        return;
    }
    /* xOffset only matters for noWrap rows; soft-wrapped rows already
     * fit within width by construction. Wrapped rows ignore the offset
     * and paint from their natural start so prose never disappears
     * sideways when the user h-scrolls a table. */
    const Row &row     = rows[viewportRow];
    const int  effX    = row.noWrap ? xOffset : 0;
    int        skipped = 0;
    int        painted = 0;
    for (const QTuiStyledRun &run : row.runs) {
        for (const QChar character : run.text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            if (skipped + chW <= effX) {
                skipped += chW;
                continue;
            }
            if (skipped < effX) {
                /* Wide char straddling the scroll boundary: skip the
                 * whole glyph rather than render half. */
                skipped += chW;
                continue;
            }
            if (painted + chW > width) {
                return;
            }
            QTuiCell &cell  = screen.at(painted, screenRow);
            cell.character  = character;
            cell.bold       = run.bold && !forceDim;
            cell.italic     = run.italic || forceDim;
            cell.dim        = run.dim || forceDim;
            cell.underline  = run.underline;
            cell.inverted   = false;
            cell.fgColor    = forceDim ? QTuiFgColor::Default : run.fg;
            cell.bgColor    = run.bg;
            cell.hyperlink  = run.hyperlink;
            cell.decorative = run.decorative;
            painted += chW;
        }
    }
}

int QTuiAssistantTextBlock::maxXOffset(int width) const
{
    if (width <= 0) {
        return 0;
    }
    int longest = 0;
    for (const Row &row : rows) {
        if (!row.noWrap) {
            continue;
        }
        int rowWidth = 0;
        for (const QTuiStyledRun &run : row.runs) {
            for (const QChar character : run.text) {
                rowWidth += QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            }
        }
        longest = std::max(longest, rowWidth);
    }
    return std::max(0, longest - width);
}

QString QTuiAssistantTextBlock::toPlainText() const
{
    /* The source markdown is the source of truth for copy. Returning
     * it unmodified keeps copy/paste round-trip safe even when the
     * cached layout has been wrapped or stylized. */
    return source;
}
