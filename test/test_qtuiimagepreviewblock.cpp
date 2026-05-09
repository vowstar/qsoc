// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiimagepreviewblock.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QRegularExpression>
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
    qunsetenv("TMUX");
    qunsetenv("STY");
    qunsetenv("QSOC_NO_IMAGE_GRAPHICS");
}

/* Build a real, decodable image buffer in the requested format.
 * Tests must never embed magic-byte stubs because the project rule
 * requires every test fixture (including images) to be generated
 * at runtime from code; the codec path runs through QImage either
 * way. */
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

QTuiImagePreviewBlock makePngBlock()
{
    return {
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG")};
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

    /* Cell-rectangle reservation */
    void textOnlyTerminalRowCountStaysAtOne();
    void graphicsTerminalReservesCellRectangle();
    void cellRectFitsViewportWidth();
    void cellRectClampsTallImageToMaxRows();
    void wideBannerKeepsNativeFlatRowCount();
    void zeroDimensionsLeaveNoReservation();
    void reservedRowsArePaintedBlank();
    void smallImageStaysAtNativeSize();
    void resizePreservesAspectRatio();

    /* Live overlay state machine */
    void emitGraphicsLayerEmptyOnTextOnlyTerminal();
    void firstFrameTransmitsAndPlaces();
    void subsequentFrameOnlyPlaces();
    void uniqueImageIdAcrossBlocks();
    void emitGraphicsLayerReencodesJpeg();
    void emitGraphicsLayerCursorMoveMatchesArguments();

    /* Lifecycle */
    void clearWithoutPlaceIsNoop();
    void clearAfterPlaceEmitsKittyPlacementDelete();
    void destroyAfterPlaceEmitsKittyImageDelete();

    /* Protocol conformance */
    void kittyTransmitContainsSuppressResponses();
    void kittyPlacementContainsSuppressAndNoMove();

    /* Multiplexer / opt-out gating */
    void tmuxSuppressesGraphicsAndReservesNoCells();
    void screenSuppressesGraphicsAndReservesNoCells();
    void optOutEnvSuppressesGraphics();
    void optOutEmptyValueDoesNotSuppress();

    /* Fold semantics */
    void blockReportsFoldable();
    void foldedBlockKeepsOnlyMetadataRow();
    void foldedBlockEmitsNoGraphicsPayload();
    void foldedBlockToAnsiOmitsGraphicsEscape();

    /* Viewport clip */
    void emitGraphicsLayerSkipsWhenViewportTooShort();

    /* iTerm2 path */
    void iTerm2FirstFrameEmitsInlineWithCellSize();
    void iTerm2SameCoordsAreThrottled();
    void iTerm2ScrollReEmitsAtNewCoords();
    void iTerm2ClearResetsThrottle();
};

void Test::layoutProducesSingleRowPlaceholder()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/foo.png"),
        QStringLiteral("image/png"),
        800,
        600,
        makeRealImageBytes("PNG"));
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
        makeRealImageBytes("PNG"));
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
        makeRealImageBytes("PNG"));
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

/* Without a graphics protocol the block stays a single line so the
 * scrollback does not grow a useless empty rectangle on terminals
 * that cannot fill it. */
void Test::textOnlyTerminalRowCountStaysAtOne()
{
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 800, 600, QByteArray());
    block.layout(80);
    QCOMPARE(block.rowCount(), 1);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.imageCellCols(), 0);
}

void Test::graphicsTerminalReservesCellRectangle()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 800, 600, QByteArray());
    block.layout(80);
    QVERIFY(block.imageCellCols() > 0);
    QVERIFY(block.imageCellRows() >= 4);
    QCOMPARE(block.rowCount(), 1 + block.imageCellRows());
}

void Test::cellRectFitsViewportWidth()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 4096, 2048, QByteArray());
    block.layout(60);
    /* The 4096-px wide native image would need 512 cell columns at
     * 8 px per cell; the layout must clamp to the viewport. */
    QVERIFY(block.imageCellCols() <= 60);
}

void Test::cellRectClampsTallImageToMaxRows()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 100, 8000, QByteArray());
    block.layout(80);
    /* A portrait taller than the screen would otherwise eat 500
     * rows; the clamp keeps the chat usable. */
    QVERIFY(block.imageCellRows() <= 30);
}

