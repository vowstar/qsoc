// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitodoblock.h"

#include "tui/qtuiwidget.h"

#include <utility>

namespace {

struct StatusGlyph
{
    QString     icon;
    QTuiFgColor color;
    bool        bold;
};

StatusGlyph glyphFor(const QString &status)
{
    if (status == QStringLiteral("done")) {
        return {QStringLiteral("✓"), QTuiFgColor::Green, true};
    }
    if (status == QStringLiteral("in_progress")) {
        return {QStringLiteral("⌛"), QTuiFgColor::Yellow, false};
    }
    if (status == QStringLiteral("failed")) {
        return {QStringLiteral("✗"), QTuiFgColor::Red, true};
    }
    return {QStringLiteral("☐"), QTuiFgColor::Default, false};
}

QString markerFor(const QString &status)
{
    if (status == QStringLiteral("done")) {
        return QStringLiteral("[x]");
    }
    if (status == QStringLiteral("in_progress")) {
        return QStringLiteral("[~]");
    }
    return QStringLiteral("[ ]");
}

QTuiStyledRun coloredRun(const QString &text, QTuiFgColor color, bool bold = false, bool dim = false)
{
    QTuiStyledRun run;
    run.text = text;
    run.bold = bold;
    run.dim  = dim;
    run.fg   = color;
    return run;
}

} // namespace

QTuiTodoBlock::QTuiTodoBlock(QList<QTuiTodoList::TodoItem> items)
    : items(std::move(items))
{}

void QTuiTodoBlock::layout(int width)
{
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rendered.clear();

    if (folded) {
        QTuiStyledRun summary;
        summary.text   = QStringLiteral("▸ TODO snapshot (%1 items)").arg(items.size());
        summary.dim    = true;
        summary.italic = true;
        rendered.append(QList<QTuiStyledRun>{summary});
        return;
    }

    /* Header so the snapshot reads as a distinct section even when no
     * todos exist. Empty list still gets a single header row to keep
     * the block visible in scrollback. */
    {
        QList<QTuiStyledRun> header;
        header.append(coloredRun(QStringLiteral("TODO"), QTuiFgColor::Cyan, /*bold=*/true));
        QTuiStyledRun count;
        count.text = QStringLiteral("  (%1 item%2)")
                         .arg(items.size())
                         .arg(items.size() == 1 ? QString() : QStringLiteral("s"));
        count.dim  = true;
        header.append(count);
        rendered.append(header);
    }

    for (const QTuiTodoList::TodoItem &item : items) {
        const StatusGlyph    glyph = glyphFor(item.status);
        QList<QTuiStyledRun> row;
        row.append(coloredRun(QStringLiteral("  "), QTuiFgColor::Default));
        row.append(coloredRun(glyph.icon, glyph.color, glyph.bold));
        QTuiStyledRun number;
        number.text = QStringLiteral(" %1. ").arg(item.id);
        number.dim  = true;
        row.append(number);
        QTuiStyledRun title;
        title.text = item.title;
        if (item.status == QStringLiteral("done")) {
            title.dim = true;
        }
        row.append(title);
        rendered.append(row);
    }
}

int QTuiTodoBlock::rowCount() const
{
    return static_cast<int>(rendered.size());
}

void QTuiTodoBlock::paintRow(
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
            QTuiCell &cell  = screen.at(col, screenRow);
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
            col += QTuiText::isWideChar(character.unicode()) ? 2 : 1;
        }
    }
}

QString QTuiTodoBlock::toPlainText() const
{
    QString out;
    for (const QTuiTodoList::TodoItem &item : items) {
        out.append(
            QStringLiteral("%1. %2 %3\n").arg(item.id).arg(markerFor(item.status)).arg(item.title));
    }
    return out;
}

QString QTuiTodoBlock::toMarkdown() const
{
    /* GFM task-list shape so a paste-back into another markdown
     * processor renders as actual checkboxes. */
    QString out;
    for (const QTuiTodoList::TodoItem &item : items) {
        out.append(markerFor(item.status));
        out.append(QStringLiteral(" "));
        out.append(item.title);
        out.append(QLatin1Char('\n'));
    }
    return out;
}
