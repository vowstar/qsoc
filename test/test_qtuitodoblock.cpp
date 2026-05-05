// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuitodoblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyListProducesHeaderOnly();
    void itemsAppearOneRowEach();
    void doneItemUsesGfmCheckbox();
    void foldCollapsesToSummary();
    void plainTextNumbersAndMarkers();
};

namespace {
QTuiTodoList::TodoItem make(int identifier, const QString &title, const QString &status)
{
    QTuiTodoList::TodoItem item;
    item.id     = identifier;
    item.title  = title;
    item.status = status;
    return item;
}
} // namespace

void Test::emptyListProducesHeaderOnly()
{
    QTuiTodoBlock block({});
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::itemsAppearOneRowEach()
{
    QTuiTodoBlock block(
        {make(1, QStringLiteral("a"), QStringLiteral("pending")),
         make(2, QStringLiteral("b"), QStringLiteral("done"))});
    block.layout(40);
    QCOMPARE(block.rowCount(), 3); /* header + 2 items */
}

void Test::doneItemUsesGfmCheckbox()
{
    QTuiTodoBlock block({make(7, QStringLiteral("ship it"), QStringLiteral("done"))});
    QCOMPARE(block.toMarkdown(), QStringLiteral("[x] ship it\n"));
}

void Test::foldCollapsesToSummary()
{
    QTuiTodoBlock block(
        {make(1, QStringLiteral("a"), QStringLiteral("pending")),
         make(2, QStringLiteral("b"), QStringLiteral("pending"))});
    block.layout(40);
    QVERIFY(block.rowCount() > 1);
    block.setFolded(true);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::plainTextNumbersAndMarkers()
{
    QTuiTodoBlock block({make(3, QStringLiteral("planning"), QStringLiteral("in_progress"))});
    QCOMPARE(block.toPlainText(), QStringLiteral("3. [~] planning\n"));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuitodoblock.moc"
