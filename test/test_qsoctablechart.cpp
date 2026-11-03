// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsvg.h"
#include "qsoc_test.h"
#include "tui/qsoctablechart.h"

#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void chartsNumericTable();
    void groupsComparableColumns();
    void orderedAxisDrawsLines_data();
    void orderedAxisDrawsLines();
    void unorderedNumbersDrawPoints();
    void sharesOfAWholeDrawAPie();
    void otherPercentagesStayBars_data();
    void otherPercentagesStayBars();
    void negativeValuesCrossTheBaseline();
    void leavesOtherTablesAlone_data();
    void leavesOtherTablesAlone();
    void escapesLabels();
};

void Test::chartsNumericTable()
{
    const auto charts = QSocTableChart::charts(
        "Intro\n\n| Block | Power |\n|---|---|\n| cpu | 12 mW |\n| noc | 3.5 mW |\n| ddr | 1,200 "
        "mW |\n");
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("cpu"));
    QVERIFY(charts[0].contains("1,200 mW"));
    QVERIFY(charts[0].contains("var(--c1)"));
    QVERIFY(!QSocSvg::sanitize(charts[0], false).isEmpty());
}

void Test::groupsComparableColumns()
{
    const auto charts = QSocTableChart::charts(
        "| Run | Old | New | Huge |\n|:--|--:|--:|--:|\n| a | 10 | 12 | 900000 |\n| b | 20 | 15 | "
        "800000 |\n"
        "| c | 30 | 25 | 700000 |\n");
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("var(--c2)"));
    QVERIFY(charts[0].contains("New"));
    QVERIFY(!charts[0].contains("Huge"));
}

void Test::orderedAxisDrawsLines_data()
{
    QTest::addColumn<QString>("markdown");
    QTest::newRow("numeric axis") << "| n | t |\n|---|---|\n| 1 | 4 |\n| 2 | 5 |\n| 3 | 9 |\n";
    QTest::newRow("named axis")
        << "| Version | t |\n|---|---|\n| v1 | 4 |\n| v2 | 5 |\n| v3 | 9 |\n";
    QTest::newRow("negative") << "| step | d |\n|---|---|\n| a | -4 |\n| b | 5 |\n| c | 2 |\n";
}

void Test::orderedAxisDrawsLines()
{
    QFETCH(QString, markdown);
    const auto charts = QSocTableChart::charts(markdown);
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("<polyline"));
    QVERIFY(!QSocSvg::sanitize(charts[0], false).isEmpty());
}

void Test::unorderedNumbersDrawPoints()
{
    const auto charts = QSocTableChart::charts(
        "| size | time |\n|---|---|\n| 3 | 4 |\n| 1 | 5 |\n| 2 | 9 |\n");
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("<circle"));
    QVERIFY(!charts[0].contains("<polyline"));
    QVERIFY(!QSocSvg::sanitize(charts[0], false).isEmpty());
}

void Test::sharesOfAWholeDrawAPie()
{
    const auto charts = QSocTableChart::charts(
        "| Block | Area |\n|---|---|\n| CPU | 60% |\n| <b>GPU</b> | 30% |\n| IO | 10% |\n");
    QCOMPARE(charts.size(), 1);
    QCOMPARE(charts[0].count("<path"), 3);
    QVERIFY(charts[0].contains("&lt;b&gt;GPU"));
    QVERIFY(charts[0].contains("60%"));
    QVERIFY(!QSocSvg::sanitize(charts[0], false).isEmpty());
}

void Test::otherPercentagesStayBars_data()
{
    QTest::addColumn<QString>("markdown");
    QTest::newRow("not a whole") << "| a | b |\n|---|---|\n| x | 60% |\n| y | 70% |\n| z | 9% |\n";
    QTest::newRow("no unit") << "| a | b |\n|---|---|\n| x | 60 |\n| y | 30 |\n| z | 10 |\n";
    QTest::newRow("two columns")
        << "| a | b | c |\n|---|---|---|\n| x | 60% | 1% |\n| y | 30% | 9% |\n| z | 10% | 90% |\n";
    QTest::newRow("more than the palette")
        << "| a | b |\n|---|---|\n| p | 15% |\n| q | 15% |\n| r | 15% |\n| s | 15% |\n"
           "| t | 15% |\n| u | 15% |\n| v | 10% |\n";
}

void Test::otherPercentagesStayBars()
{
    QFETCH(QString, markdown);
    const auto charts = QSocTableChart::charts(markdown);
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("<rect"));
    QVERIFY(!charts[0].contains("<path"));
}

void Test::negativeValuesCrossTheBaseline()
{
    const auto charts = QSocTableChart::charts(
        "| Block | Delta |\n|---|---|\n| a | -10 |\n| b | 30 |\n| c | 5 |\n");
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("<rect x=\"150\""));
    QVERIFY(charts[0].contains("-10"));
}

void Test::leavesOtherTablesAlone_data()
{
    QTest::addColumn<QString>("markdown");
    QTest::newRow("too few rows") << "| a | b |\n|---|---|\n| x | 1 |\n| y | 2 |\n";
    QTest::newRow("no numbers") << "| a | b |\n|---|---|\n| x | m |\n| y | n |\n| z | o |\n";
    QTest::newRow("all equal") << "| a | b |\n|---|---|\n| x | 1 |\n| y | 1 |\n| z | 1 |\n";
    QTest::newRow("identifier names") << "| id | b |\n|---|---|\n| 1 | 4 |\n| 2 | 5 |\n| 3 | 6 |\n";
    QTest::newRow("mixed units")
        << "| a | b |\n|---|---|\n| x | 1 ms |\n| y | 2 s |\n| z | 3 ms |\n";
    QTest::newRow("identifier values")
        << "| a | id |\n|---|---|\n| x | 1 |\n| y | 2 |\n| z | 3 |\n";
    QTest::newRow("no table") << "plain | text\nmore | text\n";
}

void Test::leavesOtherTablesAlone()
{
    QFETCH(QString, markdown);
    QVERIFY(QSocTableChart::charts(markdown).isEmpty());
}

void Test::escapesLabels()
{
    const auto charts = QSocTableChart::charts(
        "| a | b |\n|---|---|\n| <x&y> | 1 |\n| y | 2 |\n| z | 3 |\n");
    QCOMPARE(charts.size(), 1);
    QVERIFY(charts[0].contains("&lt;x&amp;y&gt;"));
    QVERIFY(!QSocSvg::sanitize(charts[0], false).isEmpty());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctablechart.moc"
