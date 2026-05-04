// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiscrollview.h"
#include "tui/qtuiwidget.h"

namespace {

/* Visual-width-aware soft wrap. Breaks preferentially on spaces, falls
 * back to hard breaks for tokens wider than the available width.
 *
 * continuationPrefix: prepended to every emitted chunk except the first,
 * and counted against that chunk's width budget. Used by diff rendering
 * to keep continuation rows aligned under the +/- marker column so a
 * wrapped add/del line still reads as a single visual block. */
QList<QString> softWrap(const QString &text, int maxWidth, const QString &continuationPrefix)
{
    QList<QString> out;
    if (maxWidth <= 0) {
        out.append(text);
        return out;
    }
    const int prefixWidth = QTuiText::visualWidth(continuationPrefix);
    auto      finalize    = [&](const QString &chunk) {
        out.append(out.isEmpty() ? chunk : continuationPrefix + chunk);
    };
    auto budget = [&]() {
        if (out.isEmpty()) {
            return maxWidth;
        }
        return qMax(1, maxWidth - prefixWidth);
    };

    QString current;
    int     currentWidth = 0;
    int     lastBreak    = -1; /* last space index in current buffer */
    for (int idx = 0; idx < text.size(); ++idx) {
        const QChar ch    = text.at(idx);
        const int   chWid = QTuiText::isWideChar(ch.unicode()) ? 2 : 1;
        if (currentWidth + chWid > budget() && !current.isEmpty()) {
            if (lastBreak > 0) {
                /* Break at the last space, carry the tail forward. */
                QString head = current.left(lastBreak);
                QString tail = current.mid(lastBreak + 1);
                finalize(head);
                current      = tail;
                currentWidth = 0;
                for (const QChar tc : tail) {
                    currentWidth += QTuiText::isWideChar(tc.unicode()) ? 2 : 1;
                }
                lastBreak = -1;
            } else {
                /* Token wider than the row: hard-break. */
                finalize(current);
                current.clear();
                currentWidth = 0;
                lastBreak    = -1;
            }
        }
        if (ch == QLatin1Char(' ')) {
            lastBreak = current.size();
        }
        current.append(ch);
        currentWidth += chWid;
    }
    if (!current.isEmpty() || out.isEmpty()) {
        finalize(current);
    }
    return out;
}

} // namespace

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
    /* Reserve 1 column for scrollbar */
    int contentWidth = width - 1;
    if (contentWidth < 1) {
        contentWidth = width;
    }

    /* Flatten every stored line (plus the active streaming partial) into
     * wrapped display rows so long paragraphs and CJK text spill onto
     * additional rows instead of being truncated at the edge. Each
     * source line may contribute one or more display rows. */
    struct DisplayRow
    {
        QString   text;
        LineStyle style;
    };
    QList<DisplayRow> displayRows;
    displayRows.reserve(lines.size() + 8);

    /* Diff lines carry a single +/-/space marker as their first column,
     * so wrapped continuation rows get a 1-char space gutter to align
     * the wrapped content under the original content's first character.
     * Without this the second visual row sticks one column to the left
     * and breaks the "single block of red/green" reading. */
    auto continuationPrefixFor = [](LineStyle style) -> QString {
        switch (style) {
        case DiffAdd:
        case DiffDel:
        case DiffContext:
            return QStringLiteral(" ");
        default:
            return {};
        }
    };

    auto pushWrapped = [&](const QString &text, LineStyle style) {
        if (text.isEmpty()) {
            displayRows.append({QString(), style});
            return;
        }
        const QString contPrefix = continuationPrefixFor(style);
        for (const QString &chunk : softWrap(text, contentWidth, contPrefix)) {
            displayRows.append({chunk, style});
        }
    };

    for (const Line &line : lines) {
        pushWrapped(line.text, line.style);
    }
    if (!partialLine.isEmpty()) {
        pushWrapped(partialLine, partialStyle);
    }

    const int totalVisible = displayRows.size();

    /* Calculate viewport in display-row space so scrolling is smooth
     * even when a single source line spans many rows. */
    int viewBottom = totalVisible - scrollOffset;
    int viewTop    = viewBottom - height;

    for (int viewRow = 0; viewRow < height; viewRow++) {
        int rowIdx  = viewTop + viewRow;
        int screenY = startRow + viewRow;

        if (rowIdx < 0 || rowIdx >= totalVisible) {
            continue;
        }

        const QString  &text  = displayRows[rowIdx].text;
        const LineStyle style = displayRows[rowIdx].style;

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
        screen.putString(0, screenY, text, isBold, isDim, /*inverted=*/false, fgColor);
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
    /* Display-row count varies with terminal width, so clamp loosely here
     * against a pessimistic upper bound and let render()'s viewport math
     * ignore rows past the top of the wrapped content. */
    const int softCap = (static_cast<int>(lines.size()) + 1) * 8;
    scrollOffset      = qMin(scrollOffset + count, qMax(0, softCap));
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
