// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitextlayout.h"

#include "tui/qtuiwidget.h"

#include <algorithm>
#include <climits>

QString qtuiLogicalText(const QList<QTuiStyledRun> &runs)
{
    QString out;
    for (const QTuiStyledRun &run : runs) {
        if (!run.decorative) {
            out.append(run.text);
        }
    }
    return out;
}

QList<QTuiVisualRow> qtuiWrapStyledRuns(
    const QList<QTuiStyledRun> &runs, int logicalLineIndex, int width)
{
    QList<QTuiVisualRow> out;
    auto                 single = [&](const QList<QTuiStyledRun> &rowRuns, int startCol) {
        out.append(
            {.runs = rowRuns, .logicalLineIndex = logicalLineIndex, .startColInLogical = startCol});
    };
    if (runs.isEmpty()) {
        single(QList<QTuiStyledRun>{}, 0);
        return out;
    }
    if (width <= 0) {
        single(runs, 0);
        return out;
    }

    struct Cell
    {
        QChar character;
        int   runIdx;
        int   visualWidth;
        bool  decorative;
    };
    QList<Cell> cells;
    cells.reserve(256);
    for (int idx = 0; idx < runs.size(); ++idx) {
        for (const QChar character : runs[idx].text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            cells.append(
                {.character   = character,
                 .runIdx      = idx,
                 .visualWidth = chW,
                 .decorative  = runs[idx].decorative});
        }
    }
    if (cells.isEmpty()) {
        single(QList<QTuiStyledRun>{}, 0);
        return out;
    }

    /* Prefix count of non-decorative cells, so a row's first-cell index
     * yields its char offset within the logical line directly. */
    QList<int> nonDecoBefore;
    nonDecoBefore.reserve(cells.size() + 1);
    nonDecoBefore.append(0);
    for (const Cell &cell : cells) {
        nonDecoBefore.append(nonDecoBefore.last() + (cell.decorative ? 0 : 1));
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
            single(regroup(rowStart, breakAt), nonDecoBefore[rowStart]);
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
        single(regroup(rowStart, cells.size()), nonDecoBefore[rowStart]);
    }
    if (out.isEmpty()) {
        single(QList<QTuiStyledRun>{}, 0);
    }
    return out;
}

QString qtuiSelectedLogicalText(
    const QList<QTuiVisualRow> &rows,
    const QStringList          &logicalLines,
    int                         rowStart,
    int                         colStart,
    int                         rowEnd,
    int                         colEnd)
{
    if (rows.isEmpty() || rowStart > rowEnd) {
        return {};
    }
    const int firstRow = std::max(0, rowStart);
    const int lastRow  = std::min(static_cast<int>(rows.size()) - 1, rowEnd);

    QString result;
    int     curLine    = -1;
    int     curMin     = 0;
    int     curMax     = -1;
    bool    curTouched = false;
    bool    firstPiece = true;

    auto flush = [&]() {
        if (curLine < 0 || curLine >= logicalLines.size()) {
            return;
        }
        if (!firstPiece) {
            result += QLatin1Char('\n');
        }
        firstPiece = false;
        if (curTouched && curMax >= curMin) {
            const QString &logical = logicalLines[curLine];
            const int      from    = std::clamp(curMin, 0, static_cast<int>(logical.size()));
            const int      to      = std::clamp(curMax + 1, from, static_cast<int>(logical.size()));
            result += logical.mid(from, to - from);
        }
    };

    for (int r = firstRow; r <= lastRow; ++r) {
        const QTuiVisualRow &row = rows[r];
        if (row.logicalLineIndex < 0) {
            continue; /* banner / separator: not logical content */
        }
        const int colBegin = (r == firstRow) ? colStart : 0;
        const int colLimit = (r == lastRow) ? colEnd : INT_MAX;

        int  painted = 0;
        int  k       = 0; /* non-decorative char index within the row */
        int  rowMin  = 0;
        int  rowMax  = -1;
        bool rowHit  = false;
        for (const QTuiStyledRun &run : row.runs) {
            for (const QChar character : run.text) {
                const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
                if (!run.decorative) {
                    const int colA = painted;
                    const int colB = painted + chW - 1;
                    if (colB >= colBegin && colA <= colLimit) {
                        const int off = row.startColInLogical + k;
                        if (!rowHit) {
                            rowMin = off;
                            rowHit = true;
                        }
                        rowMax = off;
                    }
                    k++;
                }
                painted += chW;
            }
        }

        if (row.logicalLineIndex != curLine) {
            flush();
            curLine    = row.logicalLineIndex;
            curMin     = rowHit ? rowMin : 0;
            curMax     = rowHit ? rowMax : -1;
            curTouched = rowHit;
        } else if (rowHit) {
            if (!curTouched) {
                curMin     = rowMin;
                curTouched = true;
            }
            curMax = std::max(curMax, rowMax);
        }
    }
    flush();
    return result;
}
