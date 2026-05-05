// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void singleStyledRunPaintsAttributesPerCell();
    void mixedStyledRunsCarryDistinctAttributes();
    void styledRunWrapsAcrossRowsKeepingAttributes();
    void replaceLastStyledLineSwapsContent();
    void plainAndStyledLinesCoexist();
    void cjkCharactersConsumeTwoCells();
};

namespace {
QTuiCell cellAt(QTuiScreen &screen, int col, int row)
{
    return screen.at(col, row);
}
} // namespace

void Test::singleStyledRunPaintsAttributesPerCell()
{
    QTuiScrollView view;
    view.appendStyledLine(
        {{.text      = QStringLiteral("hi"),
          .bold      = true,
          .italic    = true,
          .dim       = false,
          .underline = false,
          .fg        = QTuiFgColor::Yellow,
          .bg        = BG_DEFAULT}});

    QTuiScreen screen(20, 4);
    view.render(screen, 0, 4, 20);

    const QTuiCell zero = cellAt(screen, 0, 3);
    QCOMPARE(zero.character, QChar(QLatin1Char('h')));
    QVERIFY(zero.bold);
    QVERIFY(zero.italic);
    QCOMPARE(zero.fgColor, QTuiFgColor::Yellow);

    const QTuiCell one = cellAt(screen, 1, 3);
    QCOMPARE(one.character, QChar(QLatin1Char('i')));
    QVERIFY(one.bold);
    QVERIFY(one.italic);
    QCOMPARE(one.fgColor, QTuiFgColor::Yellow);
}

void Test::mixedStyledRunsCarryDistinctAttributes()
{
    QTuiScrollView       view;
    QList<QTuiStyledRun> runs;
    runs.append({.text = QStringLiteral("A"), .bold = true});
    runs.append({.text = QStringLiteral("B"), .italic = true, .fg = QTuiFgColor::Magenta});
    runs.append({.text = QStringLiteral("C"), .dim = true, .underline = true});
    view.appendStyledLine(runs);

    QTuiScreen screen(10, 2);
    view.render(screen, 0, 2, 10);

    const int row = 1;
    QCOMPARE(cellAt(screen, 0, row).character, QChar(QLatin1Char('A')));
    QVERIFY(cellAt(screen, 0, row).bold);
    QVERIFY(!cellAt(screen, 0, row).italic);

    QCOMPARE(cellAt(screen, 1, row).character, QChar(QLatin1Char('B')));
    QVERIFY(!cellAt(screen, 1, row).bold);
    QVERIFY(cellAt(screen, 1, row).italic);
    QCOMPARE(cellAt(screen, 1, row).fgColor, QTuiFgColor::Magenta);

    QCOMPARE(cellAt(screen, 2, row).character, QChar(QLatin1Char('C')));
    QVERIFY(cellAt(screen, 2, row).dim);
    QVERIFY(cellAt(screen, 2, row).underline);
}

void Test::styledRunWrapsAcrossRowsKeepingAttributes()
{
    QTuiScrollView view;
    view.appendStyledLine(
        {{.text = QStringLiteral("alpha bravo charlie"), .bold = true, .fg = QTuiFgColor::Cyan}});

    /* Width 11 with -1 scrollbar => contentWidth 10. The 19-char string
     * wraps at spaces into "alpha" / "bravo" / "charlie", filling all
     * three rows top-down because totalVisible == height. */
    QTuiScreen screen(11, 3);
    view.render(screen, 0, 3, 11);

    QCOMPARE(cellAt(screen, 0, 0).character, QChar(QLatin1Char('a')));
    QVERIFY(cellAt(screen, 0, 0).bold);
    QCOMPARE(cellAt(screen, 0, 0).fgColor, QTuiFgColor::Cyan);

    QCOMPARE(cellAt(screen, 0, 1).character, QChar(QLatin1Char('b')));
    QVERIFY(cellAt(screen, 0, 1).bold);

    QCOMPARE(cellAt(screen, 0, 2).character, QChar(QLatin1Char('c')));
    QVERIFY(cellAt(screen, 0, 2).bold);
    QCOMPARE(cellAt(screen, 0, 2).fgColor, QTuiFgColor::Cyan);
}

void Test::replaceLastStyledLineSwapsContent()
{
    QTuiScrollView view;
    view.appendStyledLine({{.text = QStringLiteral("draft")}});
    view.replaceLastStyledLine({{.text = QStringLiteral("final"), .bold = true}});

    QTuiScreen screen(10, 2);
    view.render(screen, 0, 2, 10);

    const int row = 1;
    QCOMPARE(cellAt(screen, 0, row).character, QChar(QLatin1Char('f')));
    QVERIFY(cellAt(screen, 0, row).bold);
    QCOMPARE(cellAt(screen, 4, row).character, QChar(QLatin1Char('l')));
}

void Test::plainAndStyledLinesCoexist()
{
    QTuiScrollView view;
    view.appendLine(QStringLiteral("plain"), QTuiScrollView::Bold);
    view.appendStyledLine({{.text = QStringLiteral("styled"), .italic = true}});

    QTuiScreen screen(20, 4);
    view.render(screen, 0, 4, 20);

    const int plainRow  = 2;
    const int styledRow = 3;

    QCOMPARE(cellAt(screen, 0, plainRow).character, QChar(QLatin1Char('p')));
    QVERIFY(cellAt(screen, 0, plainRow).bold);
    QVERIFY(!cellAt(screen, 0, plainRow).italic);

    QCOMPARE(cellAt(screen, 0, styledRow).character, QChar(QLatin1Char('s')));
    QVERIFY(!cellAt(screen, 0, styledRow).bold);
    QVERIFY(cellAt(screen, 0, styledRow).italic);
}

void Test::cjkCharactersConsumeTwoCells()
{
    QTuiScrollView view;
    view.appendStyledLine({{.text = QStringLiteral("中"), .bold = true}});

    QTuiScreen screen(10, 2);
    view.render(screen, 0, 2, 10);

    const int row = 1;
    QCOMPARE(cellAt(screen, 0, row).character, QChar(0x4E2D));
    QVERIFY(cellAt(screen, 0, row).bold);
    /* The second visual cell stays untouched (no character written). */
    QCOMPARE(cellAt(screen, 1, row).character, QChar(QLatin1Char(' ')));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiscrollviewstyledruns.moc"
