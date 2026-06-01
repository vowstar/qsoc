// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void wrappedParagraphCopiesAsSingleLine();
    void blockquoteCopyExcludesGutter();
    void columnSubrangeWithinRowCopiesSubstring();
    void foldedBlockDeclinesMapping();
};

void Test::wrappedParagraphCopiesAsSingleLine()
{
    /* A paragraph that soft-wraps over several visual rows must copy
     * back as one logical line, with the wrap-point spaces preserved
     * and no mid-word fragmentation. */
    const QString          text = QStringLiteral("alpha bravo charlie delta");
    QTuiAssistantTextBlock block(text);
    block.layout(15);
    QVERIFY(block.rowCount() >= 2); /* confirm it actually wrapped */

    const QString copied = block.selectedLogicalText(0, 0, block.rowCount() - 1, 1000);
    QCOMPARE(copied, text);
}

void Test::blockquoteCopyExcludesGutter()
{
    /* The "│ " blockquote gutter is decorative; copy must omit it. */
    QTuiAssistantTextBlock block(QStringLiteral("> hello world"));
    block.layout(40);

    const QString copied = block.selectedLogicalText(0, 0, block.rowCount() - 1, 1000);
    QCOMPARE(copied, QStringLiteral("hello world"));
}

void Test::columnSubrangeWithinRowCopiesSubstring()
{
    QTuiAssistantTextBlock block(QStringLiteral("hello world"));
    block.layout(40);

    /* Columns 0..4 cover "hello"; the space at column 5 is excluded. */
    const QString copied = block.selectedLogicalText(0, 0, 0, 4);
    QCOMPARE(copied, QStringLiteral("hello"));
}

void Test::foldedBlockDeclinesMapping()
{
    /* A folded block has only a summary row with no logical mapping;
     * it must return a null QString so the caller falls back to the
     * decorative-aware screen scrape. */
    QTuiAssistantTextBlock block(QStringLiteral("alpha bravo charlie delta"));
    block.setFolded(true);
    block.layout(15);

    const QString copied = block.selectedLogicalText(0, 0, block.rowCount() - 1, 1000);
    QVERIFY(copied.isNull());
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiblock_logicalcopy.moc"
