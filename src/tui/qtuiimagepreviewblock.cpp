// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiimagepreviewblock.h"

#include "tui/qtuigraphics.h"
#include "tui/qtuisvg.h"
#include "tui/qtuiwidget.h"

#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QImage>
#include <QProcessEnvironment>
#include <QUrl>

#include <atomic>
#include <utility>

namespace {

using GraphicsProtocol = QTuiGraphics::Protocol;

QString printable(QString text)
{
    for (QChar &character : text) {
        const ushort code = character.unicode();
        if (code < 0x20 || (code >= 0x7f && code < 0xa0))
            character = QLatin1Char(' ');
    }
    return text;
}

QTuiStyledRun coloredRun(const QString &text, QTuiFgColor color, bool bold = false, bool dim = false)
{
    QTuiStyledRun run;
    run.text = text;
    run.bold = bold;
    run.dim  = dim;
    run.fg   = color;
    return run;
}

QString sizeBucket(int bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
}

QString shortMime(const QString &mime)
{
    /* "image/png" -> "png"; lower-cased simple type. Falls back to the
     * full string when the slash is missing. */
    const int slash = mime.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return mime.toLower();
    }
    return mime.mid(slash + 1).toLower();
}

constexpr int kMarkedRows = 30;

QPair<int, int> computeImageCellRect(
    int imageWidthPx, int imageHeightPx, int viewportWidth, int maxRows)
{
    /* Compute the cell rectangle a graphics-capable terminal will
     * use to display the image. A small image keeps its native cell
     * footprint; only oversize images scale down, and the scale-down
     * uses a single ratio on both axes so kitty's `c=W,r=H`
     * placement reproduces the source aspect. */
    if (imageWidthPx <= 0 || imageHeightPx <= 0 || viewportWidth <= 4) {
        return {0, 0};
    }
    const QSize cell       = QTuiGraphics::cellSize();
    const int   nativeCols = (imageWidthPx + cell.width() - 1) / cell.width();
    const int   nativeRows = (imageHeightPx + cell.height() - 1) / cell.height();

    /* Drawable bound: viewport width minus a one-cell gutter, plus a
     * row cap so a portrait does not eat the entire chat. No minimum
     * floor: a 16x16 thumbnail must stay 2x1 cells, not get inflated
     * to a 8x4 blurry rectangle. */
    const int maxCols = qMax(1, viewportWidth - 2);

    if (nativeCols <= maxCols && nativeRows <= maxRows) {
        return {nativeCols, nativeRows};
    }

    /* Oversize on at least one axis: scale BOTH axes by the same
     * ratio (the tighter of the two overruns) so the displayed
     * shape matches the source pixel aspect. Independent clamps
     * would distort the image; kitty's `c,r` placement scales the
     * bitmap to fill exactly W x H cells. */
    const double ratioCols = static_cast<double>(nativeCols) / maxCols;
    const double ratioRows = static_cast<double>(nativeRows) / maxRows;
    const double ratio     = qMax(ratioCols, ratioRows);

    const int cols = qMax(1, static_cast<int>(nativeCols / ratio));
    const int rows = qMax(1, static_cast<int>(nativeRows / ratio));
    return {cols, rows};
}

QString iTermEscape(const QString &filename, const QByteArray &bytes, const QString &size)
{
    QString out = QStringLiteral("\x1b]1337;File=");
    if (!filename.isEmpty()) {
        const QByteArray nameB64 = filename.toUtf8().toBase64();
        out += QStringLiteral("name=") + QString::fromLatin1(nameB64) + QLatin1Char(';');
    }
    out += QStringLiteral("size=%1;").arg(bytes.size());
    out += QStringLiteral("inline=1;%1:").arg(size);
    out += QString::fromLatin1(bytes.toBase64());
    out += QChar(0x07);
    return out;
}

