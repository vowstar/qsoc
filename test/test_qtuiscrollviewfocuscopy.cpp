// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void blockAtScreenRowResolvesFocus();
    void mapScreenToBlockResolvesBlockAndRow();
    void renderTintsFocusedBlockBackground();
    void copyFocusedReturnsBlockMarkdown();
    void copyFocusedReturnsEmptyWhenNoFocus();
    void clearingFocusRestoresDefaultBackground();
};

void Test::blockAtScreenRowResolvesFocus()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("alpha")));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("bravo")));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("charlie")));

    QTuiScreen screen(20, 5);
    view.render(screen, 0, 5, 20);

    /* 3 single-line blocks, bottom-aligned at rows 2, 3, 4. */
    QCOMPARE(view.blockAtScreenRow(2), 0);
    QCOMPARE(view.blockAtScreenRow(3), 1);
    QCOMPARE(view.blockAtScreenRow(4), 2);
    /* Empty rows above the content map to no block. */
    QCOMPARE(view.blockAtScreenRow(0), -1);
    QCOMPARE(view.blockAtScreenRow(1), -1);
}

void Test::mapScreenToBlockResolvesBlockAndRow()
{
    /* A multi-row block (fenced code) lets us check that a screen row
     * resolves to both its block and the row index within that block,
     * and that blank area above the content maps to no block. */
    QTuiScrollView view;
    view.appendBlock(
        std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("```\naaa\nbbb\nccc\n```\n")));

    QTuiScreen screen(20, 12);
    view.render(screen, 0, 12, 20);

    int firstRow = -1;
    for (int r = 0; r < 12; ++r) {
        if (view.mapScreenToBlock(r).blockIdx == 0) {
            firstRow = r;
            break;
        }
    }
    QVERIFY(firstRow > 0); /* content is bottom-aligned, blank rows above */

    const auto m0 = view.mapScreenToBlock(firstRow);
    QCOMPARE(m0.blockIdx, 0);
    QCOMPARE(m0.rowInBlock, 0);
    const auto m1 = view.mapScreenToBlock(firstRow + 1);
    QCOMPARE(m1.blockIdx, 0);
    QCOMPARE(m1.rowInBlock, 1);

    /* Blank area above the content maps to no block. */
    QCOMPARE(view.mapScreenToBlock(0).blockIdx, -1);
    QCOMPARE(view.mapScreenToBlock(0).rowInBlock, -1);
    /* Out-of-range rows map to nothing. */
    QCOMPARE(view.mapScreenToBlock(999).blockIdx, -1);
}

void Test::renderTintsFocusedBlockBackground()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("first")));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("second")));

    view.setFocusedBlockIdx(1);
    QCOMPARE(view.focusedBlockIdx(), 1);

    QTuiScreen screen(20, 4);
    view.render(screen, 0, 4, 20);

    /* Row 2 = first (no tint). Row 3 = second (focused, tinted). */
    QCOMPARE(screen.at(0, 2).bgColor, BG_DEFAULT);
    QVERIFY(screen.at(0, 3).bgColor != BG_DEFAULT);
    QVERIFY(screen.at(1, 3).bgColor != BG_DEFAULT);
    /* Block content remains intact under the tint. */
    QCOMPARE(screen.at(0, 3).character, QChar(QLatin1Char('s')));
}

void Test::copyFocusedReturnsBlockMarkdown()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("# title")));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("paragraph **bold**")));

    view.setFocusedBlockIdx(1);
    QCOMPARE(view.copyFocusedAsMarkdown(), QStringLiteral("paragraph **bold**"));
    QCOMPARE(view.copyFocusedAsPlainText(), QStringLiteral("paragraph **bold**"));
}

void Test::copyFocusedReturnsEmptyWhenNoFocus()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("only")));
    QCOMPARE(view.focusedBlockIdx(), -1);
    QVERIFY(view.copyFocusedAsMarkdown().isEmpty());
}

void Test::clearingFocusRestoresDefaultBackground()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("a")));
    view.setFocusedBlockIdx(0);

    QTuiScreen screen(20, 3);
    view.render(screen, 0, 3, 20);
    QVERIFY(screen.at(1, 2).bgColor != BG_DEFAULT);

    view.setFocusedBlockIdx(-1);
    QTuiScreen screen2(20, 3);
    view.render(screen2, 0, 3, 20);
    QCOMPARE(screen2.at(1, 2).bgColor, BG_DEFAULT);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewfocuscopy.moc"
