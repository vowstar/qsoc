// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuicodeblock.h"

#include "common/qsoccodehighlighter.h"
#include "tui/qtuitextlayout.h"
#include "tui/qtuiwidget.h"

#include <QStringList>

#include <utility>

namespace {

QTuiStyledRun cyanDim(const QString &text)
{
    QTuiStyledRun run;
    run.text       = text;
    run.dim        = true;
    run.fg         = QTuiFgColor::Cyan;
    run.decorative = true;
    return run;
}

QTuiStyledRun summaryRun(const QString &text)
{
    QTuiStyledRun run;
    run.text       = text;
    run.dim        = true;
    run.italic     = true;
    run.decorative = true;
    return run;
}

} // namespace

QTuiCodeBlock::QTuiCodeBlock(QString language, QString sourceCode, bool forceDim, int groupId)
    : language(std::move(language))
    , sourceCode(std::move(sourceCode))
    , forceDim_(forceDim)
    , groupId_(groupId)
{}

void QTuiCodeBlock::appendBody(const QString &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    sourceCode.append(chunk);
    invalidate();
}

void QTuiCodeBlock::layout(int width)
{
    if (!layoutDirty && layoutWidth == width) {
        return;
    }
    layoutDirty = false;
    layoutWidth = width;
    rows.clear();
    logicalLines_.clear();

    if (folded) {
        const int lines = sourceCode.count(QLatin1Char('\n'))
                          + (sourceCode.isEmpty() || sourceCode.endsWith(QLatin1Char('\n')) ? 0 : 1);
        rows.append(
            {.runs             = QList<QTuiStyledRun>{summaryRun(
                 QStringLiteral("▸ %1 code (%2 lines)")
                     .arg(language.isEmpty() ? QStringLiteral("code") : language)
                     .arg(lines))},
             .logicalLineIndex = -1});
        return;
    }

    /* Header banner mirrors the markdown renderer's CodeBlock kind so
     * a streaming-split block is visually indistinguishable from a
     * non-streaming one. Index -1 keeps drag-copy off the banner. */
    {
        QList<QTuiStyledRun> header;
        header.append(cyanDim(QStringLiteral("┄┄┄ ")));
        header.append(cyanDim(language.isEmpty() ? QStringLiteral("code") : language));
        header.append(cyanDim(QStringLiteral(" ┄┄┄")));
        rows.append({.runs = header, .logicalLineIndex = -1});
    }

    const QStringList lines = sourceCode.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    int               count = lines.size();
    /* Drop a trailing empty entry produced by a final newline so the
     * block does not render a phantom blank row. */
    if (count > 0 && lines.last().isEmpty()) {
        --count;
    }
    for (int idx = 0; idx < count; ++idx) {
        const int lineIdx = logicalLines_.size();
        logicalLines_.append(lines[idx]);

        /* `▎ ` decorative gutter then the highlighted code. The whole
         * row soft-wraps to width; continuation rows carry no gutter,
         * which is fine because the gutter is decoration only. */
        QList<QTuiStyledRun> row;
        row.append(cyanDim(QStringLiteral("▎ ")));
        const auto tokens = QSocCodeHighlighter::highlight(lines[idx], language);
        for (const auto &tok : tokens) {
            QTuiStyledRun run = tok;
            if (forceDim_) {
                run.dim    = true;
                run.italic = true;
            }
            row.append(run);
        }
        rows.append(qtuiWrapStyledRuns(row, lineIdx, width));
    }
}

int QTuiCodeBlock::rowCount() const
{
    return static_cast<int>(rows.size());
}

void QTuiCodeBlock::paintRow(
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
            cell.bold       = run.bold;
            cell.italic     = run.italic;
            cell.dim        = run.dim;
            cell.underline  = run.underline;
            cell.inverted   = false;
            cell.fgColor    = run.fg;
            cell.bgColor    = run.bg;
            cell.hyperlink  = run.hyperlink;
            cell.decorative = run.decorative;
            painted += chW;
        }
    }
}

QString QTuiCodeBlock::toPlainText() const
{
    /* Raw body verbatim. No fences, no banner, no gutter. This is the
     * whole reason the block exists. */
    return sourceCode;
}

QString QTuiCodeBlock::selectedLogicalText(
    int rowStartInBlock, int colStart, int rowEndInBlock, int colEnd) const
{
    if (folded) {
        return {};
    }
    return qtuiSelectedLogicalText(
        rows, logicalLines_, rowStartInBlock, colStart, rowEndInBlock, colEnd);
}

QString QTuiCodeBlock::toMarkdown() const
{
    QString out;
    out.append(QStringLiteral("```"));
    out.append(language);
    out.append(QLatin1Char('\n'));
    out.append(sourceCode);
    if (!sourceCode.endsWith(QLatin1Char('\n'))) {
        out.append(QLatin1Char('\n'));
    }
    out.append(QStringLiteral("```\n"));
    return out;
}
