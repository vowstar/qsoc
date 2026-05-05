// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuicodeblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyBodyShowsBannerOnly();
    void bodyEachLineGetsAGutterRow();
    void plainTextIsRawCodeNoFenceNoBanner();
    void markdownWrapsBodyInFencedBlock();
    void foldCollapsesToSummary();
    void appendBodyExtendsAndInvalidatesLayout();
    void wideCodeRowReportsHorizontalOverflow();
};

void Test::emptyBodyShowsBannerOnly()
{
    QTuiCodeBlock block(QStringLiteral("python"), QString(), false, 1);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::bodyEachLineGetsAGutterRow()
{
    QTuiCodeBlock
        block(QStringLiteral("python"), QStringLiteral("alpha\nbravo\ncharlie\n"), false, 1);
    block.layout(40);
    /* banner + 3 body rows */
    QCOMPARE(block.rowCount(), 4);
}

void Test::plainTextIsRawCodeNoFenceNoBanner()
{
    QTuiCodeBlock
        block(QStringLiteral("python"), QStringLiteral("def f():\n    return 1\n"), false, 1);
    const QString text = block.toPlainText();
    /* No ``` fences, no banner, no gutter. */
    QVERIFY(!text.contains(QStringLiteral("```")));
    QVERIFY(!text.contains(QStringLiteral("┄")));
    QVERIFY(!text.contains(QStringLiteral("▎")));
    QCOMPARE(text, QStringLiteral("def f():\n    return 1\n"));
}

void Test::markdownWrapsBodyInFencedBlock()
{
    QTuiCodeBlock block(QStringLiteral("python"), QStringLiteral("print(1)\n"), false, 1);
    QCOMPARE(block.toMarkdown(), QStringLiteral("```python\nprint(1)\n```\n"));
}

void Test::foldCollapsesToSummary()
{
    QTuiCodeBlock block(QStringLiteral("python"), QStringLiteral("a\nb\nc\nd\n"), false, 1);
    block.layout(40);
    QVERIFY(block.rowCount() > 1);
    block.setFolded(true);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::appendBodyExtendsAndInvalidatesLayout()
{
    QTuiCodeBlock block(QStringLiteral("bash"), QStringLiteral("ls\n"), false, 1);
    block.layout(40);
    const int before = block.rowCount();
    block.appendBody(QStringLiteral("pwd\n"));
    block.layout(40);
    QVERIFY(block.rowCount() > before);
}

void Test::wideCodeRowReportsHorizontalOverflow()
{
    QTuiCodeBlock block(
        QStringLiteral("python"), QString(120, QLatin1Char('x')) + QLatin1Char('\n'), false, 1);
    block.layout(40);
    QVERIFY(block.maxXOffset(40) > 0);
}

QSOC_TEST_MAIN(Test)
#include "test_qtuicodeblock.moc"
