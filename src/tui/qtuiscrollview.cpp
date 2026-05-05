// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiscrollview.h"

#include "tui/qtuiwidget.h"

#include <utility>

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
    int     lastBreak    = -1;
    for (int idx = 0; idx < text.size(); ++idx) {
        const QChar character = text.at(idx);
        const int   chWid     = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
        if (currentWidth + chWid > budget() && !current.isEmpty()) {
            if (lastBreak > 0) {
                QString head = current.left(lastBreak);
                QString tail = current.mid(lastBreak + 1);
                finalize(head);
                current      = tail;
                currentWidth = 0;
                for (const QChar tail_ch : tail) {
                    currentWidth += QTuiText::isWideChar(tail_ch.unicode()) ? 2 : 1;
                }
                lastBreak = -1;
            } else {
                finalize(current);
                current.clear();
                currentWidth = 0;
                lastBreak    = -1;
            }
        }
        if (character == QLatin1Char(' ')) {
            lastBreak = current.size();
        }
        current.append(character);
        currentWidth += chWid;
    }
    if (!current.isEmpty() || out.isEmpty()) {
        finalize(current);
    }
    return out;
}

QList<QList<QTuiStyledRun>> wrapStyledRuns(const QList<QTuiStyledRun> &runs, int maxWidth)
{
    QList<QList<QTuiStyledRun>> out;
    if (runs.isEmpty()) {
        return out;
    }
    if (maxWidth <= 0) {
        out.append(runs);
        return out;
    }
    struct CharCell
    {
        QChar character;
        int   runIdx;
        int   width;
    };
    QList<CharCell> cells;
    cells.reserve(256);
    for (int idx = 0; idx < runs.size(); ++idx) {
        for (const QChar character : runs[idx].text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            cells.append({.character = character, .runIdx = idx, .width = chW});
        }
    }
    if (cells.isEmpty()) {
        out.append(runs);
        return out;
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
        const auto &cell = cells[idx];
        if (rowWidth + cell.width > maxWidth && idx > rowStart) {
            const int breakAt = (lastSpace > rowStart) ? lastSpace : idx;
            out.append(regroup(rowStart, breakAt));
            const int next = (lastSpace > rowStart) ? (breakAt + 1) : breakAt;
            rowStart       = next;
            rowWidth       = 0;
            lastSpace      = -1;
            for (int back = rowStart; back < idx; ++back) {
                rowWidth += cells[back].width;
            }
        }
        if (cell.character == QLatin1Char(' ')) {
            lastSpace = idx;
        }
        rowWidth += cell.width;
    }
    if (rowStart < cells.size()) {
        out.append(regroup(rowStart, cells.size()));
    }
    if (out.isEmpty()) {
        out.append(QList<QTuiStyledRun>{});
    }
    return out;
}

/* Block adapter: a single source line of plain text with a LineStyle.
 * Wraps via softWrap and paints with putString so existing diff/dim/
 * bold semantics remain bit-identical to the pre-block scrollview. */
class PlainLineBlock : public QTuiBlock
{
public:
    PlainLineBlock(QString text, QTuiScrollView::LineStyle style)
        : sourceText(std::move(text))
        , style(style)
    {}

    void layout(int width) override
    {
        if (!layoutDirty && layoutWidth == width) {
            return;
        }
        layoutWidth = width;
        layoutDirty = false;
        wrapped.clear();
        if (sourceText.isEmpty()) {
            wrapped.append(QString());
            return;
        }
        wrapped = softWrap(sourceText, width, continuationPrefix());
    }

    int rowCount() const override { return static_cast<int>(wrapped.size()); }

    void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const override
    {
        Q_UNUSED(xOffset);
        Q_UNUSED(width);
        Q_UNUSED(focused);
        Q_UNUSED(selected);
        if (viewportRow < 0 || viewportRow >= wrapped.size()) {
            return;
        }
        bool isDim  = (style == QTuiScrollView::Dim) || (style == QTuiScrollView::DiffContext);
        bool isBold = (style == QTuiScrollView::Bold) || (style == QTuiScrollView::DiffHunk);
        QTuiFgColor fgColor = QTuiFgColor::Default;
        switch (style) {
        case QTuiScrollView::DiffAdd:
            fgColor = QTuiFgColor::Green;
            break;
        case QTuiScrollView::DiffDel:
            fgColor = QTuiFgColor::Red;
            break;
        case QTuiScrollView::DiffHunk:
            fgColor = QTuiFgColor::Yellow;
            break;
        default:
            break;
        }
        screen.putString(0, screenRow, wrapped[viewportRow], isBold, isDim, false, fgColor);
    }

