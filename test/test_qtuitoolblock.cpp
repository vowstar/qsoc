// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuitoolblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void runningToolHasOnlyHeaderAndBody();
    void finishedSuccessAddsCheckmarkFooter();
    void finishedFailureAddsCrossFooter();
    void foldingCollapsesToHeaderSummary();
    void plainTextLooksLikeShellHistory();
    void markdownWrapsBodyInFencedBlock();
};

void Test::runningToolHasOnlyHeaderAndBody()
{
    QTuiToolBlock block(QStringLiteral("bash"), QStringLiteral("ls -la"));
    block.appendBody(QStringLiteral("alpha\nbravo\n"));
    block.layout(40);
    /* Header + 2 body lines = 3 rows, no footer. */
    QCOMPARE(block.rowCount(), 3);
}

void Test::finishedSuccessAddsCheckmarkFooter()
{
    QTuiToolBlock block(QStringLiteral("bash"), QStringLiteral("ls"));
    block.appendBody(QStringLiteral("alpha\n"));
    block.finish(QTuiToolBlock::Status::Success, QStringLiteral("ok"));
    block.layout(40);
    QCOMPARE(block.rowCount(), 3); /* header + body + footer */
}

void Test::finishedFailureAddsCrossFooter()
{
    QTuiToolBlock block(QStringLiteral("bash"), QStringLiteral("false"));
    block.finish(QTuiToolBlock::Status::Failure, QStringLiteral("exit 1"));
    block.layout(40);
    QCOMPARE(block.rowCount(), 2); /* header + footer */
}

void Test::foldingCollapsesToHeaderSummary()
{
    QTuiToolBlock block(QStringLiteral("bash"), QStringLiteral("ls"));
    block.appendBody(QStringLiteral("a\nb\nc\n"));
    block.finish(QTuiToolBlock::Status::Success, QString());
    block.layout(40);
    const int unfolded = block.rowCount();
    QVERIFY(unfolded > 1);
    block.setFolded(true);
    block.layout(40);
    QCOMPARE(block.rowCount(), 1);
}

void Test::plainTextLooksLikeShellHistory()
{
    QTuiToolBlock block(QStringLiteral("bash"), QStringLiteral("ls -la"));
    block.appendBody(QStringLiteral("alpha\nbravo\n"));
    const QString out = block.toPlainText();
    QVERIFY(out.contains(QStringLiteral("$ bash ls -la")));
    QVERIFY(out.contains(QStringLiteral("alpha")));
    QVERIFY(out.contains(QStringLiteral("bravo")));
}

void Test::markdownWrapsBodyInFencedBlock()
{
    QTuiToolBlock block(QStringLiteral("read_file"), QStringLiteral("/etc/hostname"));
    block.appendBody(QStringLiteral("hostname-value\n"));
    const QString out = block.toMarkdown();
    QVERIFY(out.contains(QStringLiteral("**read_file**")));
    QVERIFY(out.contains(QStringLiteral("```text")));
    QVERIFY(out.contains(QStringLiteral("hostname-value")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuitoolblock.moc"
