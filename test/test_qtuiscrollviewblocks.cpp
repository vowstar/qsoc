// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

namespace {

QString screenText(const QTuiScreen &screen, int width, int height)
{
    QString text;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width - 1; ++col) {
            text += screen.at(col, row).character;
        }
        text += QLatin1Char('\n');
    }
    return text;
}

QString renderText(QTuiScrollView &view, int width, int height)
{
    QTuiScreen screen(width, height);
    view.render(screen, 0, height, width);
    return screenText(screen, width, height);
}

QString numberedLines(int first, int count)
{
    QStringList lines;
    lines.reserve(count);
    for (int index = first; index < first + count; ++index) {
        lines.append(QStringLiteral("row-%1").arg(index, 3, 10, QLatin1Char('0')));
    }
    return lines.join(QLatin1Char('\n'));
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void appendBlockExtendsHistory();
    void appendBlockCoexistsWithLegacyAppendLine();
    void toPlainTextWalksAllBlockTypes();
    void clearDropsAllBlocksAndPartial();
    void renderShowsBlockContentBottomAligned();
    void shortHistoryDoesNotScrollOutOfView();
    void singleLongBlockScrollsToTop();
    void appendKeepsScrolledViewportAnchored();
    void streamGrowthKeepsScrolledViewportAnchored();
    void visibleFoldExpansionKeepsBlockAnchored();
    void resizeAndContentShrinkClampOffset();
};

void Test::appendBlockExtendsHistory()
{
    QTuiScrollView view;
    QCOMPARE(view.totalLines(), 0);
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("hello")));
    QCOMPARE(view.totalLines(), 1);
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("world")));
    QCOMPARE(view.totalLines(), 2);
}

void Test::appendBlockCoexistsWithLegacyAppendLine()
{
    QTuiScrollView view;
    view.appendLine(QStringLiteral("legacy"), QTuiScrollView::Bold);
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("**md**")));
    view.appendStyledLine({{.text = QStringLiteral("styled"), .italic = true}});

    QTuiScreen screen(40, 6);
    view.render(screen, 0, 6, 40);

    /* Bottom-aligned: 3 source rows render in the last 3 screen rows. */
    QCOMPARE(screen.at(0, 3).character, QChar(QLatin1Char('l')));
    QVERIFY(screen.at(0, 3).bold);

    QCOMPARE(screen.at(0, 4).character, QChar(QLatin1Char('m')));
    QVERIFY(screen.at(0, 4).bold);

    QCOMPARE(screen.at(0, 5).character, QChar(QLatin1Char('s')));
    QVERIFY(screen.at(0, 5).italic);
}

void Test::toPlainTextWalksAllBlockTypes()
{
    QTuiScrollView view;
    view.appendLine(QStringLiteral("plain row"));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("# Heading")));
    view.appendStyledLine({{.text = QStringLiteral("styled bit")}});

    const QString out = view.toPlainText();
    QVERIFY(out.contains(QStringLiteral("plain row")));
    QVERIFY(out.contains(QStringLiteral("# Heading")));
    QVERIFY(out.contains(QStringLiteral("styled bit")));
}

void Test::clearDropsAllBlocksAndPartial()
{
    QTuiScrollView view;
    view.appendLine(QStringLiteral("a"));
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("b")));
    view.appendPartial(QStringLiteral("incomplete"));
    QVERIFY(view.totalLines() > 0);
    QVERIFY(!view.toPlainText().isEmpty());
    view.clear();
    QCOMPARE(view.totalLines(), 0);
    QCOMPARE(view.toPlainText(), QString());
}

void Test::renderShowsBlockContentBottomAligned()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(QStringLiteral("**bold**")));

    QTuiScreen screen(20, 5);
    view.render(screen, 0, 5, 20);

    /* Single-line block paints to the last row of the viewport. */
    QCOMPARE(screen.at(0, 4).character, QChar(QLatin1Char('b')));
    QVERIFY(screen.at(0, 4).bold);
}

void Test::shortHistoryDoesNotScrollOutOfView()
{
    QTuiScrollView view;
    view.appendLine(QStringLiteral("first"));
    view.appendLine(QStringLiteral("second"));

    renderText(view, 30, 8);
    view.scrollUp(100);

    const QString text = renderText(view, 30, 8);
    QVERIFY(text.contains(QStringLiteral("first")));
    QVERIFY(text.contains(QStringLiteral("second")));
    QVERIFY(view.isAtBottom());
}

