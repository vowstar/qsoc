// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiuserblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void singleLineInputProducesOneRow();
    void multiLineInputProducesOneRowPerLine();
    void trailingNewlineDoesNotAddPhantomRow();
    void plainTextRoundsTripsExactly();
    void markdownEmitsBlockquoteRows();
};

void Test::singleLineInputProducesOneRow()
{
    QTuiUserBlock block(QStringLiteral("hello"));
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::multiLineInputProducesOneRowPerLine()
{
    QTuiUserBlock block(QStringLiteral("alpha\nbravo\ncharlie"));
    block.layout(40);
    QCOMPARE(block.rowCount(), 3);
}

void Test::trailingNewlineDoesNotAddPhantomRow()
{
    QTuiUserBlock block(QStringLiteral("alpha\n"));
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::plainTextRoundsTripsExactly()
{
    QTuiUserBlock block(QStringLiteral("/help"));
    QCOMPARE(block.toPlainText(), QStringLiteral("/help"));
}

void Test::markdownEmitsBlockquoteRows()
{
    QTuiUserBlock block(QStringLiteral("first\nsecond"));
    QCOMPARE(block.toMarkdown(), QStringLiteral("> first\n> second\n"));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiuserblock.moc"
