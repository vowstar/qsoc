// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

namespace {

/* Test-only block that reports a fixed maxXOffset and remembers the
 * xOffset it was last asked to paint with. Lets us drive scroll via
 * the public API and verify the value reaches paintRow. */
class HScrollMock : public QTuiBlock
{
public:
    int maxOffset       = 0;
    int lastPaintOffset = -1;

    void layout(int width) override
    {
        Q_UNUSED(width);
        layoutDirty = false;
    }

    int rowCount() const override { return 1; }

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
        Q_UNUSED(width);
        Q_UNUSED(focused);
        Q_UNUSED(selected);
        const_cast<HScrollMock *>(this)->lastPaintOffset = xOffset;
    }

    int maxXOffset(int width) const override
    {
        Q_UNUSED(width);
        return maxOffset;
    }
    QString toPlainText() const override { return QStringLiteral("mock"); }
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void scrollFocusedHorizontalAdvancesAndClamps();
    void scrollDoesNothingWhenBlockHasNoOverflow();
    void paintRowSeesCurrentXOffset();
};

void Test::scrollFocusedHorizontalAdvancesAndClamps()
{
    QTuiScrollView view;
    auto           mockOwn = std::make_unique<HScrollMock>();
    auto          *mock    = mockOwn.get();
    mock->maxOffset        = 5;
    view.appendBlock(std::move(mockOwn));
    view.setFocusedBlockIdx(0);

    /* Layout once via render() so lastRenderWidth caches. */
    QTuiScreen screen(40, 4);
    view.render(screen, 0, 4, 40);

    view.scrollFocusedHorizontal(1);
    QCOMPARE(mock->xOffset(), 1);
    view.scrollFocusedHorizontal(3);
    QCOMPARE(mock->xOffset(), 4);
    view.scrollFocusedHorizontal(5);
    QCOMPARE(mock->xOffset(), 5); /* Clamped at maxOffset */
    view.scrollFocusedHorizontal(-2);
    QCOMPARE(mock->xOffset(), 3);
    view.scrollFocusedHorizontal(-100);
    QCOMPARE(mock->xOffset(), 0); /* Clamped at zero */
}

void Test::scrollDoesNothingWhenBlockHasNoOverflow()
{
    QTuiScrollView view;
    auto           mockOwn = std::make_unique<HScrollMock>();
    auto          *mock    = mockOwn.get();
    mock->maxOffset        = 0;
    view.appendBlock(std::move(mockOwn));
    view.setFocusedBlockIdx(0);

    QTuiScreen screen(40, 4);
    view.render(screen, 0, 4, 40);

    view.scrollFocusedHorizontal(1);
    QCOMPARE(mock->xOffset(), 0);
}

void Test::paintRowSeesCurrentXOffset()
{
    QTuiScrollView view;
    auto           mockOwn = std::make_unique<HScrollMock>();
    auto          *mock    = mockOwn.get();
    mock->maxOffset        = 10;
    view.appendBlock(std::move(mockOwn));
    view.setFocusedBlockIdx(0);

    QTuiScreen screen(40, 4);
    view.render(screen, 0, 4, 40);
    view.scrollFocusedHorizontal(7);
    view.render(screen, 0, 4, 40);

    QCOMPARE(mock->lastPaintOffset, 7);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_hscroll.moc"
