// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccodehighlighter.h"
#include "qsoc_test.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void emptyLineProducesEmpty();
    void unknownLanguageReturnsSingleDefaultRun();
    void pythonKeywordsAndStringsAndCommentColored();
    void bashHashCommentSwallowsRest();
    void cppKeywordsAndPreprocAndComment();
    void jsonKeywordsAndStrings();
    void yamlKeyHeuristic();
    void verilogSizedLiteralAsNumber();
    void diffMarkersPerLine();
    void concatenatedRunsRoundTripText();
};

namespace {
QString textOf(const QList<QTuiStyledRun> &runs)
{
    QString out;
    for (const auto &run : runs) {
        out += run.text;
    }
    return out;
}

bool hasRunWithColor(const QList<QTuiStyledRun> &runs, const QString &needle, QTuiFgColor color)
{
    for (const auto &run : runs) {
        if (run.text.contains(needle) && run.fg == color) {
            return true;
        }
    }
    return false;
}
} // namespace

void Test::emptyLineProducesEmpty()
{
    QCOMPARE(QSocCodeHighlighter::highlight(QString(), QStringLiteral("python")).size(), 0);
}

void Test::unknownLanguageReturnsSingleDefaultRun()
{
    const auto runs
        = QSocCodeHighlighter::highlight(QStringLiteral("xyz 123"), QStringLiteral("foo"));
    QCOMPARE(textOf(runs), QStringLiteral("xyz 123"));
}

void Test::pythonKeywordsAndStringsAndCommentColored()
{
    const auto runs = QSocCodeHighlighter::highlight(
        QStringLiteral("def hello():  # greet"), QStringLiteral("python"));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("def"), QTuiFgColor::Magenta));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("# greet"), QTuiFgColor::Gray));
    QCOMPARE(textOf(runs), QStringLiteral("def hello():  # greet"));
}

void Test::bashHashCommentSwallowsRest()
{
    const auto runs
        = QSocCodeHighlighter::highlight(QStringLiteral("ls -la # files"), QStringLiteral("bash"));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("# files"), QTuiFgColor::Gray));
}

void Test::cppKeywordsAndPreprocAndComment()
{
    const auto preproc
        = QSocCodeHighlighter::highlight(QStringLiteral("#include <vector>"), QStringLiteral("cpp"));
    QVERIFY(hasRunWithColor(preproc, QStringLiteral("#include"), QTuiFgColor::Orange));

    const auto code
        = QSocCodeHighlighter::highlight(QStringLiteral("int x = 42; // ok"), QStringLiteral("cpp"));
    QVERIFY(hasRunWithColor(code, QStringLiteral("int"), QTuiFgColor::Magenta));
    QVERIFY(hasRunWithColor(code, QStringLiteral("42"), QTuiFgColor::Yellow));
    QVERIFY(hasRunWithColor(code, QStringLiteral("// ok"), QTuiFgColor::Gray));
}

void Test::jsonKeywordsAndStrings()
{
    const auto runs
        = QSocCodeHighlighter::highlight(QStringLiteral("\"flag\": true"), QStringLiteral("json"));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("\"flag\""), QTuiFgColor::Green));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("true"), QTuiFgColor::Magenta));
}

void Test::yamlKeyHeuristic()
{
    const auto runs
        = QSocCodeHighlighter::highlight(QStringLiteral("name: hello"), QStringLiteral("yaml"));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("name"), QTuiFgColor::Magenta));
}

void Test::verilogSizedLiteralAsNumber()
{
    const auto runs = QSocCodeHighlighter::highlight(
        QStringLiteral("assign x = 8'hFF;"), QStringLiteral("systemverilog"));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("assign"), QTuiFgColor::Magenta));
    QVERIFY(hasRunWithColor(runs, QStringLiteral("8'hFF"), QTuiFgColor::Yellow));
}

void Test::diffMarkersPerLine()
{
    const auto add
        = QSocCodeHighlighter::highlight(QStringLiteral("+added line"), QStringLiteral("diff"));
    QVERIFY(hasRunWithColor(add, QStringLiteral("+added"), QTuiFgColor::Green));

    const auto hunk
        = QSocCodeHighlighter::highlight(QStringLiteral("@@ -1,3 +1,4 @@"), QStringLiteral("diff"));
    QVERIFY(hasRunWithColor(hunk, QStringLiteral("@@"), QTuiFgColor::Magenta));
}

void Test::concatenatedRunsRoundTripText()
{
    /* For every supported language the runs must concatenate back to
     * the original input — no character is lost or duplicated by the
     * tokenizer. */
    const QStringList samples = {
        QStringLiteral("def f(): return 1"),
        QStringLiteral("ls | grep \"foo\""),
        QStringLiteral("int main() { return 0; }"),
        QStringLiteral("{\"a\": 1, \"b\": null}"),
        QStringLiteral("- name: thing"),
        QStringLiteral("module top(input clk);"),
    };
    const QStringList langs
        = {QStringLiteral("python"),
           QStringLiteral("bash"),
           QStringLiteral("cpp"),
           QStringLiteral("json"),
           QStringLiteral("yaml"),
           QStringLiteral("verilog")};
    for (int idx = 0; idx < samples.size(); ++idx) {
        const auto runs = QSocCodeHighlighter::highlight(samples[idx], langs[idx]);
        QCOMPARE(textOf(runs), samples[idx]);
    }
}

QSOC_TEST_MAIN(Test)
#include "test_qsoccodehighlighter.moc"
