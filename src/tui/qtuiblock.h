// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIBLOCK_H
#define QTUIBLOCK_H

#include "tui/qtuiscreen.h"

#include <QString>

/**
 * @brief Abstract content unit inside the scroll viewport.
 * @details Each block owns its own layout, paint, fold, and copy
 *          policies. The scroll view stacks blocks vertically and
 *          treats them as opaque rendering primitives so different
 *          content kinds (assistant prose, tool calls, diffs,
 *          tables, code, images, ...) can pick the right strategy
 *          for wrapping, horizontal scroll, and clipboard output.
 *
 *          Lifecycle:
 *          1. The owner mutates the block via its kind-specific
 *             setters (e.g. AssistantTextBlock::appendMarkdown).
 *             Mutations call invalidate() to drop layout cache.
 *          2. Before paint, the scroll view calls layout(width)
 *             with the current viewport width. layout() recomputes
 *             cached display rows iff width changed or the cache
 *             was invalidated.
 *          3. paintRow is then called once per visible row.
 *
 *          Copy:
 *          - toPlainText() returns the user-facing text without
 *            ANSI escapes or decorations (spaces preserved). Used
 *            for plain-text yank.
 *          - toMarkdown() returns a faithful markdown reconstruction
 *            so a copied block can round-trip through the next
 *            agent turn intact.
 *          The two may be identical for blocks whose source already
 *          is markdown.
 */
class QTuiBlock
{
public:
    QTuiBlock()                             = default;
    virtual ~QTuiBlock()                    = default;
    QTuiBlock(const QTuiBlock &)            = delete;
    QTuiBlock &operator=(const QTuiBlock &) = delete;
    QTuiBlock(QTuiBlock &&)                 = delete;
    QTuiBlock &operator=(QTuiBlock &&)      = delete;

    /**
     * @brief Compute or refresh the cached display rows for a viewport
     *        width. Idempotent: subsequent calls with the same width
     *        on a non-dirty cache are no-ops.
     */
    virtual void layout(int width) = 0;

    /**
     * @brief Total number of display rows produced by the last layout()
     *        call. Always 0 if layout() has never been called.
     */
    virtual int rowCount() const = 0;

    /**
     * @brief Paint a single display row to the screen.
     * @param screen      The screen buffer to paint into.
     * @param screenRow   Y coordinate inside the screen.
     * @param viewportRow Row index inside this block (0-based).
     * @param xOffset     Horizontal scroll offset in cells. Blocks
     *                    that report maxXOffset>0 are expected to
     *                    honour this; others ignore it.
     * @param width       Drawable width in cells (excluding scrollbar).
     * @param focused     True when the cursor focus is on this block.
     * @param selected    True when the block is part of an active
     *                    multi-block selection.
     */
    virtual void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const = 0;

    /**
     * @brief True if the block is allowed to fold/unfold. Default
     *        false; concrete blocks opt in.
     */
    virtual bool isFoldable() const { return false; }

    /**
     * @brief Current fold state. Folded blocks render a single
     *        summary row regardless of their underlying content.
     */
    bool isFolded() const { return folded; }

    /**
     * @brief Toggle fold state. Always invalidates the layout cache.
     */
    void setFolded(bool foldedFlag)
    {
        if (folded != foldedFlag) {
            folded = foldedFlag;
            invalidate();
        }
    }

    /**
     * @brief Maximum horizontal scroll offset for the given viewport
     *        width. Returns 0 when the block fits and does not need
     *        horizontal scroll (default).
     */
    virtual int maxXOffset(int width) const
    {
        Q_UNUSED(width);
        return 0;
    }

    /**
     * @brief Current horizontal scroll offset. The render loop hands
     *        this to paintRow; concrete blocks decide whether to honour
     *        it. The base accessor is shared so the scrollview can
     *        drive scroll without each block reimplementing storage.
     */
    int  xOffset() const { return xOffset_; }
    void setXOffset(int offset)
    {
        if (offset < 0) {
            offset = 0;
        }
        if (xOffset_ != offset) {
            xOffset_ = offset;
            invalidate();
        }
    }

