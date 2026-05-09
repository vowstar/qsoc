// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiimagepreviewblock.h"

#include "tui/qtuiwidget.h"

#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QImage>
#include <QProcessEnvironment>

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

QString kittyInlineEscape(const QByteArray &bytes, const QString &mime)
{
    /* Kitty graphics: ESC _ G f=<format>,a=T;<base64> ESC \\
     * Format 100 is PNG. JPEG/GIF/WebP attachments are decoded through
     * QImage and re-encoded as PNG so terminals like ghostty / kitty /
     * wezterm see the image instead of just the placeholder text. The
     * terminal picks reasonable display dimensions when c/r aren't
     * given. */
    QByteArray pngBytes;
    if (mime.contains(QStringLiteral("png"))) {
        pngBytes = bytes;
    } else {
        QImage image;
        if (!image.loadFromData(bytes)) {
            return QString();
        }
        QBuffer buffer(&pngBytes);
        if (!buffer.open(QIODevice::WriteOnly)) {
            return QString();
        }
        if (!image.save(&buffer, "PNG")) {
            return QString();
        }
        buffer.close();
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
        const QString header = first ? QStringLiteral("\x1b_Gf=100,a=T,m=%1;").arg(last ? 0 : 1)
                                     : QStringLiteral("\x1b_Gm=%1;").arg(last ? 0 : 1);
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
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rendered.clear();

    /* Single-row placeholder: [image: <name>  png 800x600 32 KB].
     * This is what cell-grid mode shows; cooked-mode toAnsi prepends
     * the actual graphics escape sequence on top. */
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