QByteArray reEncodeAsPngIfNeeded(const QByteArray &bytes, const QString &mime)
{
    if (mime.contains(QStringLiteral("png"))) {
        return bytes;
    }
    QImage image;
    if (!image.loadFromData(bytes)) {
        return {};
    }
    QByteArray pngBytes;
    QBuffer    buffer(&pngBytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    if (!image.save(&buffer, "PNG")) {
        return {};
    }
    buffer.close();
    return pngBytes;
}

/* `q=2` keeps the terminal's replies off the agent's stdin. */
QString kittyChunks(const QByteArray &pngBytes, const QString &firstParams)
{
    const QByteArray b64       = pngBytes.toBase64();
    constexpr int    chunkSize = 4096;
    QString          out;
    for (qsizetype offset = 0; offset < b64.size(); offset += chunkSize) {
        const bool    last   = offset + chunkSize >= b64.size();
        const QString params = offset == 0 ? firstParams : QStringLiteral("q=2");
        out += QTuiGraphics::passthrough(
            QStringLiteral("\x1b_G%1,m=%2;").arg(params).arg(last ? 0 : 1)
            + QString::fromLatin1(b64.mid(offset, chunkSize)) + QStringLiteral("\x1b\\"));
    }
    return out;
}

QString kittyTransmitOnly(const QByteArray &pngBytes, quint32 imageId)
{
    if (pngBytes.isEmpty() || imageId == 0) {
        return {};
    }
    return kittyChunks(pngBytes, QStringLiteral("i=%1,a=t,f=100,q=2").arg(imageId));
}

QString kittySourceRect(int widthPx, int heightPx, int imageRows, int top, int count)
{
    if (count >= imageRows)
        return {};
    return QStringLiteral("x=0,y=%1,w=%2,h=%3,")
        .arg(top * heightPx / imageRows)
        .arg(widthPx)
        .arg(count * heightPx / imageRows);
}

QString kittyPlaceAtCursor(
    quint32 imageId, quint32 placementId, const QString &source, int cellCols, int cellRows)
{
    /* `a=p` re-uses the previously transmitted image referenced by
     * `i=<id>`. `p=<pid>` keeps the placement slot stable so back-
     * to-back frames at the same coords are no-ops. `c,r` constrain
     * the image to a cell rectangle so scaling matches the
     * placeholder reservation. `C=1` keeps the cursor where it was
     * before the placement, so the next frame's cursor jump and
     * cell-grid diff stay stable. `q=2` suppresses the OK reply. */
    if (imageId == 0 || cellCols <= 0 || cellRows <= 0) {
        return {};
    }
    return QStringLiteral("\x1b_Ga=p,i=%1,p=%2,%3c=%4,r=%5,C=1,q=2\x1b\\")
        .arg(imageId)
        .arg(placementId)
        .arg(source)
        .arg(cellCols)
        .arg(cellRows);
}

QString kittyInlineEscape(const QByteArray &bytes, const QString &mime)
{
    const QByteArray pngBytes = reEncodeAsPngIfNeeded(bytes, mime);
    return pngBytes.isEmpty() ? QString() : kittyChunks(pngBytes, QStringLiteral("f=100,a=T,q=2"));
}

/* One half-block cell: the top pixel in the foreground, the bottom in the
 * background. A transparent half keeps the terminal's own background. */
void paintHalfBlock(QTuiCell &cell, QRgb top, QRgb bottom, bool trueColor)
{
    const bool topOpaque    = qAlpha(top) >= 128;
    const bool bottomOpaque = qAlpha(bottom) >= 128;
    if (!topOpaque && !bottomOpaque)
        return;
    const auto rgb = [](QRgb value) {
        return kRgbSet | (static_cast<std::uint32_t>(value) & 0xffffff);
    };
    const auto palette = [](QRgb value) {
        return static_cast<std::uint8_t>(
            16 + 36 * (qRed(value) * 5 / 255) + 6 * (qGreen(value) * 5 / 255)
            + qBlue(value) * 5 / 255);
    };
    const QRgb fg = topOpaque ? top : bottom;
    cell.setScalar(topOpaque ? 0x2580 : 0x2584);
    cell.decorative = true;
    if (trueColor) {
        cell.fgRgb = rgb(fg);
        if (topOpaque && bottomOpaque)
            cell.bgRgb = rgb(bottom);
    } else {
        cell.fgColor = static_cast<QTuiFgColor>(palette(fg));
        if (topOpaque && bottomOpaque)
            cell.bgColor = palette(bottom);
    }
}

} // namespace

QTuiImagePreviewBlock::QTuiImagePreviewBlock(
    QString sourceLabel, QString mimeType, int widthPx, int heightPx, QByteArray bytes)
    : sourceLabel(std::move(sourceLabel))
    , mimeType(std::move(mimeType))
    , widthPx(widthPx)
    , heightPx(heightPx)
    , bytes(std::move(bytes))
    , protocol_(QTuiGraphics::protocol())
    , placeholders_(QTuiGraphics::usesPlaceholders())
{
    this->sourceLabel = printable(this->sourceLabel);
    this->mimeType    = printable(this->mimeType);
}

