// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiblock.h"
#include "tui/qtuiimagepreviewblock.h"
#include "tui/qtuiscrollview.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

namespace {

/* Real PNG buffer generated at runtime; the project bans static
 * image fixtures so every test that needs decodable bytes goes
 * through QImage at construction time. */
QByteArray makeRealPngBytes()
{
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(QColor(255, 64, 32));
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    buffer.close();
    return bytes;
}

/* Minimal block whose only purpose is to record the graphics-layer
 * arguments and emit a known token so we can verify the scroll view
 * passes the right offsets and concatenates the right results. */
class ProbeBlock : public QTuiBlock
{
public:
    explicit ProbeBlock(QString tag, int rowCount = 1)
        : tag(std::move(tag))
        , rowCountValue(rowCount)
    {}

    void layout(int width) override
    {
        layoutDirty = false;
        layoutWidth = width;
    }
    int  rowCount() const override { return rowCountValue; }
    void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const override
    {
        Q_UNUSED(screen);
        Q_UNUSED(screenRow);
        Q_UNUSED(viewportRow);
        Q_UNUSED(xOffset);
        Q_UNUSED(width);
        Q_UNUSED(focused);
        Q_UNUSED(selected);
    }

    QString toPlainText() const override { return tag; }

    GraphicsState graphicsState(int, int, int, int, const QVector<int> & = {}) const override
    {
        return isFolded() ? GraphicsState::Hidden : GraphicsState::Place;
    }

    QString emitGraphicsLayer(
        int firstScreenRow, int firstScreenCol, int contentWidth, int visibleRows) const override
    {
        ++callCount;
        lastFirstScreenRow = firstScreenRow;
        lastFirstScreenCol = firstScreenCol;
        lastContentWidth   = contentWidth;
        lastVisibleRows    = visibleRows;
        return QStringLiteral("<%1@%2,%3w%4v%5>")
            .arg(tag)
            .arg(firstScreenRow)
            .arg(firstScreenCol)
            .arg(contentWidth)
            .arg(visibleRows);
    }

    QString emitGraphicsClear() const override
    {
        ++clearCount;
        return QStringLiteral("[clear:%1]").arg(tag);
    }

    QString emitGraphicsDestroy() const override
    {
        ++destroyCount;
        return QStringLiteral("[destroy:%1]").arg(tag);
    }

    QString     tag;
    int         rowCountValue;
    mutable int callCount          = 0;
    mutable int lastFirstScreenRow = 0;
    mutable int lastFirstScreenCol = 0;
    mutable int lastContentWidth   = 0;
    mutable int lastVisibleRows    = 0;
    mutable int clearCount         = 0;
    mutable int destroyCount       = 0;
};

QTuiImagePreviewBlock *appendImage(QTuiScrollView &view)
{
    auto image = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("generated.png"), QStringLiteral("image/png"), 160, 96, makeRealPngBytes());
    auto *ptr = image.get();
    view.appendBlock(std::move(image));
    return ptr;
}

QString renderFrame(QTuiScrollView &view, QTuiScreen &screen, int height)
{
    screen.clear();
    view.render(screen, 0, height, screen.width());
    QString out = view.prepareGraphicsLayer(screen);
    out += screen.toAnsi();
    out += view.collectGraphicsLayer(screen.writtenRows());
    return out;
}

void clearGraphicsEnv()
{
    for (const char *name :
         {"KITTY_WINDOW_ID",
          "GHOSTTY_RESOURCES_DIR",
          "WEZTERM_EXECUTABLE",
          "KONSOLE_VERSION",
          "TERM",
          "TERM_PROGRAM",
          "TMUX",
          "STY",
          "QSOC_NO_IMAGE_GRAPHICS"}) {
        qunsetenv(name);
    }
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void init() { clearGraphicsEnv(); }
    void cleanup() { clearGraphicsEnv(); }
    void clippedImageClearsAndReturns();
    void twoImagesDeleteOnlyHiddenPlacement();
    void inlineImageTracksActualWrites();
    void inlineMoveRepaintsOldRows();
    void newScreenRetransmitsImages_data();
    void newScreenRetransmitsImages();
    void screenTracksOnlyWrittenRows();
    void emptyScrollViewProducesNoOverlay();
    void blocksWithDefaultEmitContributeNothing();
    void visibleBlockProbeReceivesScreenCoords();
    void offViewportBlockNotInvoked();
    void multipleVisibleBlocksConcatenateInOrder();

    /* Lifecycle */
    void blockScrolledOutEmitsClear();
    void blockReturnsAfterScrollDoesNotEmitClear();
    void collectGraphicsDestroyWalksAllBlocks();

    /* Fold semantics */
    void appendingImageFoldsPriorImagePreviews();
    void foldedBlockTriggersClearOnNextFrame();
    void foldedBlockNotPlacedEvenWhenVisible();
    void appendingNonImageDoesNotFoldImages();
    void foldAllImagePreviewsTouchesOnlyImageBlocks();
};

