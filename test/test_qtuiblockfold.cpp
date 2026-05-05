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
    void unfoldedBlockHasMultipleRows();
    void foldedBlockCollapsesToSummaryRow();
    void unfoldRestoresOriginalRowCount();
    void toggleFocusedFoldRoundTrips();
};

void Test::unfoldedBlockHasMultipleRows()
{
    QTuiAssistantTextBlock block(
        QStringLiteral("# Heading\n\nfirst paragraph\n\nsecond paragraph\n"));
    block.layout(40);
    QVERIFY(block.rowCount() >= 3);
}

void Test::foldedBlockCollapsesToSummaryRow()
{
    QTuiAssistantTextBlock block(QStringLiteral("line one\n\nline two\n\nline three\n"));
    block.layout(40);
    QVERIFY(block.rowCount() >= 3);
    block.setFolded(true);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
    QVERIFY(block.isFoldable());
    QVERIFY(block.isFolded());
}

void Test::unfoldRestoresOriginalRowCount()
{
    QTuiAssistantTextBlock block(QStringLiteral("alpha\n\nbravo\n\ncharlie\n"));
    block.layout(40);
    const int original = block.rowCount();
    block.setFolded(true);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
    block.setFolded(false);
    block.layout(40);
    QCOMPARE(block.rowCount(), original);
}

void Test::toggleFocusedFoldRoundTrips()
{
    QTuiScrollView view;
    view.appendBlock(
        std::make_unique<QTuiAssistantTextBlock>(
            QStringLiteral("first line\n\nsecond line\n\nthird line\n")));
    view.setFocusedBlockIdx(0);

    /* Layout once via render() so the block has a row count. */
    QTuiScreen screen(40, 10);
    view.render(screen, 0, 10, 40);
    QVERIFY(view.totalLines() == 1);

    view.toggleFocusedFold();
    view.render(screen, 0, 10, 40);
    /* After fold: still 1 block, but block reports 1 row internally. */

    view.toggleFocusedFold();
    view.render(screen, 0, 10, 40);
    /* Toggle back to unfolded; markdown source is preserved verbatim. */
    QCOMPARE(
        view.copyFocusedAsMarkdown(), QStringLiteral("first line\n\nsecond line\n\nthird line\n"));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblockfold.moc"