void QTuiImagePreviewBlock::drawSvg(const QByteArray &source, std::function<void(bool)> changed)
{
    status      = QStringLiteral("drawing");
    layoutDirty = true;
    QTuiSvg::rasterize(
        source,
        [this,
         life    = std::weak_ptr<bool>(alive),
         changed = std::move(changed)](const QByteArray &png, const QString &error) {
            if (life.expired())
                return;
            const QImage image = QImage::fromData(png);
            if (image.isNull()) {
                status = printable(QStringLiteral("not drawn: %1").arg(error)).left(160);
            } else {
                status   = QString();
                bytes    = png;
                mimeType = QStringLiteral("image/png");
                widthPx  = image.width();
                heightPx = image.height();
                setFolded(false);
            }
            layoutDirty = true;
            changed(!image.isNull());
        });
}

void QTuiImagePreviewBlock::releaseImageBytes()
{
    byteSize    = bytes.size();
    bytes       = QByteArray();
    released    = true;
    cellRaster  = QImage();
    inlineData  = QString();
    inlineCells = QSize();
    setFolded(true);
}

void QTuiImagePreviewBlock::layout(int width)
{
    const int maxRows = QTuiGraphics::maxImageRows();
    if (!layoutDirty && layoutWidth == width && layoutMaxRows == maxRows) {
        return;
    }
    layoutDirty   = false;
    layoutMaxRows = maxRows;
    protocol_     = QTuiGraphics::protocol();
    placeholders_ = QTuiGraphics::usesPlaceholders();
    layoutWidth   = width;
    rendered.clear();
    cellRows   = 0;
    cellCols   = 0;
    cellRaster = QImage();

    /* Metadata row: [image: <name>  png 800x600 32 KB]. Always emitted
     * so the user can identify the file even when no graphics protocol
     * is available. */
    QList<QTuiStyledRun> row;
    row.append(coloredRun(QStringLiteral("[image: "), QTuiFgColor::Cyan, /*bold=*/true));
    {
        QFileInfo     fi(sourceLabel);
        const QString display = fi.fileName().isEmpty() ? sourceLabel : fi.fileName();
        QTuiStyledRun name;
        name.text = display;
        if (!sourceFile.isEmpty())
            name.hyperlink = QUrl::fromLocalFile(sourceFile).toString();
        row.append(name);
    }
    QTuiStyledRun stats;
    stats.text = !status.isEmpty()
                     ? QStringLiteral("  %1]").arg(status)
                     : QStringLiteral("  %1 %2x%3 %4]")
                           .arg(shortMime(mimeType))
                           .arg(widthPx)
                           .arg(heightPx)
                           .arg(sizeBucket(bytes.isEmpty() ? byteSize : bytes.size()));
    stats.dim  = true;
    row.append(stats);
    rendered.append(row);

    /* Image rows stay blank under a bitmap and carry half-blocks otherwise.
     * Folded blocks collapse to their metadata line. */
    if (folded) {
        return;
    }
    if (protocol_ == GraphicsProtocol::None || !status.isEmpty() || released) {
        return;
    }
    const auto rect = computeImageCellRect(widthPx, heightPx, width, maxRows);
    cellCols        = rect.first;
    cellRows        = rect.second;
    for (int i = 0; i < cellRows; ++i) {
        rendered.append(QList<QTuiStyledRun>{});
    }
}

int QTuiImagePreviewBlock::rowCount() const
{
    return static_cast<int>(rendered.size());
}