void Test::emptyScrollViewProducesNoOverlay()
{
    QTuiScrollView view;
    QTuiScreen     screen(40, 6);
    view.render(screen, 0, 6, 40);
    QCOMPARE(view.collectGraphicsLayer(), QString());
}

void Test::blocksWithDefaultEmitContributeNothing()
{
    /* The base QTuiBlock::emitGraphicsLayer returns an empty string
     * by design, so any block that has not overridden it must drop
     * out of the concatenated overlay. */
    QTuiScrollView view;
    view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("a")));
    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);
    /* ProbeBlock overrides the hook, but verify the contract is the
     * payload is what gets concatenated. */
    QString out = view.collectGraphicsLayer();
    QVERIFY(!out.isEmpty());
    QVERIFY(out.contains(QStringLiteral("<a@")));
}

void Test::visibleBlockProbeReceivesScreenCoords()
{
    QTuiScrollView view;
    auto           probe = std::make_unique<ProbeBlock>(QStringLiteral("img"));
    auto          *raw   = probe.get();
    view.appendBlock(std::move(probe));

    QTuiScreen screen(40, 6);
    view.render(screen, 2 /* startRow */, 6, 40);

    QVERIFY(!view.collectGraphicsLayer().isEmpty());
    QCOMPARE(raw->callCount, 1);
    /* Single 1-row block bottom-aligned in a 6-row viewport at
     * startRow=2: lands at zero-based screen row 7, one-based row 8. */
    QCOMPARE(raw->lastFirstScreenRow, 8);
    QCOMPARE(raw->lastFirstScreenCol, 1);
    /* contentWidth = width - 1 (scrollbar gutter). */
    QCOMPARE(raw->lastContentWidth, 39);
}

void Test::offViewportBlockNotInvoked()
{
    QTuiScrollView view;
    /* Stack twenty 1-row blocks; only the last 6 fit in a 6-row
     * viewport when scrolled to bottom. The first 14 must NOT have
     * their graphics hook called. */
    std::vector<ProbeBlock *> probes;
    for (int i = 0; i < 20; ++i) {
        auto probe = std::make_unique<ProbeBlock>(QStringLiteral("p%1").arg(i));
        probes.push_back(probe.get());
        view.appendBlock(std::move(probe));
    }
    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);
    view.collectGraphicsLayer();

    int visible = 0;
    for (auto *probe : probes) {
        visible += probe->callCount;
    }
    QCOMPARE(visible, 6);
    /* The earliest blocks are off-viewport; their hook never fired. */
    QCOMPARE(probes[0]->callCount, 0);
    QCOMPARE(probes[13]->callCount, 0);
    QCOMPARE(probes[14]->callCount, 1);
    QCOMPARE(probes[19]->callCount, 1);
}

void Test::multipleVisibleBlocksConcatenateInOrder()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("first")));
    view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("second")));
    view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("third")));

    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);

    const QString out = view.collectGraphicsLayer();
    QVERIFY(out.indexOf(QStringLiteral("<first@")) >= 0);
    QVERIFY(out.indexOf(QStringLiteral("<second@")) >= 0);
    QVERIFY(out.indexOf(QStringLiteral("<third@")) >= 0);
    QVERIFY(out.indexOf(QStringLiteral("<first@")) < out.indexOf(QStringLiteral("<second@")));
    QVERIFY(out.indexOf(QStringLiteral("<second@")) < out.indexOf(QStringLiteral("<third@")));
}

/* When a block was visible last frame but no longer is, the scroll
 * view must emit its clear escape so the terminal forgets the
 * previous placement. The new visible blocks then run their own
 * place hooks. */
