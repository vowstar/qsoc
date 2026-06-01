// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuidiffblock.h"
#include "tui/qtuiscreen.h"

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
    void wideRowLaysOut();
    void dragCopyYieldsSignStrippedContent();
    void gutterSignsAndNumbersAreDecorative();
    void longRowWrapsAndCopiesAsOneLine();
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

void Test::wideRowLaysOut()
{
    /* A row far wider than the viewport still lays out without error.
     * Horizontal scroll has been removed; Stage 4 makes diffs wrap. */
    QTuiDiffBlock block(QStringLiteral("a/x"), QStringLiteral("b/x"));
    block.addRow(QTuiDiffBlock::Kind::Add, QString(120, QLatin1Char('A')));
    block.layout(40);
    QVERIFY(block.rowCount() >= 1);
}

void Test::dragCopyYieldsSignStrippedContent()
{
    /* Mouse-drag copy returns the code content without +/- signs, line
     * numbers, or the file/hunk headers. */
    QTuiDiffBlock block(QStringLiteral("a/f"), QStringLiteral("b/f"));
    block.addRow(QTuiDiffBlock::Kind::Hunk, QStringLiteral("@@ -1,2 +1,2 @@"));
    block.addRow(QTuiDiffBlock::Kind::Context, QStringLiteral(" ctx"));
    block.addRow(QTuiDiffBlock::Kind::Del, QStringLiteral("-old"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+new"));
    block.layout(80);

    const QString copied = block.selectedLogicalText(0, 0, block.rowCount() - 1, 1000);
    QCOMPARE(copied, QStringLiteral("ctx\nold\nnew"));
}

void Test::gutterSignsAndNumbersAreDecorative()
{
    /* The line number + sign gutter must be decorative so selection /
     * copy skip it; the payload text must be selectable. */
    QTuiDiffBlock block(QStringLiteral("a/f"), QStringLiteral("b/f"));
    block.addRow(QTuiDiffBlock::Kind::Hunk, QStringLiteral("@@ -1,1 +1,1 @@"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+hello"));
    block.layout(80);

    QTuiScreen screen(80, 8);
    for (int r = 0; r < block.rowCount(); ++r) {
        block.paintRow(screen, r, r, 0, 80, false, false);
    }
    /* Locate the Add row: the one whose non-decorative text is "hello".
     * (The hunk header row also contains '+', but as its own content.) */
    int addRow = -1;
    for (int row = 0; row < 8 && addRow < 0; ++row) {
        QString nonDeco;
        for (int col = 0; col < 80; ++col) {
            const QTuiCell &cell = screen.at(col, row);
            if (!cell.decorative) {
                nonDeco += cell.character;
            }
        }
        if (nonDeco.trimmed() == QStringLiteral("hello")) {
            addRow = row;
        }
    }
    QVERIFY(addRow >= 0); /* content is non-decorative and intact */

    bool gutterSignDecorative = false;
    for (int col = 0; col < 80; ++col) {
        const QTuiCell &cell = screen.at(col, addRow);
        if (cell.character == QLatin1Char('+')) {
            QVERIFY(cell.decorative); /* the sign sits in the gutter */
            gutterSignDecorative = true;
        }
    }
    QVERIFY(gutterSignDecorative);
}

void Test::longRowWrapsAndCopiesAsOneLine()
{
    /* A long add line wraps onto several rows but copies back as one
     * logical line without the sign. */
    const QString content = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    QTuiDiffBlock block(QStringLiteral("a/f"), QStringLiteral("b/f"));
    block.addRow(QTuiDiffBlock::Kind::Hunk, QStringLiteral("@@ -1,1 +1,1 @@"));
    block.addRow(QTuiDiffBlock::Kind::Add, QLatin1Char('+') + content);
    block.layout(20);

    /* 2 headers + hunk + multiple wrapped add rows. */
    QVERIFY(block.rowCount() > 4);
    const QString copied = block.selectedLogicalText(0, 0, block.rowCount() - 1, 1000);
    QCOMPARE(copied, content);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuidiffblock.moc"