void Test::singleLongBlockScrollsToTop()
{
    QTuiScrollView view;
    view.appendBlock(std::make_unique<QTuiAssistantTextBlock>(numberedLines(0, 80)));

    QVERIFY(renderText(view, 32, 5).contains(QStringLiteral("row-079")));

    view.scrollUp(1000);
    const QString text = renderText(view, 32, 5);
    QVERIFY(text.contains(QStringLiteral("row-000")));
    QVERIFY(!text.contains(QStringLiteral("row-079")));
    QVERIFY(!view.isAtBottom());
}

void Test::appendKeepsScrolledViewportAnchored()
{
    QTuiScrollView view;
    for (int index = 0; index < 20; ++index) {
        view.appendLine(QStringLiteral("row-%1").arg(index, 3, 10, QLatin1Char('0')));
    }

    renderText(view, 32, 5);
    view.scrollUp(10);
    const QString anchor = renderText(view, 32, 5);
    QVERIFY(anchor.contains(QStringLiteral("row-005")));
    QVERIFY(anchor.contains(QStringLiteral("row-009")));

    for (int index = 20; index < 25; ++index) {
        view.appendLine(QStringLiteral("row-%1").arg(index, 3, 10, QLatin1Char('0')));
    }
    QCOMPARE(renderText(view, 32, 5), anchor);
}

void Test::streamGrowthKeepsScrolledViewportAnchored()
{
    QTuiScrollView view;
    auto           block = std::make_unique<QTuiAssistantTextBlock>(numberedLines(0, 20));
    auto          *live  = block.get();
    view.appendBlock(std::move(block));

    renderText(view, 32, 5);
    view.scrollUp(10);
    const QString anchor = renderText(view, 32, 5);
    QVERIFY(anchor.contains(QStringLiteral("row-005")));

    live->appendMarkdown(QLatin1Char('\n') + numberedLines(20, 5));
    QCOMPARE(renderText(view, 32, 5), anchor);
}

void Test::visibleFoldExpansionKeepsBlockAnchored()
{
    QTuiScrollView view;
    for (int index = 0; index < 10; ++index) {
        view.appendLine(QStringLiteral("lead-%1").arg(index));
    }
    auto block = std::make_unique<QTuiAssistantTextBlock>(numberedLines(100, 8));
    block->setFolded(true);
    view.appendBlock(std::move(block));
    for (int index = 0; index < 10; ++index) {
        view.appendLine(QStringLiteral("tail-%1").arg(index));
    }

    renderText(view, 32, 8);
    view.scrollUp(5);
    const QStringList before   = renderText(view, 32, 8).split(QLatin1Char('\n'));
    int               blockRow = -1;
    for (int row = 0; row < before.size(); ++row) {
        if (before[row].contains(QStringLiteral("(folded)"))) {
            blockRow = row;
            break;
        }
    }
    QVERIFY(blockRow >= 0);

    view.setFocusedBlockIdx(10);
    view.toggleFocusedFold();
    const QStringList after = renderText(view, 32, 8).split(QLatin1Char('\n'));
    QVERIFY(after[blockRow].contains(QStringLiteral("row-100")));
    QVERIFY(after[blockRow + 1].contains(QStringLiteral("row-104")));
}

void Test::resizeAndContentShrinkClampOffset()
{
    QString text = QStringLiteral("TOP ");
    for (int index = 0; index < 40; ++index) {
        text += QStringLiteral("middle ");
    }
    text += QStringLiteral("BOTTOM");

    QTuiScrollView view;
    auto           block = std::make_unique<QTuiAssistantTextBlock>(text);
    auto          *live  = block.get();
    view.appendBlock(std::move(block));

    renderText(view, 16, 4);
    view.scrollUp(1000);
    QVERIFY(renderText(view, 16, 4).contains(QStringLiteral("TOP")));

    const QString wideText = renderText(view, 120, 8);
    QVERIFY(wideText.contains(QStringLiteral("TOP")));
    QVERIFY(wideText.contains(QStringLiteral("BOTTOM")));
    QVERIFY(view.isAtBottom());

    renderText(view, 16, 4);
    view.scrollUp(1000);
    renderText(view, 16, 4);
    QVERIFY(!view.isAtBottom());

    live->setMarkdown(QStringLiteral("short"));
    QVERIFY(renderText(view, 16, 4).contains(QStringLiteral("short")));
    QVERIFY(view.isAtBottom());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewblocks.moc"