void QTuiImagePreviewBlock::paintRow(
    QTuiScreen &screen,
    int         screenRow,
    int         viewportRow,
    int         xOffset,
    int         width,
    bool        focused,
    bool        selected) const
{
    Q_UNUSED(xOffset);
    Q_UNUSED(focused);
    Q_UNUSED(selected);
    if (viewportRow < 0 || viewportRow >= rendered.size()) {
        return;
    }
    if (viewportRow > 0) {
        if (placeholders_) {
            paintPlaceholders(screen, screenRow, viewportRow - 1, width);
            return;
        }
        const auto window = imageWindow(visibleRowCount < 0 ? rowCount() : visibleRowCount);
        const bool bitmap = protocol_ == GraphicsProtocol::Kitty
                            || (QTuiGraphics::isInlineBitmap(protocol_) && window.count >= cellRows);
        if (!bitmap)
            paintCells(screen, screenRow, viewportRow - 1, width);
        return;
    }
    int col = 0;
    for (const QTuiStyledRun &run : rendered[viewportRow]) {
        for (const char32_t character : run.text.toUcs4()) {
            if (col + (QTuiText::isWideChar(character) ? 2 : 1) > width) {
                return;
            }
            QTuiCell &cell = screen.at(col, screenRow);
            cell.setScalar(character);
            cell.bold       = run.bold;
            cell.italic     = run.italic;
            cell.dim        = run.dim;
            cell.underline  = run.underline;
            cell.inverted   = false;
            cell.fgColor    = run.fg;
            cell.bgColor    = run.bg;
            cell.hyperlink  = run.hyperlink;
            cell.decorative = run.decorative;
            col += QTuiText::isWideChar(character) ? 2 : 1;
        }
    }
}

void QTuiImagePreviewBlock::paintPlaceholders(
    QTuiScreen &screen, int screenRow, int imageRow, int width) const
{
    /* Row marks from the kitty placeholder table. Later cells of a row take
     * their row and next column from the cell on their left. */
    static const char32_t marks[kMarkedRows + 1] = {0x305, 0x30d, 0x30e, 0x310, 0x312, 0x33d, 0x33e,
                                                    0x33f, 0x346, 0x34a, 0x34b, 0x34c, 0x350, 0x351,
                                                    0x352, 0x357, 0x35b, 0x363, 0x364, 0x365, 0x366,
                                                    0x367, 0x368, 0x369, 0x36a, 0x36b, 0x36c, 0x36d,
                                                    0x36e, 0x36f, 0x483};
    if (kittyImageId == 0)
        kittyImageId = QTuiGraphics::ImageStore::instance().allocateId();
    if (imageRow < 0 || imageRow > kMarkedRows)
        return;
    const int columns = qMin(cellCols, width);
    for (int col = 0; col < columns; ++col) {
        QTuiCell &cell = screen.at(col, screenRow);
        cell.setScalar(0x10eeee);
        /* The foreground color names the image. A palette index survives a
         * multiplexer that does not pass 24-bit color. */
        if (kittyImageId < 256)
            cell.fgColor = static_cast<QTuiFgColor>(kittyImageId);
        else
            cell.fgRgb = kRgbSet | kittyImageId;
        cell.decorative = true;
        if (col == 0) {
            const char32_t first[2] = {marks[imageRow], marks[0]};
            cell.combining          = QString::fromUcs4(first, 2);
        }
    }
}

bool QTuiImagePreviewBlock::transmitKitty(QString *out) const
{
    auto &store = QTuiGraphics::ImageStore::instance();
    if (kittyImageId == 0)
        kittyImageId = store.allocateId();
    if (store.isResident(kittyImageId))
        return true;
    const QByteArray pngBytes = reEncodeAsPngIfNeeded(bytes, mimeType);
    if (pngBytes.isEmpty())
        return false;
    for (const quint32 evicted : store.admit(kittyImageId, qint64(widthPx) * heightPx * 4)) {
        *out += QTuiGraphics::passthrough(
            QStringLiteral("\x1b_Ga=d,d=I,i=%1,q=2\x1b\\").arg(evicted));
    }
    *out += kittyTransmitOnly(pngBytes, kittyImageId);
    return true;
}

QRect QTuiImagePreviewBlock::placementFor(
    int firstScreenRow, int firstScreenCol, int contentWidth, const ImageWindow &window) const
{
    return {
        firstScreenCol - 1,
        firstScreenRow - 1 + (window.top + 1 - visibleFirstRow),
        qMin(cellCols, contentWidth),
        window.count};
}

QString QTuiImagePreviewBlock::emitPlaceholders(const ImageWindow &window) const
{
    const auto &store = QTuiGraphics::ImageStore::instance();
    const QSize cells(cellCols, cellRows);
    if (window.count <= 0 || (store.isResident(kittyImageId) && placedCells == cells))
        return {};
    QString out;
    if (!transmitKitty(&out))
        return {};
    out += QTuiGraphics::passthrough(QStringLiteral("\x1b_Ga=p,U=1,i=%1,c=%2,r=%3,q=2\x1b\\")
                                         .arg(kittyImageId)
                                         .arg(cellCols)
                                         .arg(cellRows));
    placedCells = cells;
    return out;
}

