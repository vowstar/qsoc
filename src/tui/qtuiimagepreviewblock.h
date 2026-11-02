// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIIMAGEPREVIEWBLOCK_H
#define QTUIIMAGEPREVIEWBLOCK_H

#include "tui/qtuiblock.h"
#include "tui/qtuigraphics.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <functional>
#include <memory>

/**
 * @brief One image in the transcript: a metadata line above its picture.
 * @details The picture is a terminal bitmap where the protocol allows one and
 *          half-blocks painted into the cells otherwise. Cells also stand in
 *          for an inline bitmap the viewport clips.
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

    GraphicsState graphicsState(
        int                 firstScreenRow,
        int                 firstScreenCol,
        int                 contentWidth,
        int                 visibleRows,
        const QVector<int> &writtenRows = {}) const override;
    QRect graphicsEraseRect() const override;
    void  resetGraphicsState() const override;

    QString emitGraphicsLayer(
        int firstScreenRow, int firstScreenCol, int contentWidth, int visibleRows) const override;
    QString emitGraphicsClear() const override;
    QString emitGraphicsDestroy() const override;

    /* Folding drops the cell rectangle from layout(), suppresses
     * future placement escapes, and the scroll view emits a clear
     * for any placement that was active on the prior frame. */
    bool isFoldable() const override { return true; }

    /* Draw SVG source into this block; @p changed runs after the result lands. */
    void drawSvg(const QByteArray &source, std::function<void(bool drawn)> changed);

    /* The saved file behind this image; its metadata name links to it. */
    void    setSourceFile(const QString &path) { sourceFile = path; }
    QString label() const { return sourceLabel; }

    qint64 imageByteSize() const { return bytes.size(); }
    /* The block keeps its metadata line after the bitmap goes. */
    void releaseImageBytes();

    int imageCellRows() const { return cellRows; }
    int imageCellCols() const { return cellCols; }

private:
    QString    sourceLabel;
    QString    mimeType;
    int        widthPx  = 0;
    int        heightPx = 0;
    QByteArray bytes;
    qint64     byteSize = 0;

    int cellRows = 0;
    int cellCols = 0;

    /* Image rows of the block on screen: the first one and how many. */
    struct ImageWindow
    {
        int top   = 0;
        int count = 0;
    };
    ImageWindow imageWindow(int visibleRows) const;
    void        paintCells(QTuiScreen &screen, int screenRow, int imageRow, int width) const;
    void        paintPlaceholders(QTuiScreen &screen, int screenRow, int imageRow, int width) const;
    bool        transmitKitty(QString *out) const;
    QRect       placementFor(
        int firstScreenRow, int firstScreenCol, int contentWidth, const ImageWindow &window) const;
    QString emitPlaceholders(const ImageWindow &window) const;
    QString inlinePayload(const QSize &cells) const;

    /* Mutable because emitGraphicsLayer is logically `const` from the scroll
     * view's point of view but caches the upload-once decision. */
    mutable quint32 kittyImageId = 0;

    mutable QRect placementRect;
    mutable int   placementTop = 0;

    QString sourceFile;
    bool    released      = false;
    int     layoutMaxRows = 0;

    QTuiGraphics::Protocol protocol_;
    bool                   placeholders_;

    /* Derived pictures, built when first shown. */
    mutable QImage  cellRaster;
    mutable QSize   inlineCells;
    mutable QString inlineData;
    mutable QSize   placedCells;

    /* Replaces the size statistics on the metadata line while set. */
    QString               status;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUIIMAGEPREVIEWBLOCK_H
