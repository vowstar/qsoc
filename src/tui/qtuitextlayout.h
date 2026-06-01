// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITEXTLAYOUT_H
#define QTUITEXTLAYOUT_H

#include "tui/qtuiscreen.h"

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Shared soft-wrap and logical-text-selection helpers for blocks.
 * @details All scrollback blocks wrap content to the viewport width and
 *          must map a visual sub-rectangle back to the unwrapped logical
 *          text for mouse-drag copy. This header centralises that logic
 *          so the assistant / code / diff blocks do not each reimplement
 *          (and drift on) the cell-walking, wide-char, and decoration-
 *          stripping rules.
 */

/** One soft-wrapped visual row tagged with where it sits in its logical
 *  line: logicalLineIndex points into the owning block's logicalLines_,
 *  startColInLogical is the char offset (counting non-decorative cells)
 *  of this row's first cell within that line. */
struct QTuiVisualRow
{
    QList<QTuiStyledRun> runs;
    int                  logicalLineIndex  = -1;
    int                  startColInLogical = 0;
};

/** Decoration-stripped text of a run list: the logical content a copy
 *  should yield, with gutters / borders / fold markers removed. */
QString qtuiLogicalText(const QList<QTuiStyledRun> &runs);

/** Soft-wrap one logical line's runs to `width`, preserving per-character
 *  style attribution and breaking at spaces when possible. Each returned
 *  row carries `logicalLineIndex` and its non-decorative start offset.
 *  Always returns at least one row. width <= 0 yields a single row. */
QList<QTuiVisualRow> qtuiWrapStyledRuns(
    const QList<QTuiStyledRun> &runs, int logicalLineIndex, int width);

/** Extract the unwrapped logical text covered by a visual sub-rectangle.
 *  Visual rows sharing a logical line are joined with no separator
 *  (unwrap); distinct logical lines are separated by '\n'. Rows whose
 *  logicalLineIndex < 0 (banners, separators) are skipped. Columns are
 *  block-local visual cells; colStart applies to rowStart, colEnd to
 *  rowEnd, intermediate rows span full width. Decorative cells never
 *  contribute. */
QString qtuiSelectedLogicalText(
    const QList<QTuiVisualRow> &rows,
    const QStringList          &logicalLines,
    int                         rowStart,
    int                         colStart,
    int                         rowEnd,
    int                         colEnd);

#endif // QTUITEXTLAYOUT_H
