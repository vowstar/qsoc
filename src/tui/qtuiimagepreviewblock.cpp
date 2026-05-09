// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiimagepreviewblock.h"

#include "tui/qtuiwidget.h"

#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QImage>
#include <QProcessEnvironment>

#include <atomic>
#include <utility>

namespace {

enum class GraphicsProtocol : std::uint8_t {
    None,
    Kitty,
    ITerm2,
    SixelPlaceholder, /* Sixel-capable terminal but encoder not bundled */
};

GraphicsProtocol detectProtocol()
{
    /* Re-evaluated on every call so tests can flip env vars between
     * cases. The cost is one environment snapshot plus a handful of
     * string compares, negligible next to the base64 image transfer. */
    const QProcessEnvironment env  = QProcessEnvironment::systemEnvironment();
    const QString             term = env.value(QStringLiteral("TERM"), QString()).toLower();
    const QString termProgram      = env.value(QStringLiteral("TERM_PROGRAM"), QString()).toLower();

    /* Explicit user opt-out. Any non-empty QSOC_NO_IMAGE_GRAPHICS
     * forces the text-only fallback regardless of terminal
     * capability, so SSH bandwidth-conscious sessions or users who
     * prefer not to see large blank rectangles when graphics get
     * stripped can suppress the cell reservation. */
    if (!env.value(QStringLiteral("QSOC_NO_IMAGE_GRAPHICS"), QString()).isEmpty()) {
        return GraphicsProtocol::None;
    }

    /* Multiplexers (tmux, GNU screen) strip kitty / iTerm2 graphics
     * escapes by default, so a graphics-capable host terminal still
     * never sees the bitmap. Detect via the multiplexer-set env
     * vars and force the text fallback so layout does not reserve
     * a tall rectangle that stays visually blank. tmux has an
     * opt-in passthrough mode that requires a host config change
     * plus DCS wrapping; that is out of scope for this gate. */
    if (env.contains(QStringLiteral("TMUX")) || env.contains(QStringLiteral("STY"))) {
        return GraphicsProtocol::None;
    }

    /* Kitty graphics protocol. WezTerm and Rio also speak iTerm2
     * inline, but Kitty's chunked transfer is the more capable
     * dialect on every terminal that supports both. */
    if (env.contains(QStringLiteral("KITTY_WINDOW_ID"))
        || env.contains(QStringLiteral("GHOSTTY_RESOURCES_DIR"))
        || env.contains(QStringLiteral("WEZTERM_EXECUTABLE"))
        || env.contains(QStringLiteral("KONSOLE_VERSION")) || term == QStringLiteral("xterm-kitty")
        || term == QStringLiteral("xterm-ghostty") || termProgram == QStringLiteral("ghostty")
        || termProgram == QStringLiteral("wezterm") || termProgram == QStringLiteral("rio")) {
        return GraphicsProtocol::Kitty;
    }
    if (termProgram == QStringLiteral("iterm.app") || termProgram == QStringLiteral("vscode")
        || termProgram == QStringLiteral("mintty") || term == QStringLiteral("mintty")) {
        return GraphicsProtocol::ITerm2;
    }
    if (term.contains(QStringLiteral("foot")) || term.contains(QStringLiteral("mlterm"))
        || term.contains(QStringLiteral("contour"))) {
        return GraphicsProtocol::SixelPlaceholder;
    }
    return GraphicsProtocol::None;
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

/* Default terminal cell pixel size used when the host does not expose
 * a query response. 8 x 16 covers most monospace fonts at 14-16pt and
 * keeps the aspect-ratio math stable across detection failures. */
constexpr int kCellWidthPx  = 8;
constexpr int kCellHeightPx = 16;
constexpr int kMinCellRows  = 4;
constexpr int kMaxCellRows  = 30;
constexpr int kMinCellCols  = 8;

QPair<int, int> computeImageCellRect(int imageWidthPx, int imageHeightPx, int viewportWidth)
{
    /* Compute the cell rectangle a graphics-capable terminal will use
     * to display the image. The formula keeps the pixel aspect ratio
     * by reading every cell as 8 x 16 px, then clamps the row count
     * so a wide banner stays readable and a tall portrait does not
     * eat the entire scrollback. Returns (0, 0) when the image dims
     * are not yet known so the block falls back to placeholder-only. */
    if (imageWidthPx <= 0 || imageHeightPx <= 0 || viewportWidth <= 4) {
        return {0, 0};
    }
    const int nativeCols = (imageWidthPx + kCellWidthPx - 1) / kCellWidthPx;
    const int maxCols    = qMax(0, viewportWidth - 2);
    int       cols       = qMin(nativeCols, maxCols);
    cols                 = qMax(cols, kMinCellCols);
    if (cols > maxCols) {
        cols = maxCols;
    }
    /* rows = cols * imgH / imgW * (cellW / cellH); the cellW/cellH
     * factor turns a pixel ratio into a cell ratio. */
    int rows = static_cast<int>(
        (static_cast<qint64>(cols) * imageHeightPx * kCellWidthPx)
        / (static_cast<qint64>(imageWidthPx) * kCellHeightPx));
    rows = qBound(kMinCellRows, rows, kMaxCellRows);
    return {cols, rows};
}

QString iTermInlineEscape(const QString &filename, const QByteArray &bytes)
{
    /* iTerm2 inline image: ESC ] 1337 ; File=...:base64 BEL.
     * inline=1 places the image in flow rather than as an attachment;
     * width/height=auto keeps the image at native size with the host
     * downscaling for cell rows. */
    QString out = QStringLiteral("\x1b]1337;File=");
    if (!filename.isEmpty()) {
        const QByteArray nameB64 = filename.toUtf8().toBase64();
        out += QStringLiteral("name=") + QString::fromLatin1(nameB64) + QLatin1Char(';');
    }
    out += QStringLiteral("size=%1;").arg(bytes.size());
    out += QStringLiteral("inline=1;width=auto;height=auto:");
    out += QString::fromLatin1(bytes.toBase64());
    out += QChar(0x07);
    return out;
}

QString iTermPlaceWithSize(
    const QString &filename, const QByteArray &bytes, int cellCols, int cellRows)
{
    /* OSC 1337 inline image with cell-unit sizing and aspect-ratio
     * preservation. `width=N;height=M` (no unit) is iTerm2's
     * character-cell measure, so a placement at N cols x M rows lines
     * up exactly with the cell rectangle the block reserved. */
    if (bytes.isEmpty() || cellCols <= 0 || cellRows <= 0) {
        return {};
    }
    QString out = QStringLiteral("\x1b]1337;File=");
    if (!filename.isEmpty()) {
        const QByteArray nameB64 = filename.toUtf8().toBase64();
        out += QStringLiteral("name=") + QString::fromLatin1(nameB64) + QLatin1Char(';');
    }
    out += QStringLiteral("size=%1;").arg(bytes.size());
    out += QStringLiteral("inline=1;preserveAspectRatio=1;width=%1;height=%2:")
               .arg(cellCols)
               .arg(cellRows);
    out += QString::fromLatin1(bytes.toBase64());
    out += QChar(0x07);
    return out;
}

/* Process-wide allocator for kitty image ids. The protocol allows
 * any non-zero 32-bit id; starting at 1 and incrementing keeps
 * collisions impossible inside one qsoc session. Multiple sessions
 * sharing one terminal are fine because each uses its own id space
 * and explicitly destroys its images on stop(). */
quint32 allocateKittyImageId()
{
    static std::atomic<quint32> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
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

QString kittyTransmitOnly(const QByteArray &pngBytes, quint32 imageId)
{
    /* Chunked `a=t` (transmit-only): upload the bytes into the
     * terminal's image cache without displaying. `q=2` suppresses
     * the per-command OK/EINVAL response so the response payload
     * does not race the agent stdin reader, matching the pattern
     * timg and notcurses both use for TUI integrations. */
    if (pngBytes.isEmpty() || imageId == 0) {
        return {};
    }
    const QByteArray b64       = pngBytes.toBase64();
    constexpr int    chunkSize = 4096;
    QString          out;
    int              offset = 0;
    bool             first  = true;
    while (offset < b64.size()) {
        const int  take  = qMin(chunkSize, static_cast<int>(b64.size() - offset));
        const auto chunk = b64.mid(offset, take);
        const bool last  = offset + take >= b64.size();
        QString    header;
        if (first) {
            header = QStringLiteral("\x1b_Gi=%1,a=t,f=100,q=2,m=%2;").arg(imageId).arg(last ? 0 : 1);
        } else {
            header = QStringLiteral("\x1b_Gq=2,m=%1;").arg(last ? 0 : 1);
        }
        out += header;
        out += QString::fromLatin1(chunk);
        out += QStringLiteral("\x1b\\");
        offset += take;
        first = false;
    }
    return out;
}

QString kittyPlaceAtCursor(quint32 imageId, quint32 placementId, int cellCols, int cellRows)
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
    return QStringLiteral("\x1b_Ga=p,i=%1,p=%2,c=%3,r=%4,C=1,q=2\x1b\\")
        .arg(imageId)
        .arg(placementId)
        .arg(cellCols)
        .arg(cellRows);
}

QString kittyInlineEscape(const QByteArray &bytes, const QString &mime)
{
    /* Kitty graphics: ESC _ G f=<format>,a=T;<base64> ESC \\
     * Format 100 is PNG. JPEG/GIF/WebP attachments are decoded through
     * QImage and re-encoded as PNG so terminals like ghostty / kitty /
     * wezterm see the image instead of just the placeholder text. The
     * terminal picks reasonable display dimensions when c/r aren't
     * given. */
    const QByteArray pngBytes = reEncodeAsPngIfNeeded(bytes, mime);
    if (pngBytes.isEmpty()) {
        return QString();
    }
    const QByteArray b64 = pngBytes.toBase64();
    /* Chunked transfer keeps individual escape payloads short enough
     * for terminals that bound their parsing buffers. m=1 marks a
     * non-final chunk, m=0 the last. */
    constexpr int chunkSize = 4096;
    QString       out;
    int           offset = 0;
    bool          first  = true;
    while (offset < b64.size()) {
        const int     take   = qMin(chunkSize, b64.size() - offset);
        const auto    chunk  = b64.mid(offset, take);
        const bool    last   = offset + take >= b64.size();
        const QString header = first ? QStringLiteral("\x1b_Gf=100,a=T,q=2,m=%1;").arg(last ? 0 : 1)
                                     : QStringLiteral("\x1b_Gq=2,m=%1;").arg(last ? 0 : 1);
        out += header;
        out += QString::fromLatin1(chunk);
        out += QStringLiteral("\x1b\\");
        offset += take;
        first = false;
    }
    return out;
}

} // namespace

QTuiImagePreviewBlock::QTuiImagePreviewBlock(
    QString sourceLabel, QString mimeType, int widthPx, int heightPx, QByteArray bytes)
    : sourceLabel(std::move(sourceLabel))
    , mimeType(std::move(mimeType))
    , widthPx(widthPx)
    , heightPx(heightPx)
    , bytes(std::move(bytes))
{}

void QTuiImagePreviewBlock::layout(int width)
{
    if (!layoutDirty && layoutWidth == width) {
        return;
    }
    layoutDirty = false;
    layoutWidth = width;
    rendered.clear();
    cellRows = 0;
    cellCols = 0;

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
        row.append(name);
    }
    QTuiStyledRun stats;
    stats.text = QStringLiteral("  %1 %2x%3 %4]")
                     .arg(shortMime(mimeType))
                     .arg(widthPx)
                     .arg(heightPx)
                     .arg(sizeBucket(bytes.size()));
    stats.dim  = true;
    row.append(stats);
    rendered.append(row);

