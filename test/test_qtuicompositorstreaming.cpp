// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void chunksAccumulateInSingleAssistantBlock();
    void interveningPrintContentSealsAssistantBlock();
    void finishStreamSealsCurrentBlock();
    void reasoningChunksLandOnSeparateBlock();
};

void Test::chunksAccumulateInSingleAssistantBlock()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(QStringLiteral("hello "));
    compositor.appendAssistantChunk(QStringLiteral("**world**"));

    QCOMPARE(compositor.contentView().totalLines(), 1);
    QVERIFY(compositor.contentView().toPlainText().contains(QStringLiteral("hello **world**")));
}

void Test::interveningPrintContentSealsAssistantBlock()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(QStringLiteral("first"));
    /* Tool / system message lands in scrollback. */
    compositor.printContent(QStringLiteral("$ tool ran\n"), QTuiScrollView::Bold);
    /* New chunk must start a fresh block instead of back-filling
     * onto the original "first" block. */
    compositor.appendAssistantChunk(QStringLiteral("second"));

    QVERIFY(compositor.contentView().totalLines() >= 3);
    /* Order check: "first" comes before "$ tool ran" comes before "second". */
    const QString flat = compositor.contentView().toPlainText();
    const int     a    = flat.indexOf(QStringLiteral("first"));
    const int     b    = flat.indexOf(QStringLiteral("$ tool ran"));
    const int     c    = flat.indexOf(QStringLiteral("second"));
    QVERIFY(a >= 0 && b > a && c > b);
}

void Test::finishStreamSealsCurrentBlock()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(QStringLiteral("a"));
    compositor.finishStream();
    compositor.appendAssistantChunk(QStringLiteral("b"));
    QCOMPARE(compositor.contentView().totalLines(), 2);
}

void Test::reasoningChunksLandOnSeparateBlock()
{
    QTuiCompositor compositor;
    compositor.appendReasoningChunk(QStringLiteral("thinking..."));
    compositor.appendAssistantChunk(QStringLiteral("answer"));
    /* Two distinct blocks: reasoning, then assistant. */
    QCOMPARE(compositor.contentView().totalLines(), 2);
    const QString flat = compositor.contentView().toPlainText();
    QVERIFY(flat.indexOf(QStringLiteral("thinking")) < flat.indexOf(QStringLiteral("answer")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuicompositorstreaming.moc"