void Test::wideBannerKeepsNativeFlatRowCount()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 4000, 100, QByteArray());
    block.layout(80);
    /* A 4000x100 banner is genuinely flat. Inflating its row count
     * to a fake floor would distort the placement: kitty `c=W,r=H`
     * stretches the bitmap to fill the rectangle exactly. Verify
     * the cell aspect stays close to native (40:1 in pixels). */
    const double pixelAspect = (block.imageCellCols() * 8.0) / (block.imageCellRows() * 16.0);
    QVERIFY(qAbs(pixelAspect - 40.0) / 40.0 < 0.10);
}

void Test::zeroDimensionsLeaveNoReservation()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 0, 0, QByteArray());
    block.layout(80);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.rowCount(), 1);
}

void Test::smallImageStaysAtNativeSize()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 16, 16, QByteArray());
    block.layout(80);
    /* 16 px / 8 px per cell = 2 cols; 16 px / 16 px per cell = 1 row.
     * Tiny thumbnails must stay tiny instead of stretching to a
     * fake minimum, otherwise kitty would scale the bitmap up and
     * blur it. */
    QCOMPARE(block.imageCellCols(), 2);
    QCOMPARE(block.imageCellRows(), 1);
}

void Test::resizePreservesAspectRatio()
{
    /* 800x400 image at four different viewport widths must keep
     * the same pixel aspect ratio (2.0) within rounding tolerance.
     * Independent min/max clamps used to break this for narrow
     * viewports where the cell rect ended up squarer than native. */
    qputenv("KITTY_WINDOW_ID", "1");
    auto cellPixelAspect = [](int viewportWidth) {
        QTuiImagePreviewBlock
            block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 800, 400, QByteArray());
        block.layout(viewportWidth);
        return (block.imageCellCols() * 8.0) / (block.imageCellRows() * 16.0);
    };
    constexpr double kNative   = 2.0;
    constexpr double kRelDelta = 0.10;
    QVERIFY(qAbs(cellPixelAspect(120) - kNative) / kNative < kRelDelta);
    QVERIFY(qAbs(cellPixelAspect(80) - kNative) / kNative < kRelDelta);
    QVERIFY(qAbs(cellPixelAspect(60) - kNative) / kNative < kRelDelta);
    QVERIFY(qAbs(cellPixelAspect(40) - kNative) / kNative < kRelDelta);
}

void Test::reservedRowsArePaintedBlank()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock
        block(QStringLiteral("/tmp/x.png"), QStringLiteral("image/png"), 320, 240, QByteArray());
    block.layout(60);
    QVERIFY(block.imageCellRows() >= 1);

    QTuiScreen screen(60, 30);
    /* paintRow for any reserved row must leave the cell unchanged
     * from its post-clear default; the graphics layer fills the area
     * later. */
    block.paintRow(screen, 0, 0, 0, 60, false, false); /* metadata row */
    block.paintRow(screen, 1, 1, 0, 60, false, false); /* first reserved row */
    QCOMPARE(screen.at(0, 1).character, QChar(QLatin1Char(' ')));
}

/* Without a graphics protocol the live hook contributes nothing so
 * text-only terminals stay at the placeholder line. */
void Test::emitGraphicsLayerEmptyOnTextOnlyTerminal()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QCOMPARE(block.emitGraphicsLayer(5, 1, 60, 100), QString());
}

void Test::firstFrameTransmitsAndPlaces()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    const QString out = block.emitGraphicsLayer(3, 1, 60, 100);
    QVERIFY(out.contains(QStringLiteral("a=t")));   /* transmit-only */
    QVERIFY(out.contains(QStringLiteral("a=p")));   /* placement */
    QVERIFY(out.contains(QStringLiteral("f=100"))); /* PNG */
}

void Test::subsequentFrameOnlyPlaces()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);
    const QString out2 = block.emitGraphicsLayer(4, 1, 60, 100);
    /* The second frame must NOT re-upload the bitmap; only the
     * lightweight placement escape is allowed through. */
    QVERIFY(!out2.contains(QStringLiteral("a=t")));
    QVERIFY(out2.contains(QStringLiteral("a=p")));
}

void Test::uniqueImageIdAcrossBlocks()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock blockA(
        QStringLiteral("/tmp/a.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    QTuiImagePreviewBlock blockB(
        QStringLiteral("/tmp/b.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    blockA.layout(60);
    blockB.layout(60);
    const QString outA = blockA.emitGraphicsLayer(3, 1, 60, 100);
    const QString outB = blockB.emitGraphicsLayer(3, 1, 60, 100);
    /* Ids appear as i=<n> in the placement escape; capture them. */
    static const QRegularExpression idRegex("i=(\\d+),");
    const auto                      matchA = idRegex.match(outA);
    const auto                      matchB = idRegex.match(outB);
    QVERIFY(matchA.hasMatch());
    QVERIFY(matchB.hasMatch());
    QVERIFY(matchA.captured(1) != matchB.captured(1));
}

void Test::emitGraphicsLayerReencodesJpeg()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.jpg"),
        QStringLiteral("image/jpeg"),
        320,
        240,
        makeRealImageBytes("JPEG"));
    block.layout(60);
    const QString out = block.emitGraphicsLayer(3, 1, 60, 100);
    /* The transmit chunk must declare format 100 (PNG) regardless
     * of the source MIME because kitty's f=100 only accepts PNG. */
    QVERIFY(out.contains(QStringLiteral("f=100")));
}

