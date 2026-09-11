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
    void toolOutputStaysWithItsCallAcrossMessages();
    void removedToolBlocksRejectLateOutput();
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

void Test::toolOutputStaysWithItsCallAcrossMessages()
{
    QTuiCompositor compositor;
    compositor.beginToolUse("bash", "first command", "run/first");
    compositor.appendToolUseBody("first line\n", "run/first");
    compositor.printContent("peer notification\n");
    compositor.beginToolUse("bash", "second command", "run/second");
    compositor.appendToolUseBody("second output\n", "run/second");
    compositor.appendToolUseBody("first tail\n", "run/first");
    compositor.finishToolUse(QTuiToolBlock::Status::Failure, {}, "run/first");
    compositor.replaceToolUseBody("second final\n", "run/second");
    compositor.finishToolUse(QTuiToolBlock::Status::Success, {}, "run/second");
    const auto text = compositor.contentView().toPlainText();
    QVERIFY(text.contains("first line\nfirst tail\n"));
    QVERIFY(text.indexOf("first tail") < text.indexOf("peer notification"));
    QVERIFY(text.indexOf("peer notification") < text.indexOf("second command"));
    QVERIFY(text.contains("second final"));
    QVERIFY(!text.contains("second output"));
}

void Test::removedToolBlocksRejectLateOutput()
{
    QTuiCompositor compositor;
    compositor.beginToolUse("bash", "old", "old");
    compositor.contentView().clear();
    compositor.beginToolUse("bash", "current", "new");
    compositor.appendToolUseBody("late", "old");
    compositor.finishToolUse(QTuiToolBlock::Status::Success, {}, "old");
    compositor.appendToolUseBody("current output", "new");
    compositor.finishStream();
    QVERIFY(!compositor.contentView().toPlainText().contains("late"));
    QVERIFY(compositor.contentView().toPlainText().contains("current output"));
    auto *block = dynamic_cast<QTuiToolBlock *>(compositor.contentView().lastBlock());
    QVERIFY(block);
    block->layout(80);
    QTuiScreen screen(80, 1);
    block->paintRow(screen, 0, block->rowCount() - 1, 0, 80, false, false);
    QString footer;
    for (int col = 0; col < 80; ++col)
        footer += screen.at(col, 0).character;
    QVERIFY(footer.contains("uncertain"));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuicompositorstreaming.moc"