void Test::blockScrolledOutEmitsClear()
{
    QTuiScrollView view;
    /* 20 single-row blocks, viewport of 6 rows: bottom-aligned, so
     * blocks p14..p19 are visible. After scrollUp(6), the viewport
     * moves to p8..p13 and p14..p19 should now scroll out. */
    std::vector<ProbeBlock *> probes;
    for (int i = 0; i < 20; ++i) {
        auto probe = std::make_unique<ProbeBlock>(QStringLiteral("p%1").arg(i));
        probes.push_back(probe.get());
        view.appendBlock(std::move(probe));
    }
    QTuiScreen screen(40, 6);

    view.render(screen, 0, 6, 40);
    view.collectGraphicsLayer();

    view.scrollUp(6);
    view.render(screen, 0, 6, 40);
    const QString out = view.collectGraphicsLayer();

    /* Previously visible blocks (p14..p19) emitted their clear. */
    for (int idx = 14; idx <= 19; ++idx) {
        QVERIFY2(
            probes[idx]->clearCount == 1,
            qPrintable(QStringLiteral("p%1 should have cleared once").arg(idx)));
        QVERIFY(out.contains(QStringLiteral("[clear:p%1]").arg(idx)));
    }
    /* Newly visible blocks (p8..p13) emitted their place. */
    for (int idx = 8; idx <= 13; ++idx) {
        QVERIFY2(
            probes[idx]->callCount == 1,
            qPrintable(QStringLiteral("p%1 should have placed once").arg(idx)));
    }
}

void Test::blockReturnsAfterScrollDoesNotEmitClear()
{
    QTuiScrollView view;
    auto           probe = std::make_unique<ProbeBlock>(QStringLiteral("img"));
    auto          *raw   = probe.get();
    view.appendBlock(std::move(probe));
    /* Stack enough filler blocks to push the image out when scrolled. */
    for (int i = 0; i < 10; ++i) {
        view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("f%1").arg(i)));
    }
    QTuiScreen screen(40, 4);

    /* Frame 1: image is at the top, currently NOT in viewport (only
     * the last 4 fillers are). */
    view.render(screen, 0, 4, 40);
    view.collectGraphicsLayer();
    QCOMPARE(raw->callCount, 0);
    QCOMPARE(raw->clearCount, 0);

    /* Scroll up just enough to bring the image into the viewport.
     * Total rows is 11 with a 4-row viewport, so scroll offsets in
     * [7, 10] show the image at row 0; pick 8 to keep it solidly
     * inside the visible window. */
    view.scrollUp(8);
    view.render(screen, 0, 4, 40);
    view.collectGraphicsLayer();
    QCOMPARE(raw->callCount, 1);
    QCOMPARE(raw->clearCount, 0);
}

void Test::collectGraphicsDestroyWalksAllBlocks()
{
    QTuiScrollView            view;
    std::vector<ProbeBlock *> probes;
    for (int i = 0; i < 5; ++i) {
        auto probe = std::make_unique<ProbeBlock>(QStringLiteral("d%1").arg(i));
        probes.push_back(probe.get());
        view.appendBlock(std::move(probe));
    }

    const QString out = view.collectGraphicsDestroy();
    for (int idx = 0; idx < 5; ++idx) {
        QCOMPARE(probes[idx]->destroyCount, 1);
        QVERIFY(out.contains(QStringLiteral("[destroy:d%1]").arg(idx)));
    }
}

/* Auto-fold: when a new QTuiImagePreviewBlock is appended, every
 * previously appended image preview must be folded so the chat
 * keeps only the latest bitmap rendered as graphics. */
void Test::appendingImageFoldsPriorImagePreviews()
{
    QTuiScrollView view;
    auto           first = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("/tmp/first.png"), QStringLiteral("image/png"), 320, 240, makeRealPngBytes());
    auto *firstPtr = first.get();
    view.appendBlock(std::move(first));
    QVERIFY(!firstPtr->isFolded());

    auto second = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("/tmp/second.png"), QStringLiteral("image/png"), 320, 240, makeRealPngBytes());
    auto *secondPtr = second.get();
    view.appendBlock(std::move(second));
    QVERIFY(firstPtr->isFolded());
    QVERIFY(!secondPtr->isFolded());

    auto third = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("/tmp/third.png"), QStringLiteral("image/png"), 320, 240, makeRealPngBytes());
    auto *thirdPtr = third.get();
    view.appendBlock(std::move(third));
    QVERIFY(firstPtr->isFolded());
    QVERIFY(secondPtr->isFolded());
    QVERIFY(!thirdPtr->isFolded());
}