void Test::emitGraphicsLayerCursorMoveMatchesArguments()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    const QString out = block.emitGraphicsLayer(7, 4, 60, 100);
    /* Image rectangle starts one row below the metadata line, in
     * the same column as the block (1-based). */
    QVERIFY(out.contains(QStringLiteral("\x1b[8;4H")));
}

/* Without a previous transmit there is nothing to clear; the block
 * stays silent so we do not flood ghostty with deletes for images
 * that were never uploaded. */
void Test::clearWithoutPlaceIsNoop()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QCOMPARE(block.emitGraphicsClear(), QString());
}

void Test::clearAfterPlaceEmitsKittyPlacementDelete()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);

    const QString out = block.emitGraphicsClear();
    /* Per the kitty spec, the only delete action is `a=d`; the
     * `d=p` selector means "delete placement matched by both i= and
     * p=", which preserves the image cache for a future scroll-in. */
    QVERIFY(out.contains(QStringLiteral("a=d")));
    QVERIFY(out.contains(QStringLiteral("d=p")));
    QVERIFY(out.contains(QStringLiteral("p=1")));
    QVERIFY(out.contains(QStringLiteral("q=2")));
}

void Test::destroyAfterPlaceEmitsKittyImageDelete()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);

    const QString out = block.emitGraphicsDestroy();
    /* `a=d,d=I,i=N` is the protocol-correct way to delete every
     * placement and free the bitmap. The kitty spec has no `a=D`
     * action; the uppercase only ever appears as the d= selector. */
    QVERIFY(out.contains(QStringLiteral("a=d")));
    QVERIFY(out.contains(QStringLiteral("d=I")));
    QVERIFY(!out.contains(QStringLiteral("a=D")));
    QVERIFY(out.contains(QStringLiteral("q=2")));
}

void Test::kittyTransmitContainsSuppressResponses()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    const QString out = block.emitGraphicsLayer(3, 1, 60, 100);
    /* Without q=2 the terminal answers every command with an OK
     * payload that races the agent stdin reader; this guards
     * against regressing back to the noisy emission. */
    QVERIFY(out.contains(QStringLiteral("q=2")));
}

void Test::kittyPlacementContainsSuppressAndNoMove()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    /* Drain the first-frame transmit so the second emission is a
     * pure place sequence; we want to look at the placement keys
     * specifically. */
    block.emitGraphicsLayer(3, 1, 60, 100);
    const QString out = block.emitGraphicsLayer(4, 1, 60, 100);
    QVERIFY(out.contains(QStringLiteral("a=p")));
    QVERIFY(out.contains(QStringLiteral("C=1")));
    QVERIFY(out.contains(QStringLiteral("q=2")));
}

/* When the viewport cannot fit the block's full footprint (1 row
 * for metadata + cellRows for the image), the graphics escape must
 * be suppressed so the placement does not paint past the scroll
 * area into the input or status bar. */
void Test::emitGraphicsLayerSkipsWhenViewportTooShort()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    const int rowsNeeded = block.rowCount();
    QVERIFY(rowsNeeded > 1);

    /* Plenty of room: graphics emitted. */
    QVERIFY(!block.emitGraphicsLayer(3, 1, 60, rowsNeeded).isEmpty());

    /* Viewport too short by even one row: nothing emitted. */
    QCOMPARE(block.emitGraphicsLayer(3, 1, 60, rowsNeeded - 1), QString());

    /* Only the metadata row visible: nothing emitted. */
    QCOMPARE(block.emitGraphicsLayer(3, 1, 60, 1), QString());
}

/* iTerm2 path: the first frame must emit OSC 1337 with cell-unit
 * width and height plus preserveAspectRatio so the inline image
 * fits exactly inside the placeholder rectangle. */