QString QTuiImagePreviewBlock::inlinePayload(const QSize &cells) const
{
    if (inlineCells == cells)
        return inlineData;
    const QSize  cell  = QTuiGraphics::cellSize();
    const QImage image = QImage::fromData(bytes).scaled(
        cells.width() * cell.width(),
        cells.height() * cell.height(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
    inlineCells = cells;
    if (protocol_ == GraphicsProtocol::Sixel) {
        inlineData = QString::fromLatin1(QTuiGraphics::encodeSixel(image));
        return inlineData;
    }
    QByteArray png;
    QBuffer    buffer(&png);
    const bool encoded = buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG");
    inlineData         = iTermEscape(
        QFileInfo(sourceLabel).fileName(),
        encoded && png.size() < bytes.size() ? png : bytes,
        QStringLiteral("preserveAspectRatio=1;width=%1;height=%2")
            .arg(cells.width())
            .arg(cells.height()));
    return inlineData;
}

void QTuiImagePreviewBlock::paintCells(
    QTuiScreen &screen, int screenRow, int imageRow, int width) const
{
    if (cellRaster.isNull() && !bytes.isEmpty() && cellRows > 0) {
        cellRaster
            = QImage::fromData(bytes)
                  .scaled(cellCols, cellRows * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                  .convertToFormat(QImage::Format_ARGB32);
    }
    if (cellRaster.isNull() || imageRow < 0 || imageRow * 2 + 1 >= cellRaster.height())
        return;
    const int  columns   = qMin(cellRaster.width(), width);
    const bool trueColor = QTuiGraphics::hasTrueColor();
    for (int col = 0; col < columns; ++col) {
        paintHalfBlock(
            screen.at(col, screenRow),
            cellRaster.pixel(col, imageRow * 2),
            cellRaster.pixel(col, imageRow * 2 + 1),
            trueColor);
    }
}

QTuiImagePreviewBlock::ImageWindow QTuiImagePreviewBlock::imageWindow(int visibleRows) const
{
    const int top    = qMax(visibleFirstRow, 1);
    const int bottom = qMin(visibleFirstRow + visibleRows, rowCount());
    return {top - 1, qMax(0, bottom - top)};
}

QString QTuiImagePreviewBlock::toPlainText() const
{
    return QStringLiteral("[image: %1 %2 %3x%4 %5]")
        .arg(sourceLabel)
        .arg(shortMime(mimeType))
        .arg(widthPx)
        .arg(heightPx)
        .arg(sizeBucket(bytes.isEmpty() ? byteSize : bytes.size()));
}

QString QTuiImagePreviewBlock::toMarkdown() const
{
    /* GFM image syntax. The label doubles as the alt-text and as the
     * link path so a paste-back into another markdown processor finds
     * the file when it lives at the same relative path. */
    return QStringLiteral("![image](%1)\n").arg(sourceLabel);
}

QString QTuiImagePreviewBlock::toAnsi(int width)
{
    if (width <= 0 || bytes.isEmpty()) {
        return QString();
    }
    layout(width);
    /* Folded blocks contribute only the metadata line so the
     * cooked-mode scrollback dump (alt-screen exit path) does not
     * stack a kitty image on top of the placeholder cells, which
     * would double the vertical footprint into a large blank gap
     * after the image renders. */
    QString graphicsEscape;
    if (!folded) {
        switch (protocol_) {
        case GraphicsProtocol::Kitty:
            graphicsEscape = kittyInlineEscape(bytes, mimeType);
            break;
        case GraphicsProtocol::ITerm2:
            graphicsEscape = iTermEscape(
                QFileInfo(sourceLabel).fileName(), bytes, QStringLiteral("width=auto;height=auto"));
            break;
        default:
            break;
        }
    }
    /* Compose: the graphics escape lands at the cursor, then a
     * newline pushes the cell-grid placeholder down so the image and
     * the metadata both stay visible. Default base implementation
     * paints the placeholder; we re-use it. */
    QString out;
    if (!graphicsEscape.isEmpty()) {
        out.append(graphicsEscape);
        out.append(QLatin1Char('\n'));
    }
    out.append(QTuiBlock::toAnsi(width));
    return out;
}

QTuiBlock::GraphicsState QTuiImagePreviewBlock::graphicsState(
    int                 firstScreenRow,
    int                 firstScreenCol,
    int                 contentWidth,
    int                 visibleRows,
    const QVector<int> &writtenRows) const
{
    if (folded || cellRows <= 0 || cellCols <= 0 || contentWidth <= 0 || bytes.isEmpty()) {
        return GraphicsState::Hidden;
    }
    const bool inlined = QTuiGraphics::isInlineBitmap(protocol_);
    if (protocol_ != GraphicsProtocol::Kitty && !inlined) {
        return GraphicsState::Hidden;
    }
    const ImageWindow window = imageWindow(visibleRows);
    if (window.count <= 0 || (inlined && window.count < cellRows)) {
        return GraphicsState::Hidden;
    }
    if (placeholders_) {
        return QTuiGraphics::ImageStore::instance().isResident(kittyImageId)
                       && placedCells == QSize(cellCols, cellRows)
                   ? GraphicsState::Keep
                   : GraphicsState::Place;
    }
    const QRect nextRect = placementFor(firstScreenRow, firstScreenCol, contentWidth, window);
    if (placementRect != nextRect || placementTop != window.top
        || (protocol_ == GraphicsProtocol::Kitty
            && !QTuiGraphics::ImageStore::instance().isResident(kittyImageId))) {
        return GraphicsState::Place;
    }
    if (inlined) {
        for (const int row : writtenRows) {
            if (row >= nextRect.top() && row <= nextRect.bottom()) {
                return GraphicsState::Place;
            }
        }
    }
    return GraphicsState::Keep;
}

QRect QTuiImagePreviewBlock::graphicsEraseRect() const
{
    return QTuiGraphics::isInlineBitmap(protocol_) ? placementRect : QRect();
}

void QTuiImagePreviewBlock::resetGraphicsState() const
{
    QTuiGraphics::ImageStore::instance().forget(kittyImageId);
    placementRect = {};
    placementTop  = 0;
    placedCells   = {};
}

QString QTuiImagePreviewBlock::emitGraphicsLayer(
    int firstScreenRow, int firstScreenCol, int contentWidth, int visibleRows) const
{
    if (graphicsState(firstScreenRow, firstScreenCol, contentWidth, visibleRows)
        == GraphicsState::Hidden) {
        return {};
    }
    const ImageWindow window = imageWindow(visibleRows);
    if (placeholders_)
        return emitPlaceholders(window);
    const QRect nextRect = placementFor(firstScreenRow, firstScreenCol, contentWidth, window);
    QString     out;
    if (protocol_ == GraphicsProtocol::Kitty) {
        auto &store = QTuiGraphics::ImageStore::instance();
        if (!transmitKitty(&out)) {
            return {};
        }
        store.setPlaced(kittyImageId, true);
        out += QStringLiteral("\x1b[%1;%2H").arg(nextRect.y() + 1).arg(firstScreenCol);
        out += kittyPlaceAtCursor(
            kittyImageId,
            /*placementId=*/1,
            kittySourceRect(widthPx, heightPx, cellRows, window.top, window.count),
            nextRect.width(),
            nextRect.height());
    } else if (QTuiGraphics::isInlineBitmap(protocol_)) {
        out += QStringLiteral("\x1b[%1;%2H").arg(nextRect.y() + 1).arg(firstScreenCol);
        out += inlinePayload(QSize(nextRect.width(), nextRect.height()));
    }
    if (!out.isEmpty()) {
        placementRect = nextRect;
        placementTop  = window.top;
    }
    return out;
}

QString QTuiImagePreviewBlock::emitGraphicsClear() const
{
    placementRect = {};
    auto &store   = QTuiGraphics::ImageStore::instance();
    store.setPlaced(kittyImageId, false);
    if (!store.isResident(kittyImageId) || protocol_ != GraphicsProtocol::Kitty || placeholders_) {
        return {};
    }
    /* Lowercase i removes this placement while retaining the image data. */
    return QStringLiteral("\x1b_Ga=d,d=i,i=%1,p=1,q=2\x1b\\").arg(kittyImageId);
}

QString QTuiImagePreviewBlock::emitGraphicsDestroy() const
{
    QString out;
    if (QTuiGraphics::ImageStore::instance().isResident(kittyImageId)
        && protocol_ == GraphicsProtocol::Kitty) {
        out = QTuiGraphics::passthrough(
            QStringLiteral("\x1b_Ga=d,d=I,i=%1,q=2\x1b\\").arg(kittyImageId));
    }
    resetGraphicsState();
    return out;
}