    QString toPlainText() const override { return sourceText; }

private:
    QString continuationPrefix() const
    {
        switch (style) {
        case QTuiScrollView::DiffAdd:
        case QTuiScrollView::DiffDel:
        case QTuiScrollView::DiffContext:
            return QStringLiteral(" ");
        default:
            return {};
        }
    }

    QString                   sourceText;
    QTuiScrollView::LineStyle style;
    QList<QString>            wrapped;
};

/* Block adapter: a single source line of styled runs (markdown
 * inline output, status-line snippets, etc.). */
class StyledLineBlock : public QTuiBlock
{
public:
    explicit StyledLineBlock(QList<QTuiStyledRun> runs)
        : sourceRuns(std::move(runs))
    {}

    void layout(int width) override
    {
        if (!layoutDirty && layoutWidth == width) {
            return;
        }
        layoutWidth = width;
        layoutDirty = false;
        wrapped     = wrapStyledRuns(sourceRuns, width);
        if (wrapped.isEmpty()) {
            wrapped.append(QList<QTuiStyledRun>{});
        }
    }

    int rowCount() const override { return static_cast<int>(wrapped.size()); }

    void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const override
    {
        Q_UNUSED(xOffset);
        Q_UNUSED(focused);
        Q_UNUSED(selected);
        if (viewportRow < 0 || viewportRow >= wrapped.size()) {
            return;
        }
        int col = 0;
        for (const QTuiStyledRun &run : wrapped[viewportRow]) {
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

    QString toPlainText() const override
    {
        QString out;
        for (const QTuiStyledRun &run : sourceRuns) {
            out += run.text;
        }
        return out;
    }

private:
    QList<QTuiStyledRun>        sourceRuns;
    QList<QList<QTuiStyledRun>> wrapped;
};

} // namespace

void QTuiScrollView::appendLine(const QString &text, LineStyle style)
{
    blocks.push_back(std::make_unique<PlainLineBlock>(text, style));
    while (static_cast<int>(blocks.size()) > MAX_BLOCKS) {
        blocks.erase(blocks.begin());
        if (scrollOffset > 0) {
            scrollOffset--;
        }
    }
}

void QTuiScrollView::appendStyledLine(const QList<QTuiStyledRun> &runs)
{
    blocks.push_back(std::make_unique<StyledLineBlock>(runs));
    while (static_cast<int>(blocks.size()) > MAX_BLOCKS) {
        blocks.erase(blocks.begin());
        if (scrollOffset > 0) {
            scrollOffset--;
        }
    }
}

void QTuiScrollView::replaceLastStyledLine(const QList<QTuiStyledRun> &runs)
{
    if (blocks.empty()) {
        appendStyledLine(runs);
        return;
    }
    blocks.back() = std::make_unique<StyledLineBlock>(runs);
}

void QTuiScrollView::appendBlock(std::unique_ptr<QTuiBlock> block)
{
    if (block == nullptr) {
        return;
    }
    blocks.push_back(std::move(block));
    while (static_cast<int>(blocks.size()) > MAX_BLOCKS) {
        blocks.erase(blocks.begin());
        if (scrollOffset > 0) {
            scrollOffset--;
        }
    }
}

void QTuiScrollView::appendPartial(const QString &text, LineStyle style)
{
    /* Style transition mid-stream: lock down the prior fragment under
     * its original style first so a follow-up render does not
     * retroactively recolor unfinished prose. */
    if (style != partialStyle && !partialLine.isEmpty()) {
        appendLine(partialLine, partialStyle);
        partialLine.clear();
    }
    partialStyle = style;
    partialLine += text;

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
    int contentWidth = width - 1;
    if (contentWidth < 1) {
        contentWidth = width;
    }

    /* Layout every block at the current width so per-block row counts
     * stack into a single virtual row coordinate space. */
    int totalVisible = 0;
    for (const auto &block : blocks) {
        block->layout(contentWidth);
        totalVisible += block->rowCount();
    }

    /* Streaming partial line is treated as a virtual trailing block
     * for paint purposes. Avoids reallocating a real PlainLineBlock
     * on every keystroke when the line will get absorbed on \n. */
    QList<QString> partialRows;
    if (!partialLine.isEmpty()) {
        const QString contPrefix = (partialStyle == DiffAdd || partialStyle == DiffDel
                                    || partialStyle == DiffContext)
                                       ? QStringLiteral(" ")
                                       : QString();
        partialRows              = softWrap(partialLine, contentWidth, contPrefix);
        totalVisible += partialRows.size();
    }

    const int viewBottom = totalVisible - scrollOffset;
    const int viewTop    = viewBottom - height;

    /* Walk blocks once, mapping viewport rows to (block, rowInBlock). */
    int globalRow = 0;
    for (const auto &block : blocks) {
        const int rows = block->rowCount();
        if (globalRow + rows <= viewTop) {
            globalRow += rows;
            continue;
        }
        if (globalRow >= viewBottom) {
            break;
        }
        for (int rowInBlock = 0; rowInBlock < rows; ++rowInBlock) {
            const int gRow = globalRow + rowInBlock;
            if (gRow < viewTop || gRow >= viewBottom) {
                continue;
            }
            const int screenY = startRow + (gRow - viewTop);
            block->paintRow(screen, screenY, rowInBlock, 0, contentWidth, false, false);
        }
        globalRow += rows;
    }

    /* Paint any partial-line rows after the blocks. */
    for (int idx = 0; idx < partialRows.size(); ++idx) {
        const int gRow = globalRow + idx;
        if (gRow < viewTop || gRow >= viewBottom) {
            continue;
        }
        const int   screenY = startRow + (gRow - viewTop);
        bool        isDim   = (partialStyle == Dim) || (partialStyle == DiffContext);
        bool        isBold  = (partialStyle == Bold) || (partialStyle == DiffHunk);
        QTuiFgColor fgColor = QTuiFgColor::Default;
        switch (partialStyle) {
        case DiffAdd:
            fgColor = QTuiFgColor::Green;
            break;
        case DiffDel:
            fgColor = QTuiFgColor::Red;
            break;
        case DiffHunk:
            fgColor = QTuiFgColor::Yellow;
            break;
        default:
            break;
        }
        screen.putString(0, screenY, partialRows[idx], isBold, isDim, false, fgColor);
    }

    /* ASCII scrollbar on the rightmost column. */
    if (width > 1) {
        const int scrollCol = width - 1;
        if (totalVisible <= height || height <= 0) {
            for (int row = 0; row < height; ++row) {
                screen.putChar(scrollCol, startRow + row, ' ', false, true);
            }
        } else {
            int       thumbSize   = qBound(1, height * height / totalVisible, height);
            const int scrollRange = totalVisible - height;
            const int clampedOff  = qBound(0, scrollOffset, scrollRange);
            const int currentPos  = scrollRange - clampedOff;
            int       thumbTop    = 0;
            if (scrollRange > 0) {
                thumbTop = currentPos * (height - thumbSize) / scrollRange;
            }
            thumbTop              = qBound(0, thumbTop, height - thumbSize);
            const int thumbBottom = thumbTop + thumbSize;
            for (int row = 0; row < height; ++row) {
                const QChar scrollChar = (row >= thumbTop && row < thumbBottom) ? QChar('#')
                                                                                : QChar('|');
                screen.putChar(scrollCol, startRow + row, scrollChar, false, true);
            }
        }
    }
}

void QTuiScrollView::scrollUp(int count)
{
    /* Soft cap: blocks count is a lower bound on display-row count.
     * The actual cap depends on width-driven wrapping; render() clamps
     * once it knows totalVisible. */
    const int softCap = (static_cast<int>(blocks.size()) + 1) * 8;
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
    for (const auto &block : blocks) {
        result += block->toPlainText();
        result += QLatin1Char('\n');
    }
    if (!partialLine.isEmpty()) {
        result += partialLine;
    }
    return result;
}

void QTuiScrollView::clear()
{
    blocks.clear();
    partialLine.clear();
    scrollOffset = 0;
}
