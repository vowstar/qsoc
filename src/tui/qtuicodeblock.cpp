// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuicodeblock.h"

#include "common/qsoccodehighlighter.h"
#include "tui/qtuiwidget.h"

#include <QStringList>

#include <utility>

namespace {

QTuiStyledRun cyanDim(const QString &text)
{
    QTuiStyledRun run;
    run.text = text;
    run.dim  = true;
    run.fg   = QTuiFgColor::Cyan;
    return run;
}

QTuiStyledRun summaryRun(const QString &text)
{
    QTuiStyledRun run;
    run.text   = text;
    run.dim    = true;
    run.italic = true;
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
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rendered.clear();

    if (folded) {
        const int lines = sourceCode.count(QLatin1Char('\n'))
                          + (sourceCode.isEmpty() || sourceCode.endsWith(QLatin1Char('\n')) ? 0 : 1);
        rendered.append(
            QList<QTuiStyledRun>{
                summaryRun(QStringLiteral("▸ %1 code (%2 lines)")
                               .arg(language.isEmpty() ? QStringLiteral("code") : language)
                               .arg(lines))});
        return;
    }

    /* Header banner mirrors the markdown renderer's CodeBlock kind so
     * a streaming-split block is visually indistinguishable from a
     * non-streaming one. */
    {
        QList<QTuiStyledRun> header;
        header.append(cyanDim(QStringLiteral("┄┄┄ ")));
        header.append(cyanDim(language.isEmpty() ? QStringLiteral("code") : language));
        header.append(cyanDim(QStringLiteral(" ┄┄┄")));
        rendered.append(header);
    }

    const QStringList lines = sourceCode.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    int               count = lines.size();
    /* Drop a trailing empty entry produced by a final newline so the
     * block does not render a phantom blank row. */
    if (count > 0 && lines.last().isEmpty()) {
        --count;
    }
    for (int idx = 0; idx < count; ++idx) {
        QList<QTuiStyledRun> row;
        row.append(cyanDim(QStringLiteral("▎ ")));
        const auto tokens = QSocCodeHighlighter::highlight(lines[idx], language);
        for (const auto &tok : tokens) {
            QTuiStyledRun run = tok;
            if (forceDim_) {
                run.dim    = true;
                run.italic = true;
                /* Keep the highlighter colors visible but a notch
                 * dimmer; resetting the fg would erase reasoning code
                 * differentiation entirely. */
            }
            row.append(run);
        }
        rendered.append(row);
    }
}

int QTuiCodeBlock::rowCount() const
{
    return static_cast<int>(rendered.size());
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
    Q_UNUSED(focused);
    Q_UNUSED(selected);
    if (viewportRow < 0 || viewportRow >= rendered.size()) {
        return;
    }
    /* Code rows are noWrap by design — long lines clip via xOffset
     * rather than soft-wrapping which would break gutter alignment. */
    int       skipped = 0;
    int       painted = 0;
    const int effX    = std::max(0, xOffset);
    for (const QTuiStyledRun &run : rendered[viewportRow]) {
        for (const QChar character : run.text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            if (skipped + chW <= effX) {
                skipped += chW;
                continue;
            }
            if (skipped < effX) {
                skipped += chW;
                continue;
            }
            if (painted + chW > width) {
                return;
            }
            QTuiCell &cell = screen.at(painted, screenRow);
            cell.character = character;
            cell.bold      = run.bold;
            cell.italic    = run.italic;
            cell.dim       = run.dim;
            cell.underline = run.underline;
            cell.inverted  = false;
            cell.fgColor   = run.fg;
            cell.bgColor   = run.bg;
            cell.hyperlink = run.hyperlink;
            painted += chW;
        }
    }
}

int QTuiCodeBlock::maxXOffset(int width) const
{
    if (width <= 0) {
        return 0;
    }
    int longest = 0;
    for (const auto &row : rendered) {
        int rowWidth = 0;
        for (const QTuiStyledRun &run : row) {
            for (const QChar character : run.text) {
                rowWidth += QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            }
        }
        longest = std::max(longest, rowWidth);
    }
    return std::max(0, longest - width);
}

QString QTuiCodeBlock::toPlainText() const
{
    /* Raw body verbatim. No fences, no banner, no gutter. This is the
     * whole reason the block exists. */
    return sourceCode;
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
