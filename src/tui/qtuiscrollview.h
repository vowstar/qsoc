// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISCROLLVIEW_H
#define QTUISCROLLVIEW_H

#include "tui/qtuiblock.h"
#include "tui/qtuiscreen.h"

#include <QList>
#include <QStringList>

#include <memory>
#include <vector>

/**
 * @brief Scrollable text content area with line history
 * @details Stores all output lines. Renders a viewport window.
 *          Auto-scrolls to bottom when new content arrives (unless user scrolled up).
 *          Each line can be normal, dim (thinking), or bold (tool), or
 *          carry a list of styled runs for mixed-style markdown output.
 */
class QTuiScrollView
{
public:
    enum LineStyle : int {
        Normal      = 0,
        Dim         = 1,
        Bold        = 2,
        DiffAdd     = 3, /* '+' lines, rendered green */
        DiffDel     = 4, /* '-' lines, rendered red */
        DiffHunk    = 5, /* '@@' headers, rendered yellow + bold */
        DiffContext = 6, /* unchanged context lines, dim */
        Styled      = 7, /* runs[] holds mixed-style segments; text unused */
    };

    /* Append a complete line */
    void appendLine(const QString &text, LineStyle style = Normal);

    /* Append a styled-runs line. Each run carries its own bold/italic/
     * dim/underline/fg/bg attributes; render() paints them cell-by-cell
     * with soft-wrap that preserves run boundaries. */
    void appendStyledLine(const QList<QTuiStyledRun> &runs);

    /* Replace the most-recently-appended line with a new styled-runs
     * payload. Used by the streaming markdown pipeline so the
     * in-progress final line gets repainted on every chunk without
     * trailing duplicates. No-op if the buffer is empty. */
    void replaceLastStyledLine(const QList<QTuiStyledRun> &runs);

    /* Append a generic block. Takes ownership; the scrollview is the
     * sole owner of every block in its history. */
    void appendBlock(std::unique_ptr<QTuiBlock> block);

    /* Append partial text (streaming). Completes on \n. */
    void appendPartial(const QString &text, LineStyle style = Normal);

    /* Render the viewport to a screen buffer region.
     * Reserves 1 column on the right for ASCII scrollbar. */
    void render(QTuiScreen &screen, int startRow, int height, int width);

    static constexpr int MAX_BLOCKS = 65536;
    static constexpr int MAX_LINES  = MAX_BLOCKS; /* Deprecated alias */

    /* Scroll control */
    void scrollUp(int lines = 1);
    void scrollDown(int lines = 1);
    void scrollToBottom();
    bool isAtBottom() const;

    /* Clear all content */
    void clear();

    int totalLines() const { return static_cast<int>(blocks.size()); }

    /* Non-owning pointer to the last block in the scrollback (or
     * nullptr if empty). Lets a streaming consumer detect when an
     * unrelated block has landed in between two chunks so it can
     * start a fresh block instead of retroactively appending out of
     * visual order. */
    QTuiBlock *lastBlock() const { return blocks.empty() ? nullptr : blocks.back().get(); }

    /* Block-aware focus and copy.
     *
     * focusedBlockIdx == -1 means no block is focused; render() applies
     * a bg tint to every cell of the focused block so users can see at
     * a glance which one will be copied. blockAtScreenRow maps a hit
     * test back to a block index using the row->block table cached on
     * the previous render() call, so inputs from outside (mouse click
     * coordinates from the input monitor) line up with what the user
     * actually saw. */
    int  focusedBlockIdx() const { return focusedBlockIdx_; }
    void setFocusedBlockIdx(int idx);
    int  blockAtScreenRow(int screenRow) const;

    /* Copy: returns toMarkdown() of the focused block, or empty if no
     * block is focused. The caller is responsible for delivering the
     * result to the system clipboard (OSC 52, xclip, etc.). */
    QString copyFocusedAsMarkdown() const;
    QString copyFocusedAsPlainText() const;

    /* Toggle the focused block's fold state if it is foldable. No-op
     * when no block is focused or the block does not support folding. */
    void toggleFocusedFold();

    /* Step the focused block's horizontal scroll by `delta` cells.
     * Direction matches the key: +1 for right, -1 for left. Clamped
     * to [0, maxXOffset(width)]. Width is the most recent layout
     * width, so the caller doesn't need to know the viewport. */
    void scrollFocusedHorizontal(int delta);

    /* Get all content as plain text (for dumping after alt screen exit) */
    QString toPlainText() const;

private:
    std::vector<std::unique_ptr<QTuiBlock>> blocks;
    QString                                 partialLine; /* Current incomplete line */
    LineStyle                               partialStyle     = Normal;
    int                                     scrollOffset     = 0;  /* 0 = at bottom */
    int                                     focusedBlockIdx_ = -1; /* -1 = none */

    /* Cached during the previous render() call: per-screen-row block
     * index, indexed by `screenRow - lastRenderStartRow_`. Lets a hit
     * test convert a mouse click back to a block without re-doing the
     * scroll math. */
    std::vector<int> rowToBlock_;
    int              lastRenderStartRow_ = 0;
    int              lastRenderHeight_   = 0;
    int              lastRenderWidth_    = 0; /* contentWidth of last paint */
};

#endif // QTUISCROLLVIEW_H
