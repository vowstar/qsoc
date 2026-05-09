// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiimagepreviewblock.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QtTest>

namespace {

void clearGraphicsEnv()
{
    /* Wipe every signal detectProtocol() looks at so each test starts
     * from a clean slate regardless of what host env shell launched
     * the suite. */
    qunsetenv("KITTY_WINDOW_ID");
    qunsetenv("GHOSTTY_RESOURCES_DIR");
    qunsetenv("WEZTERM_EXECUTABLE");
    qunsetenv("KONSOLE_VERSION");
    qunsetenv("TERM");
    qunsetenv("TERM_PROGRAM");
}

QTuiImagePreviewBlock makePngBlock()
{
    return {
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        QByteArray("\x89PNG\r\n", 6)};
}

/* Build a real, decodable image buffer in the requested format. The
 * kitty-graphics path now re-encodes through QImage, so the magic-byte
 * stub used elsewhere in this file is not enough; we need bytes that
 * QImage::loadFromData accepts. */
QByteArray makeRealImageBytes(const char *format)
{
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(QColor(255, 64, 32));
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    buffer.close();
    return bytes;
}

constexpr auto kKittyEscapePrefix  = "\x1b_G";
constexpr auto kITerm2EscapePrefix = "\x1b]1337;File=";

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void init() { clearGraphicsEnv(); }
    void cleanup() { clearGraphicsEnv(); }

    void layoutProducesSingleRowPlaceholder();
    void plainTextSummarisesMetadata();
    void markdownEmitsImageSyntax();
    void toAnsiContainsPlaceholderText();

    /* Protocol routing */
    void emptyEnvProducesNoEscape();
    void kittyWindowIdSelectsKitty();
    void ghosttyResourcesDirSelectsKitty();
    void ghosttyTermProgramSelectsKitty();
    void wezTermExecutableSelectsKitty();
    void wezTermProgramSelectsKitty();
    void rioTermProgramSelectsKitty();
    void konsoleVersionSelectsKitty();
    void xtermKittySelectsKitty();
    void xtermGhosttySelectsKitty();
    void itermSelectsITerm2();
    void vscodeSelectsITerm2();
    void minttySelectsITerm2();
    void termProgramCaseInsensitive();

    /* Re-encode path */
    void kittyJpegIsReEncodedToPng();
    void kittyGifIsReEncodedToPng();
    void kittyPngPassesThrough();
};

void Test::layoutProducesSingleRowPlaceholder()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/foo.png"),
        QStringLiteral("image/png"),
        800,
        600,
        QByteArray("\x89PNG\r\n", 6));
    block.layout(80);
    QCOMPARE(block.rowCount(), 1);
}

void Test::plainTextSummarisesMetadata()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/foo.png"),
        QStringLiteral("image/png"),
        800,
        600,
        QByteArray("\x89PNG\r\n", 6));
    const QString text = block.toPlainText();
    QVERIFY(text.contains(QStringLiteral("/tmp/foo.png")));
    QVERIFY(text.contains(QStringLiteral("png")));
    QVERIFY(text.contains(QStringLiteral("800x600")));
}

void Test::markdownEmitsImageSyntax()
{
    QTuiImagePreviewBlock
        block(QStringLiteral("hello.jpg"), QStringLiteral("image/jpeg"), 100, 50, QByteArray());
    QCOMPARE(block.toMarkdown(), QStringLiteral("![image](hello.jpg)\n"));
}

void Test::toAnsiContainsPlaceholderText()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        QByteArray("\x89PNG\r\n", 6));
    const QString out = block.toAnsi(60);
    QVERIFY(out.contains(QStringLiteral("[image: ")));
    QVERIFY(out.contains(QStringLiteral("x.png")));
}

void Test::emptyEnvProducesNoEscape()
{
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(!out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kITerm2EscapePrefix)));
    QVERIFY(out.contains(QStringLiteral("[image: ")));
}

void Test::kittyWindowIdSelectsKitty()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::ghosttyResourcesDirSelectsKitty()
{
    qputenv("GHOSTTY_RESOURCES_DIR", "/usr/share/ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::ghosttyTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::wezTermExecutableSelectsKitty()
{
    qputenv("WEZTERM_EXECUTABLE", "/usr/bin/wezterm");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::wezTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "WezTerm");
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::rioTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "rio");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::konsoleVersionSelectsKitty()
{
    qputenv("KONSOLE_VERSION", "230400");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::xtermKittySelectsKitty()
{
    qputenv("TERM", "xterm-kitty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::xtermGhosttySelectsKitty()
{
    qputenv("TERM", "xterm-ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::itermSelectsITerm2()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kITerm2EscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::vscodeSelectsITerm2()
{
    qputenv("TERM_PROGRAM", "vscode");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::minttySelectsITerm2()
{
    qputenv("TERM", "mintty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::termProgramCaseInsensitive()
{
    /* Real terminals advertise mixed case (iTerm.app, WezTerm). The
     * detector lowercases TERM_PROGRAM so capitalisation can't gate
     * detection. */
    qputenv("TERM_PROGRAM", "GHOSTTY");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

/* The canonical ghostty failure mode: the user attaches a JPEG and the
 * old code returned an empty graphics escape because it only accepted
 * PNG bytes, leaving ghostty / kitty / wezterm with just the placeholder
 * text. After the re-encode, every supported raster format produces a
 * non-empty kitty graphics escape that carries valid PNG payload. */
void Test::kittyJpegIsReEncodedToPng()
{
    qputenv("TERM_PROGRAM", "ghostty");
    const QByteArray jpegBytes = makeRealImageBytes("JPEG");
    QVERIFY2(!jpegBytes.isEmpty(), "QImage failed to encode JPEG fixture");

    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.jpg"), QStringLiteral("image/jpeg"), 16, 16, jpegBytes);
    const QString out = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(out.contains(QStringLiteral("f=100")));
}

void Test::kittyGifIsReEncodedToPng()
{
    qputenv("KITTY_WINDOW_ID", "1");
    const QByteArray gifBytes = makeRealImageBytes("GIF");
    if (gifBytes.isEmpty()) {
        QSKIP("Qt build lacks GIF write support");
    }

    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.gif"), QStringLiteral("image/gif"), 16, 16, gifBytes);
    const QString out = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::kittyPngPassesThrough()
{
    qputenv("TERM_PROGRAM", "ghostty");
    const QByteArray pngBytes = makeRealImageBytes("PNG");
    QVERIFY2(!pngBytes.isEmpty(), "QImage failed to encode PNG fixture");

    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 16, 16, pngBytes);
    const QString out = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(out.contains(QStringLiteral("f=100")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiimagepreviewblock.moc"
