// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiuserblock.h"

#include "tui/qtuiwidget.h"

#include <utility>

QTuiUserBlock::QTuiUserBlock(QString text)
    : source(std::move(text))
{
    /* Trim a single trailing newline so an input ending in \n does
     * not produce a phantom empty row. Leave any explicit blank
     * lines the user wrote in the middle alone. */
    QString normalised = source;
    if (normalised.endsWith(QLatin1Char('\n'))) {
        normalised.chop(1);
    }
    lines = normalised.split(QLatin1Char('\n'));
}

void QTuiUserBlock::layout(int width)
{
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rendered.clear();
    for (const QString &line : lines) {
        QList<QTuiStyledRun> row;
        QTuiStyledRun        gutter;
        gutter.text = QStringLiteral("▍ ");
        gutter.bold = true;
        gutter.fg   = QTuiFgColor::Blue;
        row.append(gutter);
        QTuiStyledRun body;
        body.text = line;
        body.fg   = QTuiFgColor::Blue;
        row.append(body);
        rendered.append(row);
    }
    if (rendered.isEmpty()) {
        /* Degenerate empty input still renders one row so the block
         * has visible presence in scrollback. */
        QList<QTuiStyledRun> row;
        QTuiStyledRun        gutter;
        gutter.text = QStringLiteral("▍ ");
        gutter.bold = true;
        gutter.fg   = QTuiFgColor::Blue;
        row.append(gutter);
        rendered.append(row);
    }
}

int QTuiUserBlock::rowCount() const
{
    return static_cast<int>(rendered.size());
}

void QTuiUserBlock::paintRow(
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
    if (viewportRow < 0 || viewportRow >= rendered.size()) {
        return;
    }
    int col = 0;
    for (const QTuiStyledRun &run : rendered[viewportRow]) {
        for (const QChar character : run.text) {
            if (col >= width) {
                return;
            }
            QTuiCell &cell = screen.at(col, screenRow);
            cell.character = character;
            cell.bold      = run.bold;
            cell.italic    = run.italic;
            cell.dim       = run.dim;
            cell.underline = run.underline;
            cell.inverted  = false;
            cell.fgColor   = run.fg;
            cell.bgColor   = run.bg;
            col += QTuiText::isWideChar(character.unicode()) ? 2 : 1;
        }
    }
}

QString QTuiUserBlock::toPlainText() const
{
    /* Copy returns exactly what the user typed; downstream tools that
     * want the visual gutter can re-render via toMarkdown. */
    return source;
}

QString QTuiUserBlock::toMarkdown() const
{
    /* Markdown blockquote so a paste back into a chat client reads
     * as a quoted user message rather than the next assistant turn. */
    QString out;
    for (const QString &line : lines) {
        out.append(QStringLiteral("> "));
        out.append(line);
        out.append(QLatin1Char('\n'));
    }
    if (out.isEmpty()) {
        out = QStringLiteral("> \n");
    }
    return out;
}
