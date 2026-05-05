// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void narrowTableExposesHorizontalScroll();
    void plainProseHasNoHorizontalScroll();
    void scrollOffsetClipsLeftSide();
};

void Test::narrowTableExposesHorizontalScroll()
{
    /* A 5-column table whose minimum row width inevitably exceeds 20
     * cells. With layoutWidth=20 the renderer cannot fit it; the
     * block must report a non-zero maxXOffset so the user can pan
     * across with Shift+Right. */
    const QString markdown = QStringLiteral(
        "| Column One | Column Two | Column Three | Column Four | Column Five |\n"
        "|---|---|---|---|---|\n"
        "| value-aaaa | value-bbbb | value-cccc | value-dddd | value-eeee |\n");
    QTuiAssistantTextBlock block(markdown);
    block.layout(20);
    QVERIFY(block.rowCount() >= 3);
    QVERIFY(block.maxXOffset(20) > 0);
}

void Test::plainProseHasNoHorizontalScroll()
{
    QTuiAssistantTextBlock block(
        QStringLiteral("alpha bravo charlie delta echo foxtrot golf hotel\n"));
    block.layout(15);
    QCOMPARE(block.maxXOffset(15), 0);
}

void Test::scrollOffsetClipsLeftSide()
{
    /* Same wide-table; with xOffset > 0 the leftmost cells should be
     * clipped from paintRow output. We verify by checking that the
     * first painted character at xOffset=4 is not the same as at
     * xOffset=0. */
    const QString markdown = QStringLiteral(
        "| AAAAAAA | BBBBBBB | CCCCCCC | DDDDDDD |\n"
        "|---|---|---|---|\n"
        "| 1 | 2 | 3 | 4 |\n");
    QTuiAssistantTextBlock block(markdown);
    block.layout(20);
    QVERIFY(block.maxXOffset(20) > 0);

    QTuiScreen left(20, 6);
    block.paintRow(left, 0, 0, 0, 20, false, false);
    const QChar leftFirst = left.at(0, 0).character;

    QTuiScreen shifted(20, 6);
    block.paintRow(shifted, 0, 0, 4, 20, false, false);
    const QChar shiftedFirst = shifted.at(0, 0).character;

    QVERIFY(leftFirst != shiftedFirst);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_table_overflow.moc"
