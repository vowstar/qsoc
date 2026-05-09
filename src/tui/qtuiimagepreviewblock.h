// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIIMAGEPREVIEWBLOCK_H
#define QTUIIMAGEPREVIEWBLOCK_H

#include "tui/qtuiblock.h"

#include <QByteArray>
#include <QString>

/**
 * @brief Inline image preview block for read_file / web_fetch images.
 * @details Holds the raw image bytes plus enough metadata to render
 *          either a graphical preview (Sixel / iTerm2 / Kitty
 *          graphics) or a textual placeholder. The cell-grid paint
 *          path always renders the placeholder; the cooked-mode
 *          toAnsi() path emits the right escape sequence based on a
 *          one-shot protocol probe so the user sees the actual image
 *          inline in their terminal scrollback after the agent exits.
 *
 *          Detection priority (env-only, no DA1 probe):
 *          - Kitty graphics: $KITTY_WINDOW_ID, $GHOSTTY_RESOURCES_DIR,
 *            $WEZTERM_EXECUTABLE, $KONSOLE_VERSION, $TERM in
 *            {xterm-kitty, xterm-ghostty}, or $TERM_PROGRAM in
 *            {ghostty, wezterm, rio}
 *          - iTerm2 inline: $TERM_PROGRAM in {iterm.app, vscode,
 *            mintty} or $TERM=mintty
 *          - Sixel placeholder: $TERM matches foot|mlterm|contour
 *            (no encoder bundled yet)
 *          - otherwise: text placeholder only
 */
class QTuiImagePreviewBlock : public QTuiBlock
{
public:
    QTuiImagePreviewBlock(
        QString sourceLabel, QString mimeType, int widthPx, int heightPx, QByteArray bytes);

    void layout(int width) override;
    int  rowCount() const override;
    void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const override;

    QString toPlainText() const override;
    QString toMarkdown() const override;

    /* Inject the chosen graphics protocol's payload before the
     * placeholder text so the actual image lands in the user's
     * normal scrollback alongside the metadata. */
    QString toAnsi(int width) override;

    /* Live alt-screen overlay: transmit the PNG once, then re-place
     * it at the block's current cell rectangle on every frame. The
     * placement re-emission is small (~30 bytes) and idempotent in
     * the kitty graphics protocol, so per-frame cost stays trivial
     * even at the 100 ms compositor tick. */
    QString emitGraphicsLayer(
        int firstScreenRow, int firstScreenCol, int contentWidth, int visibleRows) const override;

    /* Erase the live placement so the cell rectangle returns to
     * blank when the block scrolls out of the viewport. The transmit
     * cache is kept so a scroll back in only re-emits the small
     * placement escape, not the full bitmap upload. */
    QString emitGraphicsClear() const override;

    /* Free the bitmap from the terminal cache on compositor shutdown
     * so qsoc does not leak megabytes of image data into the host
     * terminal's memory after exit. */
    QString emitGraphicsDestroy() const override;

    /* Image previews participate in fold so older bitmaps collapse
     * into their `[image: ...]` line when a newer image arrives.
     * Folding drops the cell rectangle from layout(), suppresses
     * future placement escapes, and the scroll view emits a clear
     * for any placement that was active on the prior frame. */
    bool isFoldable() const override { return true; }

    /* Cell-grid footprint reserved for the eventual graphics overlay.
     * Zero on text-only terminals; positive when a graphics protocol
     * is available so subsequent compositor frames can paint a real
     * image into the same rectangle. */
    int imageCellRows() const { return cellRows; }
    int imageCellCols() const { return cellCols; }

private:
    QString    sourceLabel;
    QString    mimeType;
    int        widthPx  = 0;
    int        heightPx = 0;
    QByteArray bytes;

    int cellRows = 0;
    int cellCols = 0;

    /* Kitty graphics state machine. The first emitGraphicsLayer call
     * uploads the PNG bytes once with `a=t,i=<id>` so subsequent
     * frames only need a small `a=p` placement at the new cursor
     * position. Mutable because emitGraphicsLayer is logically
     * `const` from the scroll view's point of view but caches the
     * upload-once decision. */
    struct KittyState
    {
        quint32 imageId     = 0;
        bool    transmitted = false;
    };
    mutable KittyState kittyState;

    /* iTerm2 has no transmit-once primitive: every place embeds the
     * full base64 again. Throttle by remembering the last placement
     * coords; we only re-emit when the block has actually moved
     * (scroll, layout change). The same-coord skip means a stable
     * frame emits nothing for the image, since iTerm2 keeps the
     * pixels until the underlying cells get overwritten. */
    mutable int iTerm2LastRow = -1;
    mutable int iTerm2LastCol = -1;

    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUIIMAGEPREVIEWBLOCK_H
