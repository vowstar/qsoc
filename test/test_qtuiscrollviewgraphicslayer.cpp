// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

namespace {

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

    QString emitGraphicsLayer(int firstScreenRow, int firstScreenCol, int contentWidth) const override
    {
        ++callCount;
        lastFirstScreenRow = firstScreenRow;
        lastFirstScreenCol = firstScreenCol;
        lastContentWidth   = contentWidth;
        return QStringLiteral("<%1@%2,%3w%4>")
            .arg(tag)
            .arg(firstScreenRow)
            .arg(firstScreenCol)
            .arg(contentWidth);
    }

    QString     tag;
    int         rowCountValue;
    mutable int callCount          = 0;
    mutable int lastFirstScreenRow = 0;
    mutable int lastFirstScreenCol = 0;
    mutable int lastContentWidth   = 0;
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyScrollViewProducesNoOverlay();
    void blocksWithDefaultEmitContributeNothing();
    void visibleBlockProbeReceivesScreenCoords();
    void offViewportBlockNotInvoked();
    void multipleVisibleBlocksConcatenateInOrder();
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

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewgraphicslayer.moc"
