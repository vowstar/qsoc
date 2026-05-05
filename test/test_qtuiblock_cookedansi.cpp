// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuidiffblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void textBlockToAnsiContainsBoldEscape();
    void diffBlockToAnsiColorsAddAndDel();
    void scrollViewToAnsiConcatenatesBlocks();
    void zeroWidthYieldsEmptyString();
};

void Test::textBlockToAnsiContainsBoldEscape()
{
    QTuiAssistantTextBlock block(QStringLiteral("**hello**"));
    const QString          out = block.toAnsi(40);
    QVERIFY(out.contains(QStringLiteral("\033[")));
    QVERIFY(out.contains(QStringLiteral("hello")));
    /* SGR 1 = bold */
    QVERIFY(out.contains(QStringLiteral("1;")) || out.contains(QStringLiteral("[1m")));
}

void Test::diffBlockToAnsiColorsAddAndDel()
{
    QTuiDiffBlock block(QStringLiteral("a/x"), QStringLiteral("b/x"));
    block.addRow(QTuiDiffBlock::Kind::Add, QStringLiteral("+ok"));
    block.addRow(QTuiDiffBlock::Kind::Del, QStringLiteral("-bad"));
    const QString out = block.toAnsi(40);
    /* Green palette index 142 for added rows, red 167 for removed. */
    QVERIFY(out.contains(QStringLiteral("38;5;142")));
    QVERIFY(out.contains(QStringLiteral("38;5;167")));
}

void Test::scrollViewToAnsiConcatenatesBlocks()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("alpha")));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("**bravo**")));
    const QString out = view.toAnsi(40);
    QVERIFY(out.contains(QStringLiteral("alpha")));
    QVERIFY(out.contains(QStringLiteral("bravo")));
}

void Test::zeroWidthYieldsEmptyString()
{
    QTuiAssistantTextBlock block(QStringLiteral("anything"));
    QCOMPARE(block.toAnsi(0), QString());
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_cookedansi.moc"
