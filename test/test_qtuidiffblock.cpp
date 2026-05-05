// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuidiffblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyDiffStillCarriesHeaders();
    void rowsAccumulateAndLayout();
    void plainTextRoundsTripsAsUnifiedDiff();
    void markdownWrapsInDiffFence();
    void foldCollapsesToSummary();
    void wideRowReportsHorizontalOverflow();
};

void Test::emptyDiffStillCarriesHeaders()
{
    QTuiDiffBlock block(QStringLiteral("a/foo.txt"), QStringLiteral("b/foo.txt"));
    block.layout(80);
    QCOMPARE(block.rowCount(), 2); /* both --- and +++ headers */
}

void Test::rowsAccumulateAndLayout()
{
    QTuiDiffBlock block(QStringLiteral("a/foo.txt"), QStringLiteral("b/foo.txt"));
    block.addRow(QTuiDiffBlock::Kind::Hunk, QStringLiteral("@@ -1,3 +1,4 @@"));
    block.addRow(QTuiDiffBlock::Kind::Context, QStringLiteral(" alpha"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+bravo"));
    block.addRow(QTuiDiffBlock::Kind::Del, QStringLiteral("-charlie"));
    block.layout(80);
    QCOMPARE(block.rowCount(), 6);
}

void Test::plainTextRoundsTripsAsUnifiedDiff()
{
    QTuiDiffBlock block(QStringLiteral("a/foo.txt"), QStringLiteral("b/foo.txt"));
    block.addRow(QTuiDiffBlock::Kind::Hunk, QStringLiteral("@@ -1 +1 @@"));
    block.addRow(QTuiDiffBlock::Kind::Del, QStringLiteral("-old"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+new"));
    const QString out = block.toPlainText();
    QVERIFY(out.startsWith(QStringLiteral("--- a/foo.txt\n")));
    QVERIFY(out.contains(QStringLiteral("+++ b/foo.txt\n")));
    QVERIFY(out.contains(QStringLiteral("@@ -1 +1 @@\n")));
    QVERIFY(out.contains(QStringLiteral("-old\n")));
    QVERIFY(out.contains(QStringLiteral("+new\n")));
}

void Test::markdownWrapsInDiffFence()
{
    QTuiDiffBlock block(QStringLiteral("a/x"), QStringLiteral("b/x"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+ok"));
    const QString md = block.toMarkdown();
    QVERIFY(md.startsWith(QStringLiteral("```diff\n")));
    QVERIFY(md.endsWith(QStringLiteral("```\n")));
    QVERIFY(md.contains(QStringLiteral("+ok")));
}

void Test::foldCollapsesToSummary()
{
    QTuiDiffBlock block(QStringLiteral("a/foo"), QStringLiteral("b/foo"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+1"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+2"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+3"));
    block.layout(80);
    QVERIFY(block.rowCount() > 1);
    block.setFolded(true);
    block.layout(80);
    QCOMPARE(block.rowCount(), 1);
}

void Test::wideRowReportsHorizontalOverflow()
{
    QTuiDiffBlock block(QStringLiteral("a/x"), QStringLiteral("b/x"));
    block.addRow(QTuiDiffBlock::Kind::Add, QString(120, QLatin1Char('A')));
    block.layout(40);
    QVERIFY(block.maxXOffset(40) > 0);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuidiffblock.moc"
