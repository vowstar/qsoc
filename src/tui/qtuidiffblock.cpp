// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuidiffblock.h"

#include "tui/qtuitextlayout.h"
#include "tui/qtuiwidget.h"

#include <QRegularExpression>

#include <algorithm>
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
    if (!layoutDirty && layoutWidth == width) {
        return;
    }
    layoutDirty = false;
    layoutWidth = width;
    rows.clear();
    logicalLines_.clear();

    if (folded) {
        QTuiStyledRun run;
        run.text   = QStringLiteral("▸ diff %1 (%2 rows)").arg(headerB).arg(sourceRows.size() - 2);
        run.dim    = true;
        run.italic = true;
        rows.append({.runs = QList<QTuiStyledRun>{run}, .logicalLineIndex = -1});
        return;
    }

    /* First pass: resolve per-row old/new line numbers from the hunk
     * headers so the gutter can show them and we can size its width. */
    struct Lined
    {
        const Row *row;
        int        lineNo; /* number shown in the gutter; -1 = none */
        QChar      sign;
    };
    QList<Lined> lined;
    lined.reserve(sourceRows.size());
    int                             oldNo = 0;
    int                             newNo = 0;
    int                             maxNo = 1;
    static const QRegularExpression hunkRe(
        QStringLiteral("@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@"));
    for (const Row &row : sourceRows) {
        switch (row.kind) {
        case Kind::Hunk: {
            const auto match = hunkRe.match(row.text);
            if (match.hasMatch()) {
                oldNo = match.captured(1).toInt();
                newNo = match.captured(2).toInt();
            }
            lined.append({.row = &row, .lineNo = -1, .sign = QLatin1Char('\0')});
            break;
        }
        case Kind::Add:
            lined.append({.row = &row, .lineNo = newNo, .sign = QLatin1Char('+')});
            maxNo = std::max(maxNo, newNo);
            newNo++;
            break;
        case Kind::Del:
            lined.append({.row = &row, .lineNo = oldNo, .sign = QLatin1Char('-')});
            maxNo = std::max(maxNo, oldNo);
            oldNo++;
            break;
        case Kind::Context:
            lined.append({.row = &row, .lineNo = newNo, .sign = QLatin1Char(' ')});
            maxNo = std::max(maxNo, newNo);
            oldNo++;
            newNo++;
            break;
        case Kind::Header:
        default:
            lined.append({.row = &row, .lineNo = -1, .sign = QLatin1Char('\0')});
            break;
        }
    }

    const int digits       = QString::number(maxNo).size();
    const int gutterWidth  = digits + 3; /* "<num> <sign> " */
    const int blankGutter  = gutterWidth;
    const int contentWidth = std::max(1, width - gutterWidth);

    auto gutterRun = [](const QString &text) {
        QTuiStyledRun run;
        run.text       = text;
        run.dim        = true;
        run.decorative = true;
        return run;
    };

    for (const Lined &item : lined) {
        const Row        &row   = *item.row;
        const QTuiFgColor color = colorFor(row.kind);
        const bool        dim   = isDimKind(row.kind);
        const bool        bold  = isBoldKind(row.kind);

        if (item.lineNo < 0) {
            /* Header / hunk: full-width text, blank gutter, not copyable
             * (logicalLineIndex -1) so a drag yields code content only. */
            QList<QTuiStyledRun> runs;
            runs.append(gutterRun(QString(blankGutter, QLatin1Char(' '))));
            runs.append(coloredRun(row.text, color, bold, dim));
            rows.append({.runs = runs, .logicalLineIndex = -1});
            continue;
        }

        /* Strip the leading marker; the sign lives in the gutter. */
        const QString content = row.text.isEmpty() ? QString() : row.text.mid(1);
        const int     lineIdx = logicalLines_.size();
        logicalLines_.append(content);

        const QString gutter = QStringLiteral("%1 %2 ").arg(item.lineNo, digits).arg(item.sign);
        const QString blank  = QString(blankGutter, QLatin1Char(' '));

        const QList<QTuiVisualRow> wrapped = qtuiWrapStyledRuns(
            QList<QTuiStyledRun>{coloredRun(content, color, bold, dim)}, lineIdx, contentWidth);
        for (int i = 0; i < wrapped.size(); ++i) {
            QList<QTuiStyledRun> runs;
            runs.append(gutterRun(i == 0 ? gutter : blank));
            runs.append(wrapped[i].runs);
            rows.append(
                {.runs              = runs,
                 .logicalLineIndex  = lineIdx,
                 .startColInLogical = wrapped[i].startColInLogical});
        }
    }
}

int QTuiDiffBlock::rowCount() const
{
    return static_cast<int>(rows.size());
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

QString QTuiDiffBlock::selectedLogicalText(
    int rowStartInBlock, int colStart, int rowEndInBlock, int colEnd) const
{
    if (folded) {
        return {};
    }
    /* Yields the sign-stripped content of the selected rows; line
     * numbers, signs, and headers (all gutter / index -1) are excluded.
     * Use Ctrl+Y (toPlainText) for a full git-applyable patch. */
    return qtuiSelectedLogicalText(
        rows, logicalLines_, rowStartInBlock, colStart, rowEndInBlock, colEnd);
}