void Test::iTerm2FirstFrameEmitsInlineWithCellSize()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    const QString out = block.emitGraphicsLayer(3, 1, 60, 100);
    QVERIFY(out.contains(QStringLiteral("\x1b]1337;File=")));
    QVERIFY(out.contains(QStringLiteral("preserveAspectRatio=1")));
    QVERIFY(out.contains(QStringLiteral("width=")));
    QVERIFY(out.contains(QStringLiteral("height=")));
    QVERIFY(out.contains(QStringLiteral("inline=1")));
}

void Test::iTerm2SameCoordsAreThrottled()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);
    const QString second = block.emitGraphicsLayer(3, 1, 60, 100);
    /* Same coords - iTerm2 keeps the bitmap until the cells under
     * it get overwritten, so re-uploading would only burn bandwidth. */
    QCOMPARE(second, QString());
}

void Test::iTerm2ScrollReEmitsAtNewCoords()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);
    const QString moved = block.emitGraphicsLayer(7, 1, 60, 100);
    /* Scroll changed the screen row, so the inline image must be
     * re-uploaded at the new cursor position. */
    QVERIFY(moved.contains(QStringLiteral("\x1b]1337;File=")));
    QVERIFY(moved.contains(QStringLiteral("\x1b[8;1H")));
}

void Test::iTerm2ClearResetsThrottle()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    block.emitGraphicsLayer(3, 1, 60, 100);
    block.emitGraphicsClear();
    const QString reentry = block.emitGraphicsLayer(3, 1, 60, 100);
    /* After clear (block scrolled out and back in), the throttle
     * resets so the inline image is re-uploaded at the next place. */
    QVERIFY(reentry.contains(QStringLiteral("\x1b]1337;File=")));
}

/* Inside tmux the graphics escapes get stripped, so the layout
 * must NOT reserve a tall cell rectangle that the user would see
 * as blank space with no image. */
void Test::tmuxSuppressesGraphicsAndReservesNoCells()
{
    qputenv("TERM_PROGRAM", "ghostty");
    qputenv("TMUX", "/tmp/tmux-1000/default,1234,0");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.rowCount(), 1);
    /* No graphics escape on any path. */
    QVERIFY(!block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
    QCOMPARE(block.emitGraphicsLayer(3, 1, 60, 100), QString());
}

/* GNU screen has the same problem as tmux for the same reason. */
void Test::screenSuppressesGraphicsAndReservesNoCells()
{
    qputenv("KITTY_WINDOW_ID", "1");
    qputenv("STY", "12345.pts-0.host");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.rowCount(), 1);
}

/* Explicit user opt-out forces text-only regardless of any other
 * detection signals. */
void Test::optOutEnvSuppressesGraphics()
{
    qputenv("TERM_PROGRAM", "ghostty");
    qputenv("QSOC_NO_IMAGE_GRAPHICS", "1");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.emitGraphicsLayer(3, 1, 60, 100), QString());
}

/* An empty value of the opt-out env must NOT trigger the gate so a
 * user who simply set the variable then unset it leaves the
 * default behavior intact. */
void Test::optOutEmptyValueDoesNotSuppress()
{
    qputenv("TERM_PROGRAM", "ghostty");
    qputenv("QSOC_NO_IMAGE_GRAPHICS", "");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.layout(60);
    QVERIFY(block.imageCellRows() > 0);
}

/* Image previews must opt into fold so the scroll view's
 * focused-fold key (and the auto-fold path on new images) work. */
void Test::blockReportsFoldable()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    QVERIFY(block.isFoldable());
}

void Test::foldedBlockKeepsOnlyMetadataRow()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.setFolded(true);
    block.layout(60);
    QCOMPARE(block.imageCellRows(), 0);
    QCOMPARE(block.rowCount(), 1);
}

void Test::foldedBlockEmitsNoGraphicsPayload()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.setFolded(true);
    block.layout(60);
    /* Even with a graphics-capable terminal, a folded block must
     * not emit any place sequence; the scroll view drives the
     * clear of any prior placement. */
    QCOMPARE(block.emitGraphicsLayer(3, 1, 60, 100), QString());
}

/* The cooked-mode dump path must honor folded state so the
 * scrollback after alt-screen exit shows just `[image: ...]` for
 * folded blocks; otherwise a kitty placement plus the reserved
 * empty cell rectangle would double the footprint. */
void Test::foldedBlockToAnsiOmitsGraphicsEscape()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        makeRealImageBytes("PNG"));
    block.setFolded(true);
    const QString out = block.toAnsi(60);
    QVERIFY(!out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kITerm2EscapePrefix)));
    QVERIFY(out.contains(QStringLiteral("[image: ")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiimagepreviewblock.moc"
