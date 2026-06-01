// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiassistanttextblock.h"

#include "common/qsocmarkdownrenderer.h"
#include "tui/qtuitextlayout.h"
#include "tui/qtuiwidget.h"

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
    logicalLines_.clear();
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
    QList<QTuiVisualRow> fullRows;
    for (const auto &line : rendered) {
        const int lineIdx = logicalLines_.size();
        logicalLines_.append(qtuiLogicalText(line.runs));

        QList<QTuiStyledRun> runs;
        if (line.runs.isEmpty()) {
            runs.append(QTuiStyledRun{});
        } else {
            runs = line.runs;
        }
        /* Box tables keep their borders aligned across the row, so they
         * are emitted as a single visual row (column planning already
         * fit them to width, or the table degraded to records). All
         * other content soft-wraps to width. */
        if (line.kind == QSocMarkdownRenderer::Kind::Table) {
            fullRows.append({.runs = runs, .logicalLineIndex = lineIdx, .startColInLogical = 0});
        } else {
            fullRows.append(qtuiWrapStyledRuns(runs, lineIdx, width));
        }
    }

    if (folded) {
        /* Summary row: ▸ N lines (folded). Plain styled run, dim italic
         * to match the fold-control aesthetic shared with tool boxes.
         * logicalLineIndex stays -1 so a drag never maps onto it. */
        QTuiStyledRun summary;
        summary.text   = QStringLiteral("▸ %1 lines (folded)").arg(fullRows.size());
        summary.dim    = true;
        summary.italic = true;
        rows.append({.runs = QList<QTuiStyledRun>{summary}, .logicalLineIndex = -1});
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
    Q_UNUSED(xOffset);
    Q_UNUSED(focused);
    Q_UNUSED(selected);
    if (viewportRow < 0 || viewportRow >= rows.size()) {
        return;
    }
    int painted = 0;
    for (const QTuiStyledRun &run : rows[viewportRow].runs) {
        for (const QChar character : run.text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
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

QString QTuiAssistantTextBlock::toPlainText() const
{
    /* The source markdown is the source of truth for copy. Returning
     * it unmodified keeps copy/paste round-trip safe even when the
     * cached layout has been wrapped or stylized. */
    return source;
}

QString QTuiAssistantTextBlock::selectedLogicalText(
    int rowStartInBlock, int colStart, int rowEndInBlock, int colEnd) const
{
    /* Folded blocks show only a summary; defer to the cell scrape. */
    if (folded) {
        return {};
    }
    return qtuiSelectedLogicalText(
        rows, logicalLines_, rowStartInBlock, colStart, rowEndInBlock, colEnd);
}
