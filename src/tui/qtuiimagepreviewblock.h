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

    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUIIMAGEPREVIEWBLOCK_H
