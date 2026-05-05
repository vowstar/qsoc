// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiblock.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

#include <memory>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void appendBlockExtendsHistory();
    void appendBlockCoexistsWithLegacyAppendLine();
    void toPlainTextWalksAllBlockTypes();
    void clearDropsAllBlocksAndPartial();
    void renderShowsBlockContentBottomAligned();
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

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewblocks.moc"