    /* Reserve empty cells for a future graphics overlay only on
     * terminals that have a real graphics protocol. Sixel and the
     * text fallback would just leave a blank gap, which is worse than
     * the bare placeholder. */
    const GraphicsProtocol protocol = detectProtocol();
    if (protocol == GraphicsProtocol::Kitty || protocol == GraphicsProtocol::ITerm2) {
        const auto rect = computeImageCellRect(widthPx, heightPx, width);
        cellCols        = rect.first;
        cellRows        = rect.second;
        for (int i = 0; i < cellRows; ++i) {
            rendered.append(QList<QTuiStyledRun>{});
        }
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
    int col = 0;
    for (const QTuiStyledRun &run : rendered[viewportRow]) {
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

QString QTuiImagePreviewBlock::toPlainText() const
{
    return QStringLiteral("[image: %1 %2 %3x%4 %5]")
        .arg(sourceLabel)
        .arg(shortMime(mimeType))
        .arg(widthPx)
        .arg(heightPx)
        .arg(sizeBucket(bytes.size()));
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
    QString graphicsEscape;
    switch (detectProtocol()) {
    case GraphicsProtocol::Kitty:
        graphicsEscape = kittyInlineEscape(bytes, mimeType);
        break;
    case GraphicsProtocol::ITerm2:
        graphicsEscape = iTermInlineEscape(QFileInfo(sourceLabel).fileName(), bytes);
        break;
    case GraphicsProtocol::SixelPlaceholder:
    case GraphicsProtocol::None:
    default:
        break;
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

QString QTuiImagePreviewBlock::emitGraphicsLayer(
    int firstScreenRow, int firstScreenCol, int contentWidth) const
{
    /* Live overlay: paint the image into the cell rectangle
     * reserved by layout(). The cell-grid pass has already drawn
     * the metadata line and left the rest of the rectangle blank,
     * so the placement lands right where the user expects. */
    if (cellRows <= 0 || cellCols <= 0 || bytes.isEmpty()) {
        return QString();
    }

    /* Place the rectangle one row below the metadata line so the
     * label stays readable above the image. The width is clamped to
     * the current viewport in case the user shrank the terminal
     * after layout but before this frame. */
    const int placeRow  = firstScreenRow + 1;
    const int placeCols = qMin(cellCols, qMax(1, contentWidth));

    const GraphicsProtocol protocol = detectProtocol();
    if (protocol == GraphicsProtocol::Kitty) {
        QString out;
        if (!kittyState.transmitted) {
            const QByteArray pngBytes = reEncodeAsPngIfNeeded(bytes, mimeType);
            if (pngBytes.isEmpty()) {
                return QString();
            }
            if (kittyState.imageId == 0) {
                kittyState.imageId = allocateKittyImageId();
            }
            out += kittyTransmitOnly(pngBytes, kittyState.imageId);
            kittyState.transmitted = true;
        }
        out += QStringLiteral("\x1b[%1;%2H").arg(placeRow).arg(firstScreenCol);
        out += kittyPlaceAtCursor(kittyState.imageId, /*placementId=*/1, placeCols, cellRows);
        return out;
    }

    if (protocol == GraphicsProtocol::ITerm2) {
        /* iTerm2 has no place-by-id, so every emission re-uploads
         * the bitmap. Throttle: skip when neither the cell grid
         * nor the block coordinates moved since the last frame. */
        if (iTerm2LastRow == placeRow && iTerm2LastCol == firstScreenCol) {
            return QString();
        }
        iTerm2LastRow = placeRow;
        iTerm2LastCol = firstScreenCol;
        QString out;
        out += QStringLiteral("\x1b[%1;%2H").arg(placeRow).arg(firstScreenCol);
        out += iTermPlaceWithSize(QFileInfo(sourceLabel).fileName(), bytes, placeCols, cellRows);
        return out;
    }

    return QString();
}

QString QTuiImagePreviewBlock::emitGraphicsClear() const
{
    /* iTerm2 has no protocol delete: the cells get overwritten by
     * the next frame's text, which is enough to erase the bitmap.
     * Reset the throttle so a future re-entry into the viewport
     * unconditionally re-emits the inline image. */
    iTerm2LastRow = -1;
    iTerm2LastCol = -1;

    /* `a=d` is the only delete action; the `d=` parameter selects
     * what to delete. `d=p,i=<id>,p=<pid>` removes exactly one
     * placement while keeping the bitmap cached, so a scroll back
     * into the viewport only needs the lightweight placement
     * escape again, not a full re-upload. */
    if (!kittyState.transmitted || kittyState.imageId == 0) {
        return QString();
    }
    if (detectProtocol() != GraphicsProtocol::Kitty) {
        return QString();
    }
    return QStringLiteral("\x1b_Ga=d,d=p,i=%1,p=1,q=2\x1b\\").arg(kittyState.imageId);
}

QString QTuiImagePreviewBlock::emitGraphicsDestroy() const
{
    /* `a=d,d=I,i=<id>` deletes every placement of the image AND
     * frees the bitmap from the terminal's cache. The kitty
     * protocol has no `a=D` action; uppercase only ever appears as
     * a `d=` selector to distinguish "free image data" from "keep
     * image data". Called once per shutdown so the terminal
     * reclaims any memory the session asked it to hold. */
    if (!kittyState.transmitted || kittyState.imageId == 0) {
        return QString();
    }
    if (detectProtocol() != GraphicsProtocol::Kitty) {
        return QString();
    }
    return QStringLiteral("\x1b_Ga=d,d=I,i=%1,q=2\x1b\\").arg(kittyState.imageId);
}
