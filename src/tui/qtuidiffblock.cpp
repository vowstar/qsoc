// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuidiffblock.h"

#include "tui/qtuiwidget.h"

#include <utility>

namespace {

QTuiStyledRun coloredRun(const QString &text, QTuiFgColor fg, bool bold = false, bool dim = false)
{
    QTuiStyledRun run;
    run.text = text;
    run.bold = bold;
    run.dim  = dim;
    run.fg   = fg;
    return run;
}

QTuiFgColor colorFor(QTuiDiffBlock::Kind kind)
{
    switch (kind) {
    case QTuiDiffBlock::Kind::Header:
        return QTuiFgColor::Cyan;
    case QTuiDiffBlock::Kind::Hunk:
        return QTuiFgColor::Yellow;
    case QTuiDiffBlock::Kind::Add:
        return QTuiFgColor::Green;
    case QTuiDiffBlock::Kind::Del:
        return QTuiFgColor::Red;
    case QTuiDiffBlock::Kind::Context:
    default:
        return QTuiFgColor::Default;
    }
}

bool isDimKind(QTuiDiffBlock::Kind kind)
{
    return kind == QTuiDiffBlock::Kind::Context || kind == QTuiDiffBlock::Kind::Header;
}

bool isBoldKind(QTuiDiffBlock::Kind kind)
{
    return kind == QTuiDiffBlock::Kind::Hunk;
}

} // namespace

QTuiDiffBlock::QTuiDiffBlock(QString headerA, QString headerB)
    : headerA(std::move(headerA))
    , headerB(std::move(headerB))
{
    /* Stash the unified-diff headers as the first two source rows so
     * the copy paths produce a fully self-contained patch without the
     * caller needing to remember to add them. */
    sourceRows.append({.kind = Kind::Header, .text = QStringLiteral("--- ") + this->headerA});
    sourceRows.append({.kind = Kind::Header, .text = QStringLiteral("+++ ") + this->headerB});
}

void QTuiDiffBlock::addRow(Kind kind, const QString &text)
{
    sourceRows.append({.kind = kind, .text = text});
    invalidate();
}

void QTuiDiffBlock::layout(int width)
{
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rendered.clear();

    if (folded) {
        QTuiStyledRun run;
        run.text   = QStringLiteral("▸ diff %1 (%2 rows)").arg(headerB).arg(sourceRows.size() - 2);
        run.dim    = true;
        run.italic = true;
        rendered.append(QList<QTuiStyledRun>{run});
        return;
    }

    for (const Row &row : sourceRows) {
        const QTuiFgColor color = colorFor(row.kind);
        const bool        dim   = isDimKind(row.kind);
        const bool        bold  = isBoldKind(row.kind);
        rendered.append(QList<QTuiStyledRun>{coloredRun(row.text, color, bold, dim)});
    }
}

int QTuiDiffBlock::rowCount() const
{
    return static_cast<int>(rendered.size());
}

void QTuiDiffBlock::paintRow(
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

int QTuiDiffBlock::maxXOffset(int width) const
{
    if (width <= 0) {
        return 0;
    }
    int longest = 0;
    for (const Row &row : sourceRows) {
        int rowW = 0;
        for (const QChar character : row.text) {
            rowW += QTuiText::isWideChar(character.unicode()) ? 2 : 1;
        }
        longest = std::max(longest, rowW);
    }
    return std::max(0, longest - width);
}

QString QTuiDiffBlock::toPlainText() const
{
    /* Emit the rows verbatim with a trailing newline on each. Headers
     * are already in sourceRows so the result is a complete unified
     * diff that `git apply` will accept without massaging. */
    QString out;
    for (const Row &row : sourceRows) {
        out.append(row.text);
        out.append(QLatin1Char('\n'));
    }
    return out;
}

QString QTuiDiffBlock::toMarkdown() const
{
    /* Wrap the patch in a ```diff fence so it round-trips through
     * markdown processors that might otherwise eat the leading +/-. */
    QString out = QStringLiteral("```diff\n");
    out.append(toPlainText());
    out.append(QStringLiteral("```\n"));
    return out;
}
