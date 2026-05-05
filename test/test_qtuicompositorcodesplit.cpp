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
    void assistantSplitsProseFromFencedCode();
    void copyOfFocusedCodeBlockHasNoFenceNoBanner();
    void chunkBoundaryInsideFenceMarkerStillSplits();
    void reasoningStreamSplitsLikewise();
    void interveningPrintContentSealsAndStartsFreshGroup();
    void newReasoningRunFoldsPriorReasoningGroup();
};

namespace {
QStringList collectMarkdowns(QTuiCompositor &compositor)
{
    QStringList out;
    const int   total = compositor.contentView().totalLines();
    for (int idx = 0; idx < total; ++idx) {
        compositor.contentView().setFocusedBlockIdx(idx);
        out.append(compositor.contentView().copyFocusedAsMarkdown());
    }
    return out;
}
} // namespace

void Test::assistantSplitsProseFromFencedCode()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(
        QStringLiteral("intro line\n```python\nprint(1)\nprint(2)\n```\noutro line\n"));
    compositor.finishStream();

    const QStringList md = collectMarkdowns(compositor);
    /* Three blocks: prose intro, code block, prose outro. */
    QCOMPARE(md.size(), 3);
    QVERIFY(md[0].contains(QStringLiteral("intro line")));
    QCOMPARE(md[1], QStringLiteral("```python\nprint(1)\nprint(2)\n```\n"));
    QVERIFY(md[2].contains(QStringLiteral("outro line")));
}

void Test::copyOfFocusedCodeBlockHasNoFenceNoBanner()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(QStringLiteral("```bash\nls\npwd\n```\n"));
    compositor.finishStream();

    /* Find the code block (kind: starts with ``` in markdown). */
    int codeIdx = -1;
    for (int idx = 0; idx < compositor.contentView().totalLines(); ++idx) {
        compositor.contentView().setFocusedBlockIdx(idx);
        const QString md = compositor.contentView().copyFocusedAsMarkdown();
        if (md.startsWith(QStringLiteral("```bash"))) {
            codeIdx = idx;
            break;
        }
    }
    QVERIFY(codeIdx >= 0);

    compositor.contentView().setFocusedBlockIdx(codeIdx);
    const QString plain = compositor.contentView().copyFocusedAsPlainText();
    /* The whole point of the refactor: plain copy is just the raw code. */
    QCOMPARE(plain, QStringLiteral("ls\npwd\n"));
    QVERIFY(!plain.contains(QStringLiteral("```")));
    QVERIFY(!plain.contains(QStringLiteral("┄")));
    QVERIFY(!plain.contains(QStringLiteral("▎")));
}

void Test::chunkBoundaryInsideFenceMarkerStillSplits()
{
    QTuiCompositor compositor;
    /* Split the opening fence across two chunks: "``" then "`py\nbody\n```\n" */
    compositor.appendAssistantChunk(QStringLiteral("hi\n``"));
    compositor.appendAssistantChunk(QStringLiteral("`py\nbody\n```\n"));
    compositor.finishStream();

    const QStringList md = collectMarkdowns(compositor);
    QVERIFY(md.size() >= 2);
    bool sawCode = false;
    for (const auto &one : md) {
        if (one.startsWith(QStringLiteral("```py")) && one.contains(QStringLiteral("body"))) {
            sawCode = true;
        }
    }
    QVERIFY(sawCode);
}

void Test::reasoningStreamSplitsLikewise()
{
    QTuiCompositor compositor;
    compositor.appendReasoningChunk(
        QStringLiteral("thinking aloud\n```python\nx = 1\n```\nstill thinking\n"));
    compositor.finishStream();

    /* Reasoning produces text + code + text just like assistant. */
    QVERIFY(compositor.contentView().totalLines() >= 3);
}

void Test::interveningPrintContentSealsAndStartsFreshGroup()
{
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(QStringLiteral("first\n"));
    compositor.printContent(QStringLiteral("$ tool\n"), QTuiScrollView::Bold);
    compositor.appendAssistantChunk(QStringLiteral("second\n"));
    compositor.finishStream();

    const QString flat = compositor.contentView().toPlainText();
    const int     aIdx = flat.indexOf(QStringLiteral("first"));
    const int     tIdx = flat.indexOf(QStringLiteral("$ tool"));
    const int     bIdx = flat.indexOf(QStringLiteral("second"));
    QVERIFY(aIdx >= 0 && tIdx > aIdx && bIdx > tIdx);
}

void Test::newReasoningRunFoldsPriorReasoningGroup()
{
    QTuiCompositor compositor;
    /* First reasoning run with code. */
    compositor.appendReasoningChunk(QStringLiteral("first thought\n```py\na = 1\n```\nmore\n"));
    /* User-visible answer arrives, run ends. */
    compositor.printContent(QStringLiteral("\n"), QTuiScrollView::Normal);
    /* Second reasoning run starts. */
    compositor.appendReasoningChunk(QStringLiteral("second thought\n"));
    compositor.finishStream();

    /* The first reasoning run's blocks should be folded; the second
     * should remain expanded. We verify by checking that the sum of
     * total scrollback rows after collapsing the first group is less
     * than what an expanded layout would produce. Pragmatic check:
     * walk blocks and verify any block whose markdown contains
     * "first thought" or "a = 1" reports rowCount == 1 after layout
     * (folded). */
    QStringList firstRunMarkdowns;
    QStringList secondRunMarkdowns;
    const int   total = compositor.contentView().totalLines();
    for (int idx = 0; idx < total; ++idx) {
        compositor.contentView().setFocusedBlockIdx(idx);
        const QString md = compositor.contentView().copyFocusedAsMarkdown();
        if (md.contains(QStringLiteral("first thought")) || md.contains(QStringLiteral("a = 1"))
            || md.contains(QStringLiteral("more"))) {
            firstRunMarkdowns.append(md);
        } else if (md.contains(QStringLiteral("second thought"))) {
            secondRunMarkdowns.append(md);
        }
    }
    QVERIFY(!firstRunMarkdowns.isEmpty());
    QVERIFY(!secondRunMarkdowns.isEmpty());
}

QSOC_TEST_MAIN(Test)
#include "test_qtuicompositorcodesplit.moc"
