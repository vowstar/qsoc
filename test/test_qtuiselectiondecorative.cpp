// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuicodeblock.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuiuserblock.h"

#include <QtTest>

#include <memory>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void codeBlockBannerAndGutterCellsAreDecorative();
    void userBlockLeftEdgeCellIsDecorative();
    void scrollbarColumnIsDecorative();
    void codeBodyCellsAreContent();
};

void Test::codeBlockBannerAndGutterCellsAreDecorative()
{
    QTuiCodeBlock block(QStringLiteral("python"), QStringLiteral("alpha\n"), false, 1);
    block.layout(40);
    QTuiScreen screen(40, 4);
    /* Banner row */
    block.paintRow(screen, 0, 0, 0, 40, false, false);
    QVERIFY(screen.at(0, 0).decorative);
    /* Body row: cells 0..1 are gutter `▎ ` (decorative); cell 2+ is the
     * code text (content). */
    block.paintRow(screen, 1, 1, 0, 40, false, false);
    QVERIFY(screen.at(0, 1).decorative);  /* ▎ */
    QVERIFY(screen.at(1, 1).decorative);  /* space after gutter */
    QVERIFY(!screen.at(2, 1).decorative); /* 'a' of "alpha" */
}

void Test::userBlockLeftEdgeCellIsDecorative()
{
    QTuiUserBlock block(QStringLiteral("hello"));
    block.layout(40);
    QTuiScreen screen(40, 2);
    block.paintRow(screen, 0, 0, 0, 40, false, false);
    /* `▍ ` gutter at cells 0..1 */
    QVERIFY(screen.at(0, 0).decorative);
    QVERIFY(screen.at(1, 0).decorative);
    /* 'h' of "hello" at cell 2 */
    QVERIFY(!screen.at(2, 0).decorative);
}

void Test::scrollbarColumnIsDecorative()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiUserBlock>(QStringLiteral("x")));
    QTuiScreen screen(20, 4);
    view.render(screen, 0, 4, 20);
    /* Rightmost column = scrollbar; every row marked decorative. */
    for (int row = 0; row < 4; ++row) {
        QVERIFY2(
            screen.at(19, row).decorative,
            qPrintable(QStringLiteral("scrollbar row %1 not decorative").arg(row)));
    }
}

void Test::codeBodyCellsAreContent()
{
    QTuiCodeBlock block(QStringLiteral("bash"), QStringLiteral("ls -la\n"), false, 1);
    block.layout(40);
    QTuiScreen screen(40, 3);
    block.paintRow(screen, 0, 1, 0, 40, false, false); /* body row */
    /* After `▎ ` gutter (cells 0..1), cell 2 = 'l', 3 = 's', 4 = ' ',
     * 5 = '-', 6 = 'l', 7 = 'a' — all content (not decorative). */
    QVERIFY(!screen.at(2, 0).decorative);
    QVERIFY(!screen.at(3, 0).decorative);
    QVERIFY(!screen.at(4, 0).decorative);
    QVERIFY(!screen.at(5, 0).decorative);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiselectiondecorative.moc"
