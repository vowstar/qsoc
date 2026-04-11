// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiscrollview.h"
#include "tui/qtuiwidget.h"

void QTuiScrollView::appendLine(const QString &text, LineStyle style)
{
    lines.append({.text = text, .style = style});

    /* Enforce max buffer depth */
    while (lines.size() > MAX_LINES) {
        lines.removeFirst();
        if (scrollOffset > 0) {
            scrollOffset--;
        }
    }
}

void QTuiScrollView::appendPartial(const QString &text, LineStyle style)
{
    /* When the caller switches style mid-stream while a partial line is
     * still pending, finalize that partial under its ORIGINAL style first
     * — otherwise the new style would retroactively recolor the prior
     * content (e.g. a diff render landing right after a streaming
     * reasoning chunk would paint the unfinished sentence red). */
    if (style != partialStyle && !partialLine.isEmpty()) {
        appendLine(partialLine, partialStyle);
        partialLine.clear();
    }
    partialStyle = style;
    partialLine += text;

    /* Split on newlines */
    while (true) {
        int idx = partialLine.indexOf('\n');
        if (idx < 0) {
            break;
        }
        appendLine(partialLine.left(idx), partialStyle);
        partialLine = partialLine.mid(idx + 1);
    }
}

void QTuiScrollView::render(QTuiScreen &screen, int startRow, int height, int width)
{
    int totalVisible = static_cast<int>(lines.size()) + (partialLine.isEmpty() ? 0 : 1);

    /* Reserve 1 column for scrollbar */
    int contentWidth = width - 1;
    if (contentWidth < 1) {
        contentWidth = width;
    }

    /* Calculate viewport */
    int viewBottom = totalVisible - scrollOffset;
    int viewTop    = viewBottom - height;

    for (int viewRow = 0; viewRow < height; viewRow++) {
        int lineIdx = viewTop + viewRow;
        int screenY = startRow + viewRow;

        if (lineIdx < 0 || lineIdx >= totalVisible) {
            continue;
        }

        QString   text;
        LineStyle style;

        if (lineIdx < lines.size()) {
            text  = lines[lineIdx].text;
            style = lines[lineIdx].style;
        } else {
            text  = partialLine;
            style = partialStyle;
        }

        bool        isDim   = (style == Dim) || (style == DiffContext);
        bool        isBold  = (style == Bold) || (style == DiffHunk);
        QTuiFgColor fgColor = QTuiFgColor::Default;
        switch (style) {
        case DiffAdd:
            fgColor = QTuiFgColor::Green;
            break;
        case DiffDel:
            fgColor = QTuiFgColor::Red;
            break;
        case DiffHunk:
            fgColor = QTuiFgColor::Yellow;
            break;
        case Normal:
        case Dim:
        case Bold:
        case DiffContext:
        default:
            break;
        }
        /* Truncate by visual width (not char count) to prevent CJK overflow into scrollbar */
        QString truncated = text;
        if (QTuiText::visualWidth(truncated) > contentWidth) {
            /* Use visual-width-aware truncation without "..." suffix */
            truncated = truncated.left(contentWidth); /* rough cut */
            while (QTuiText::visualWidth(truncated) > contentWidth && !truncated.isEmpty()) {
                truncated.chop(1);
            }
        }
        screen.putString(0, screenY, truncated, isBold, isDim, /*inverted=*/false, fgColor);
    }

    /* Render ASCII scrollbar on the right column */
    if (width > 1) {
        int scrollCol = width - 1;

        if (totalVisible <= height || height <= 0) {
            /* No scrollbar needed — all content fits */
            for (int row = 0; row < height; row++) {
                screen.putChar(scrollCol, startRow + row, ' ', false, true);
            }
        } else {
            /* Calculate thumb position and size with clamping */
            int thumbSize   = qBound(1, height * height / totalVisible, height);
            int scrollRange = totalVisible - height;
            int clampedOff  = qBound(0, scrollOffset, scrollRange);
            int currentPos  = scrollRange - clampedOff;
            int thumbTop    = 0;
            if (scrollRange > 0) {
                thumbTop = currentPos * (height - thumbSize) / scrollRange;
            }
            thumbTop        = qBound(0, thumbTop, height - thumbSize);
            int thumbBottom = thumbTop + thumbSize;

            for (int row = 0; row < height; row++) {
                QChar scrollChar;
                if (row >= thumbTop && row < thumbBottom) {
                    scrollChar = '#'; /* Thumb */
                } else {
                    scrollChar = '|'; /* Track */
                }
                screen.putChar(scrollCol, startRow + row, scrollChar, false, true);
            }
        }
    }
}

void QTuiScrollView::scrollUp(int count)
{
    int totalVisible = static_cast<int>(lines.size()) + (partialLine.isEmpty() ? 0 : 1);
    scrollOffset     = qMin(scrollOffset + count, qMax(0, totalVisible - 1));
}

void QTuiScrollView::scrollDown(int count)
{
    scrollOffset = qMax(0, scrollOffset - count);
}

void QTuiScrollView::scrollToBottom()
{
    scrollOffset = 0;
}

bool QTuiScrollView::isAtBottom() const
{
    return scrollOffset == 0;
}

QString QTuiScrollView::toPlainText() const
{
    QString result;
    for (const auto &line : lines) {
        result += line.text + "\n";
    }
    if (!partialLine.isEmpty()) {
        result += partialLine;
    }
    return result;
}

void QTuiScrollView::clear()
{
    lines.clear();
    partialLine.clear();
    scrollOffset = 0;
}
