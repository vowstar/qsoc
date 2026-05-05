// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiscreen.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyBlockHasZeroRows();
    void simpleParagraphLayoutsAndPaintsBoldRun();
    void appendMarkdownInvalidatesAndExtendsRows();
    void widthChangeReflowsRows();
    void toPlainTextRoundTripsMarkdownSource();
    void copyAfterRenderReturnsSource();
    void paintAtFarRightDoesNotOverflowScreenWidth();
    void cjkCharactersDoNotSplitMidGlyph();
};

void Test::emptyBlockHasZeroRows()
{
    QTuiAssistantTextBlock block;
    block.layout(40);
    QCOMPARE(block.rowCount(), 0);
    QCOMPARE(block.toPlainText(), QString());
}

void Test::simpleParagraphLayoutsAndPaintsBoldRun()
{
    QTuiAssistantTextBlock block(QStringLiteral("hello **world**"));
    block.layout(40);
    QVERIFY(block.rowCount() >= 1);

    QTuiScreen screen(40, 4);
    block.paintRow(screen, 0, 0, 0, 40, false, false);
    QCOMPARE(screen.at(0, 0).character, QChar(QLatin1Char('h')));
    QVERIFY(!screen.at(0, 0).bold);
    /* "hello " (6 chars) then "world" bold */
    QCOMPARE(screen.at(6, 0).character, QChar(QLatin1Char('w')));
    QVERIFY(screen.at(6, 0).bold);
}

void Test::appendMarkdownInvalidatesAndExtendsRows()
{
    QTuiAssistantTextBlock block(QStringLiteral("first line"));
    block.layout(40);
    const int initial = block.rowCount();
    block.appendMarkdown(QStringLiteral("\n\nsecond"));
    block.layout(40);
    QVERIFY(block.rowCount() > initial);
}

void Test::widthChangeReflowsRows()
{
    const QString markdown = QStringLiteral(
        "alpha bravo charlie delta echo foxtrot golf hotel india juliet");
    QTuiAssistantTextBlock block(markdown);
    block.layout(80);
    const int wide = block.rowCount();
    block.layout(20);
    const int narrow = block.rowCount();
    QVERIFY(narrow > wide);
}

void Test::toPlainTextRoundTripsMarkdownSource()
{
    const QString          markdown = QStringLiteral("# Title\n\n- item one\n- item two\n");
    QTuiAssistantTextBlock block(markdown);
    block.layout(40);
    QCOMPARE(block.toPlainText(), markdown);
    QCOMPARE(block.toMarkdown(), markdown);
}

void Test::copyAfterRenderReturnsSource()
{
    QTuiAssistantTextBlock block;
    block.appendMarkdown(QStringLiteral("``` "));
    block.appendMarkdown(QStringLiteral("\ncode\n```\n"));
    block.layout(40);
    QCOMPARE(block.toPlainText(), QStringLiteral("``` \ncode\n```\n"));
}

void Test::paintAtFarRightDoesNotOverflowScreenWidth()
{
    QTuiAssistantTextBlock block(QStringLiteral("xyz"));
    block.layout(10);
    QTuiScreen screen(10, 2);
    block.paintRow(screen, 0, 0, 0, 10, false, false);
    QCOMPARE(screen.at(0, 0).character, QChar(QLatin1Char('x')));
    QCOMPARE(screen.at(2, 0).character, QChar(QLatin1Char('z')));
    /* Trailing cells stay default (' '). */
    QCOMPARE(screen.at(3, 0).character, QChar(QLatin1Char(' ')));
}

void Test::cjkCharactersDoNotSplitMidGlyph()
{
    QTuiAssistantTextBlock block(QStringLiteral("中文文本测试段落"));
    block.layout(6); /* 6 cells = 3 CJK glyphs per row */
    QVERIFY(block.rowCount() >= 2);
    QTuiScreen screen(6, 4);
    block.paintRow(screen, 0, 0, 0, 6, false, false);
    QCOMPARE(screen.at(0, 0).character, QChar(0x4E2D));
    QCOMPARE(screen.at(2, 0).character, QChar(0x6587));
    QCOMPARE(screen.at(4, 0).character, QChar(0x6587));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_text.moc"
