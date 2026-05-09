// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiscrollview.h"

#include "tui/qtuiimagepreviewblock.h"
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
        previousVisibleBlocks_.clear();
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
        previousVisibleBlocks_.clear();
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

    /* Auto-fold prior image previews when a new image arrives so the
     * chat history keeps only the latest bitmap rendered as graphics
     * and older ones collapse into their `[image: ...]` line. The
     * scroll view's collectGraphicsLayer drops the placements of the
     * newly folded blocks on the next frame so the cells return to
     * normal text flow. */
    if (dynamic_cast<QTuiImagePreviewBlock *>(block.get()) != nullptr) {
        for (auto &existing : blocks) {
            if (dynamic_cast<QTuiImagePreviewBlock *>(existing.get()) != nullptr) {
                existing->setFolded(true);
            }
        }
    }

    blocks.push_back(std::move(block));
    while (static_cast<int>(blocks.size()) > MAX_BLOCKS) {
        blocks.erase(blocks.begin());
        previousVisibleBlocks_.clear();
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

    lastRenderStartRow_ = startRow;
    lastRenderHeight_   = height;
    lastRenderWidth_    = contentWidth;
    rowToBlock_.assign(static_cast<std::size_t>(qMax(0, height)), -1);
    visibleGraphicsEntries_.clear();

    /* Walk blocks once, mapping viewport rows to (block, rowInBlock).
     * Records each painted screen row's owning block index so a later
     * mouse hit test can resolve a click back to a block. */
    int globalRow = 0;
    int blockIdx  = 0;
    for (const auto &block : blocks) {
        const int rows = block->rowCount();
        if (globalRow + rows <= viewTop) {
            globalRow += rows;
            ++blockIdx;
            continue;
        }
        if (globalRow >= viewBottom) {
            ++blockIdx;
            break;
        }
        /* Record the first visible screen row of this block so the
         * graphics-layer pass can position its escapes correctly.
         * `firstScreenRow` is 1-based to match CSI cursor-position. */
        {
            const int blockTopGRow      = qMax(globalRow, viewTop);
            const int firstScreenRowOne = (startRow + (blockTopGRow - viewTop)) + 1;
            visibleGraphicsEntries_.push_back(
                VisibleGraphicsEntry{block.get(), firstScreenRowOne, /*col=*/1, contentWidth});
        }
        const bool focused = (blockIdx == focusedBlockIdx_);
        for (int rowInBlock = 0; rowInBlock < rows; ++rowInBlock) {
            const int gRow = globalRow + rowInBlock;
            if (gRow < viewTop || gRow >= viewBottom) {
                continue;
            }
            const int screenY  = startRow + (gRow - viewTop);
            const int rowLocal = screenY - lastRenderStartRow_;
            if (rowLocal >= 0 && rowLocal < static_cast<int>(rowToBlock_.size())) {
                rowToBlock_[rowLocal] = blockIdx;
            }
            block->paintRow(
                screen, screenY, rowInBlock, block->xOffset(), contentWidth, focused, false);
            if (focused) {
                /* Subtle bg tint across the painted row so the focused
                 * block stands out without overdrawing block content.
                 * Cells with an explicit non-default bg keep theirs. */
                for (int col = 0; col < contentWidth; ++col) {
                    QTuiCell &cell = screen.at(col, screenY);
                    if (cell.bgColor == BG_DEFAULT) {
                        cell.bgColor = 235; /* dark gray */
                    }
                }
            }
        }
        globalRow += rows;
        ++blockIdx;
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

    /* Vertical scrollbar on the rightmost column. The track always
     * shows as a dim │ so the right edge of the scroll area is a
     * visible border, even when content fits the viewport. When the
     * content overflows, an opaque █ thumb segment indicates scroll
     * position over the track. */
    if (width > 1) {
        const int   scrollCol = width - 1;
        const QChar trackChar(0x2502); /* │ */
        const QChar thumbChar(0x2588); /* █ */
        auto markDecorative = [&](int rowY) { screen.at(scrollCol, rowY).decorative = true; };
        if (totalVisible <= height || height <= 0) {
            for (int row = 0; row < height; ++row) {
                screen.putChar(scrollCol, startRow + row, trackChar, false, true);
                markDecorative(startRow + row);
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
                const bool  inThumb   = (row >= thumbTop && row < thumbBottom);
                const QChar character = inThumb ? thumbChar : trackChar;
                /* Track is dim, thumb is bold (not dim) to stand out. */
                screen.putChar(
                    scrollCol,
                    startRow + row,
                    character,
                    /*bold=*/inThumb,
                    /*dim=*/!inThumb);
                markDecorative(startRow + row);
            }
        }
    }
}

void QTuiScrollView::setFocusedBlockIdx(int idx)
{
    if (idx < -1 || idx >= static_cast<int>(blocks.size())) {
        focusedBlockIdx_ = -1;
        return;
    }
    focusedBlockIdx_ = idx;
}

int QTuiScrollView::blockAtScreenRow(int screenRow) const
{
    const int local = screenRow - lastRenderStartRow_;
    if (local < 0 || local >= static_cast<int>(rowToBlock_.size())) {
        return -1;
    }
    return rowToBlock_[local];
}

QString QTuiScrollView::copyFocusedAsMarkdown() const
{
    if (focusedBlockIdx_ < 0 || focusedBlockIdx_ >= static_cast<int>(blocks.size())) {
        return {};
    }
    return blocks[focusedBlockIdx_]->toMarkdown();
}

QString QTuiScrollView::copyFocusedAsPlainText() const
{
    if (focusedBlockIdx_ < 0 || focusedBlockIdx_ >= static_cast<int>(blocks.size())) {
        return {};
    }
    return blocks[focusedBlockIdx_]->toPlainText();
}

void QTuiScrollView::toggleFocusedFold()
{
    if (focusedBlockIdx_ < 0 || focusedBlockIdx_ >= static_cast<int>(blocks.size())) {
        return;
    }
    auto &block = blocks[focusedBlockIdx_];
    if (!block->isFoldable()) {
        return;
    }
    block->setFolded(!block->isFolded());
}

void QTuiScrollView::scrollFocusedHorizontal(int delta)
{
    if (focusedBlockIdx_ < 0 || focusedBlockIdx_ >= static_cast<int>(blocks.size())) {
        return;
    }
    auto     &block     = blocks[focusedBlockIdx_];
    const int maxOffset = block->maxXOffset(qMax(1, lastRenderWidth_));
    if (maxOffset <= 0) {
        return;
    }
    int newOffset = block->xOffset() + delta;
    if (newOffset < 0) {
        newOffset = 0;
    }
    if (newOffset > maxOffset) {
        newOffset = maxOffset;
    }
    block->setXOffset(newOffset);
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

QString QTuiScrollView::toAnsi(int width)
{
    if (width <= 0) {
        return QString();
    }
    QString out;
    for (auto &block : blocks) {
        out.append(block->toAnsi(width));
    }
    if (!partialLine.isEmpty()) {
        out.append(partialLine);
        if (!partialLine.endsWith(QLatin1Char('\n'))) {
            out.append(QLatin1Char('\n'));
        }
    }
    return out;
}

QString QTuiScrollView::collectGraphicsLayer()
{
    QString out;

    /* "Eligible" = visible this frame AND not folded. A folded block
     * contributes no payload of its own and any prior placement must
     * be cleared so a freshly folded image collapses cleanly. */
    std::vector<QTuiBlock *> eligibleBlocks;
    eligibleBlocks.reserve(visibleGraphicsEntries_.size());
    for (const auto &entry : visibleGraphicsEntries_) {
        if (entry.block != nullptr && !entry.block->isFolded()) {
            eligibleBlocks.push_back(entry.block);
        }
    }

    /* Diff: blocks that contributed graphics last frame but no longer
     * do (left the viewport, or folded into a single line) receive a
     * clear so the terminal drops their stale placement. */
    for (QTuiBlock *prev : previousVisibleBlocks_) {
        if (prev == nullptr) {
            continue;
        }
        const bool stillEligible = std::find(eligibleBlocks.begin(), eligibleBlocks.end(), prev)
                                   != eligibleBlocks.end();
        if (!stillEligible) {
            out.append(prev->emitGraphicsClear());
        }
    }

    /* Place every eligible block. The block's own state machine
     * decides whether to also re-emit the bitmap upload or just the
     * placement escape. */
    for (const auto &entry : visibleGraphicsEntries_) {
        if (entry.block == nullptr || entry.block->isFolded()) {
            continue;
        }
        out.append(
            entry.block->emitGraphicsLayer(entry.firstScreenRow, entry.firstScreenCol, entry.width));
    }

    previousVisibleBlocks_ = std::move(eligibleBlocks);
    return out;
}

QString QTuiScrollView::collectGraphicsDestroy() const
{
    QString out;
    for (const auto &block : blocks) {
        if (block == nullptr) {
            continue;
        }
        out.append(block->emitGraphicsDestroy());
    }
    return out;
}

void QTuiScrollView::clear()
{
    blocks.clear();
    partialLine.clear();
    scrollOffset = 0;
    previousVisibleBlocks_.clear();
    visibleGraphicsEntries_.clear();
}
