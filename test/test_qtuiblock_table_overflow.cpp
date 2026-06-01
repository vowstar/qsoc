// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void narrowTableProducesRecordRows();
    void plainProseWrapsToMultipleRows();
    void longCodeLineWrapsWhenNarrow();
};

void Test::narrowTableProducesRecordRows()
{
    /* A 5-column table that cannot fit at width 20 degrades to vertical
     * key/value records (one row per cell), so a single body row yields
     * at least 5 rows instead of overflowing. */
    const QString markdown = QStringLiteral(
        "| Column One | Column Two | Column Three | Column Four | Column Five |\n"
        "|---|---|---|---|---|\n"
        "| value-aaaa | value-bbbb | value-cccc | value-dddd | value-eeee |\n");
    QTuiAssistantTextBlock block(markdown);
    block.layout(20);
    QVERIFY(block.rowCount() >= 5);
}

void Test::plainProseWrapsToMultipleRows()
{
    /* Long prose soft-wraps to the viewport width across several rows. */
    QTuiAssistantTextBlock block(
        QStringLiteral("alpha bravo charlie delta echo foxtrot golf hotel india"));
    block.layout(15);
    QVERIFY(block.rowCount() >= 3);
}

void Test::longCodeLineWrapsWhenNarrow()
{
    /* Code blocks now wrap instead of horizontally scrolling: a long
     * code line occupies more rows at a narrow width than at a wide
     * one. */
    const QString markdown = QStringLiteral(
        "```\n"
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n"
        "```\n");
    QTuiAssistantTextBlock block(markdown);

    block.layout(20);
    const int narrowRows = block.rowCount();
    block.layout(200);
    const int wideRows = block.rowCount();

    QVERIFY(wideRows >= 2);         /* banner + one code row */
    QVERIFY(narrowRows > wideRows); /* the long line wrapped */
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_table_overflow.moc"