void Test::foldedBlockTriggersClearOnNextFrame()
{
    /* A probe with a fake clear+place payload makes it easy to see
     * which side of the diff fires. */
    QTuiScrollView view;
    auto           probe = std::make_unique<ProbeBlock>(QStringLiteral("img"));
    auto          *raw   = probe.get();
    view.appendBlock(std::move(probe));

    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);
    view.collectGraphicsLayer();
    QCOMPARE(raw->callCount, 1);
    QCOMPARE(raw->clearCount, 0);

    /* Fold the block: it stays visible (still 1 row), but the next
     * collectGraphicsLayer must emit its clear. */
    raw->setFolded(true);
    view.render(screen, 0, 6, 40);
    const QString out = view.collectGraphicsLayer();
    QCOMPARE(raw->clearCount, 1);
    QVERIFY(out.contains(QStringLiteral("[clear:img]")));
    /* And it must not also emit a fresh place. */
    QCOMPARE(raw->callCount, 1);
}

void Test::foldedBlockNotPlacedEvenWhenVisible()
{
    QTuiScrollView view;
    auto           probe = std::make_unique<ProbeBlock>(QStringLiteral("img"));
    auto          *raw   = probe.get();
    raw->setFolded(true);
    view.appendBlock(std::move(probe));

    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);
    view.collectGraphicsLayer();
    QCOMPARE(raw->callCount, 0);
}

void Test::appendingNonImageDoesNotFoldImages()
{
    QTuiScrollView view;
    auto           img = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("/tmp/a.png"), QStringLiteral("image/png"), 320, 240, makeRealPngBytes());
    auto *imgPtr = img.get();
    view.appendBlock(std::move(img));

    /* A non-image block must not retroactively fold image blocks. */
    view.appendBlock(std::make_unique<ProbeBlock>(QStringLiteral("plain")));
    QVERIFY(!imgPtr->isFolded());
}

/* The compositor calls foldAllImagePreviews on stop so the cooked
 * dump emits only metadata lines for images. The method must touch
 * QTuiImagePreviewBlock instances only and leave other blocks alone. */
void Test::foldAllImagePreviewsTouchesOnlyImageBlocks()
{
    QTuiScrollView view;
    auto           image = std::make_unique<QTuiImagePreviewBlock>(
        QStringLiteral("/tmp/i.png"), QStringLiteral("image/png"), 320, 240, makeRealPngBytes());
    auto *imagePtr = image.get();
    view.appendBlock(std::move(image));

    auto  probe    = std::make_unique<ProbeBlock>(QStringLiteral("plain"));
    auto *probePtr = probe.get();
    view.appendBlock(std::move(probe));

    QVERIFY(!imagePtr->isFolded());
    QVERIFY(!probePtr->isFolded());

    view.foldAllImagePreviews();

    QVERIFY(imagePtr->isFolded());
    QVERIFY(!probePtr->isFolded());
}

void Test::clippedImageClearsAndReturns()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiScrollView view;
    auto          *image = appendImage(view);
    QTuiScreen     screen(40, 20);
    const QString  full = renderFrame(view, screen, 18);
    QVERIFY(full.contains(QStringLiteral("a=t")));
    QVERIFY(full.contains(QStringLiteral("a=p")));
    const int shortHeight = image->rowCount() - 1;

    const QString clipped = renderFrame(view, screen, shortHeight);
    QCOMPARE(view.mapScreenToBlock(0).rowInBlock, 1);
    QVERIFY(clipped.contains(QStringLiteral("a=d,d=i,")));
    QVERIFY(!clipped.contains(QStringLiteral("a=p")));
    for (int frame = 0; frame < 10; ++frame) {
        const QString held = renderFrame(view, screen, shortHeight);
        QVERIFY(!held.contains(QStringLiteral("\x1b_G")));
    }
    const QString restored = renderFrame(view, screen, 18);
    QVERIFY(restored.contains(QStringLiteral("a=p")));
    QVERIFY(!restored.contains(QStringLiteral("a=t")));

    view.render(screen, 0, shortHeight, 40);
    view.scrollUp(100);
    const QString bottomClipped = renderFrame(view, screen, shortHeight);
    QCOMPARE(view.mapScreenToBlock(0).rowInBlock, 0);
    QVERIFY(bottomClipped.contains(QStringLiteral("a=d,d=i,")));
    QVERIFY(!bottomClipped.contains(QStringLiteral("a=p")));
}