    /**
     * @brief Plain-text content for clipboard yank. Excludes any
     *        decorative gutters, borders, fold markers, ANSI codes.
     */
    virtual QString toPlainText() const = 0;

    /**
     * @brief Markdown round-trip representation. Defaults to plain
     *        text; concrete blocks override to emit fenced code,
     *        GFM tables, unified diff, etc.
     */
    virtual QString toMarkdown() const { return toPlainText(); }

    /**
     * @brief Drop the layout cache so the next layout() call
     *        recomputes display rows. Called automatically by fold
     *        toggle; concrete blocks must call it from any setter
     *        that mutates content.
     */
    virtual void invalidate()
    {
        layoutDirty = true;
        layoutWidth = -1;
    }

    /**
     * @brief Emit the block as a sequence of ANSI-encoded lines.
     * @details Used by cooked-mode (--print) output and the alt-screen
     *          exit path so the scrollback content survives as
     *          colored text in the user's terminal scroll history.
     *          The default implementation lays out the block at
     *          `width`, paints each row into a freshly-sized
     *          QTuiScreen, and emits one SGR-encoded line per row.
     *          Concrete blocks can override to inject extra escape
     *          payloads (for instance, the image preview block emits
     *          a Kitty / iTerm2 graphics escape ahead of the cell
     *          grid so the actual bitmap shows up inline).
     */
    virtual QString toAnsi(int width);

    /**
     * @brief Emit a raw escape payload that overlays the cell grid
     *        for one frame of live alt-screen rendering.
     * @details Called by the scroll view after the cell-grid pass
     *          on every block currently visible in the viewport.
     *          The default implementation returns an empty string
     *          so non-graphical blocks contribute nothing. Concrete
     *          graphical blocks (for instance the image preview
     *          block) move the cursor with `\x1b[<row>;<col>H` and
     *          emit Kitty / iTerm2 graphics escapes at the cell
     *          coordinates the cell-grid pass already reserved for
     *          them. The compositor pipes the result straight to
     *          stdout after the cell grid, so the graphics paint
     *          on top of the empty placeholder cells.
     * @param firstScreenRow 1-based screen row of the block's first
     *        visible viewport row. Suitable for use directly in a
     *        CSI cursor-position escape.
     * @param firstScreenCol 1-based screen column of the block's
     *        leftmost cell after the scrollview gutter.
     * @param contentWidth Drawable width in cells available to the
     *        block on the current frame.
     * @return Raw ANSI/CSI bytes to emit, or an empty string when
     *         the block does not need a graphics overlay this frame.
     */
    virtual QString emitGraphicsLayer(int firstScreenRow, int firstScreenCol, int contentWidth) const
    {
        Q_UNUSED(firstScreenRow);
        Q_UNUSED(firstScreenCol);
        Q_UNUSED(contentWidth);
        return {};
    }

    /**
     * @brief Emit an escape that erases this block's live placement.
     * @details Called by the scroll view when the block was visible
     *          on the previous collectGraphicsLayer call but is no
     *          longer visible on the current one. Concrete graphics
     *          blocks return the protocol-specific `delete placement`
     *          escape so the terminal reclaims the cells covered by
     *          the previous frame's image. Default: empty.
     */
    virtual QString emitGraphicsClear() const { return {}; }

    /**
     * @brief Emit an escape that destroys this block's transmitted
     *        image so the terminal can reclaim its bitmap cache.
     * @details Called once per block during compositor shutdown,
     *          before the alt screen unwinds. Default: empty.
     */
    virtual QString emitGraphicsDestroy() const { return {}; }

protected:
    /** Cached width that produced the current row layout. */
    int layoutWidth = -1;
    /** True until layout() satisfies the cache. */
    bool layoutDirty = true;
    /** Fold state. Concrete blocks decide whether to honour it. */
    bool folded = false;
    /** Horizontal scroll offset honoured by blocks that opt in. */
    int xOffset_ = 0;
};

#endif // QTUIBLOCK_H