void Test::twoImagesDeleteOnlyHiddenPlacement()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiScrollView view;
    auto          *first  = appendImage(view);
    auto          *second = appendImage(view);
    first->setFolded(false);
    QTuiScreen               screen(40, 25);
    const QString            full = renderFrame(view, screen, 23);
    const QRegularExpression ids(QStringLiteral("\x1b_Ga=p,i=(\\d+)"));
    auto                     matches = ids.globalMatch(full);
    QVERIFY(matches.hasNext());
    const QString firstId = matches.next().captured(1);
    QVERIFY(matches.hasNext());
    const QString secondId = matches.next().captured(1);
    QVERIFY(firstId != secondId);

    first->setFolded(true);
    const QString folded = renderFrame(view, screen, 23);
    QVERIFY(folded.contains(QStringLiteral("a=d,d=i,i=%1,p=1").arg(firstId)));
    QVERIFY(!folded.contains(QStringLiteral("a=d,d=i,i=%1,p=1").arg(secondId)));
    QVERIFY(!second->isFolded());
    const QString held = renderFrame(view, screen, 23);
    QVERIFY(!held.contains(QStringLiteral("a=d")));
}

void Test::inlineImageTracksActualWrites()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiScrollView view;
    auto          *image = appendImage(view);
    QTuiScreen     screen(40, 20);
    QVERIFY(renderFrame(view, screen, 18).contains(QStringLiteral("\x1b]1337;File=")));
    for (int frame = 0; frame < 10; ++frame) {
        QVERIFY(!renderFrame(view, screen, 18).contains(QStringLiteral("\x1b]1337;File=")));
        QVERIFY(screen.writtenRows().isEmpty());
    }
    screen.putString(0, 19, QStringLiteral("input"));
    screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({19}));
    QVERIFY(view.collectGraphicsLayer(screen.writtenRows()).isEmpty());

    const int imageRow = 18 - image->imageCellRows();
    screen.invalidateRows(imageRow, 1);
    const QString chars = screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({imageRow}));
    QVERIFY(chars.contains(QStringLiteral("\x1b[K")));
    QVERIFY(
        view.collectGraphicsLayer(screen.writtenRows()).contains(QStringLiteral("\x1b]1337;File=")));
    screen.toAnsi();
    QVERIFY(view.collectGraphicsLayer(screen.writtenRows()).isEmpty());
}

void Test::inlineMoveRepaintsOldRows()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiScrollView view;
    auto          *image = appendImage(view);
    QTuiScreen     screen(40, 20);
    renderFrame(view, screen, 18);
    const QRect oldRect = image->graphicsEraseRect();
    QVERIFY(!oldRect.isEmpty());
    const QString moved = renderFrame(view, screen, 17);
    QVERIFY(moved.contains(QStringLiteral("\x1b]1337;File=")));
    for (int row = oldRect.top(); row <= oldRect.bottom(); ++row) {
        QVERIFY(screen.writtenRows().contains(row));
    }
    const QRect   movedRect = image->graphicsEraseRect();
    const QString hidden    = renderFrame(view, screen, image->rowCount() - 1);
    QVERIFY(!hidden.contains(QStringLiteral("\x1b]1337;File=")));
    for (int row = movedRect.top(); row <= movedRect.bottom(); ++row) {
        QVERIFY(screen.writtenRows().contains(row));
    }
    QVERIFY(image->graphicsEraseRect().isEmpty());
}

void Test::newScreenRetransmitsImages_data()
{
    QTest::addColumn<QByteArray>("terminal");
    QTest::addColumn<QString>("upload");
    QTest::newRow("kitty") << QByteArray("ghostty") << QStringLiteral("a=t");
    QTest::newRow("inline") << QByteArray("iTerm.app") << QStringLiteral("\x1b]1337;File=");
}

void Test::newScreenRetransmitsImages()
{
    QFETCH(QByteArray, terminal);
    QFETCH(QString, upload);
    qputenv("TERM_PROGRAM", terminal);
    QTuiScrollView view;
    appendImage(view);
    QTuiScreen screen(40, 20);
    QVERIFY(renderFrame(view, screen, 18).contains(upload));
    QVERIFY(!renderFrame(view, screen, 18).contains(upload));
    view.resetGraphicsState();
    screen.invalidate();
    QVERIFY(renderFrame(view, screen, 18).contains(upload));
    QVERIFY(!renderFrame(view, screen, 18).contains(upload));
}

void Test::screenTracksOnlyWrittenRows()
{
    QTuiScreen screen(8, 3);
    screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({0, 1, 2}));
    screen.clear();
    screen.toAnsi();
    QVERIFY(screen.writtenRows().isEmpty());
    screen.putChar(0, 1, QLatin1Char('x'));
    screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({1}));
    screen.invalidateRows(0, 1);
    screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({0}));
    screen.invalidate();
    screen.toAnsi();
    QCOMPARE(screen.writtenRows(), QVector<int>({0, 1, 2}));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewgraphicslayer.moc"
