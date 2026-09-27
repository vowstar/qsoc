// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmarkdownrenderer.h"
#include "common/qsocmath.h"
#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiwidget.h"

#include <QStringDecoder>
#include <QtTest>

namespace {
using Kind = QSocMarkdownRenderer::Kind;

QStringList rendered(const QString &source, int width = 80)
{
    QStringList result;
    for (const auto &line : QSocMarkdownRenderer::render(source, width)) {
        QString value;
        for (const auto &run : line.runs)
            value += run.text;
        result.append(value);
    }
    return result;
}

QString matrix(const QString &name, int rows, int columns, const QString &value = "x")
{
    QStringList content;
    for (int row = 0; row < rows; ++row) {
        QStringList cells;
        for (int col = 0; col < columns; ++col)
            cells.append(value);
        content.append(cells.join('&'));
    }
    return "\\begin{" + name + "}" + content.join("\\\\") + "\\end{" + name + "}";
}

QStringList markdowns(QTuiCompositor &compositor)
{
    QStringList result;
    for (int index = 0; index < compositor.contentView().totalLines(); ++index) {
        compositor.contentView().setFocusedBlockIdx(index);
        result.append(compositor.contentView().copyFocusedAsMarkdown());
    }
    return result;
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void scalarLayouts();
    void matrixLayouts();
    void matrixEnvironments();
    void invalidSyntax();
    void dimensions();
    void budgets();
    void sourceProtection();
    void containersAndWidth();
    void copyAndResize();
    void streamingCuts();
    void streamSealing();
};

void Test::scalarLayouts()
{
    QCOMPARE(rendered(R"(Value $x_2+\alpha^3$.)"), QStringList{"Value x₂+α³."});
    QCOMPARE(rendered(R"($\frac{1}{2}+\sqrt{x}$)"), QStringList{"(1)/(2)+√(x)"});
    QCOMPARE(rendered(R"($$\frac{1}{2}$$)"), (QStringList{" 1 ", "───", " 2 "}));
    QCOMPARE(rendered(R"($$\sqrt{x}$$)"), (QStringList{" ─", "√x"}));
    QCOMPARE(rendered(R"($$\frac{12}{3}=x$$)"), (QStringList{" 12   ", "────=x", " 3    "}));
    QVERIFY(!QSocMath::render("x_i", false));
    QVERIFY(!QSocMath::render(QString::fromUcs4(U"𝛼"), false));
    QVERIFY(!QSocMath::render(QStringLiteral("a\u0301"), false));
}

void Test::matrixLayouts()
{
    QCOMPARE(
        rendered(R"($$\begin{bmatrix}1&2\\3&4\end{bmatrix}$$)"),
        (QStringList{"⎡ 1  2 ⎤", "⎢      ⎥", "⎣ 3  4 ⎦"}));
    QCOMPARE(rendered(R"($$\begin{pmatrix}1&20&3\end{pmatrix}$$)"), QStringList{"(1  20  3)"});
    QCOMPARE(
        rendered(R"($$\begin{bmatrix}\frac{1}{2}&x\\y&z\end{bmatrix}$$)"),
        (QStringList{"⎡  1     ⎤", "⎢ ───  x ⎥", "⎢  2     ⎥", "⎢        ⎥", "⎣  y   z ⎦"}));
    QCOMPARE(rendered(R"($$A=\begin{matrix}1\\2\end{matrix}$$)"), (QStringList{"  1", "A= ", "  2"}));
    QCOMPARE(
        rendered(R"($$\begin{matrix}&a\\b&\end{matrix}$$)"), (QStringList{"   a", "    ", "b   "}));
    QCOMPARE(
        rendered(R"($$\begin{matrix}\& &2\\3&4\\\end{matrix}$$)"),
        (QStringList{"&  2", "    ", "3  4"}));
    const auto even = QSocMath::render(R"(\begin{matrix}\sqrt{x}\end{matrix})", true);
    QVERIFY(even);
    QCOMPARE(even->rows.size(), 2);
    QCOMPARE(even->baseline, 0);
}

void Test::matrixEnvironments()
{
    for (const auto &name : {"matrix", "pmatrix", "bmatrix", "vmatrix", "Vmatrix"}) {
        for (const auto &[rows, columns] : {std::pair{1, 1}, {1, 3}, {3, 1}, {2, 2}}) {
            const QString source = matrix(name, rows, columns);
            const auto    layout = QSocMath::render(source, true);
            QVERIFY2(layout, qPrintable(source));
            QCOMPARE(layout->rows.size(), 2 * rows - 1);
            for (const auto &line : layout->rows)
                QCOMPARE(QTuiText::visualWidth(line), layout->width);
            QVERIFY(!QSocMath::render(source, false));
        }
    }
    QCOMPARE(
        rendered("$$" + matrix("Vmatrix", 2, 1) + "$$"),
        (QStringList{"|| x ||", "||   ||", "|| x ||"}));
}

void Test::invalidSyntax()
{
    const QStringList inputs{
        R"(\begin{matrix}1&2\\3\end{matrix})",
        R"(\begin{matrix}1\end{pmatrix})",
        R"(\begin{matrix}1)",
        R"(\begin{matrix}\end{matrix})",
        R"(\begin{matrix}1\\[2pt]2\end{matrix})",
        R"(\begin{matrix}1% comment\end{matrix})",
        R"(\begin{matrix}{1&2}\end{matrix})",
        R"(\begin{matrix}1&\unknown\end{matrix})",
        R"(\begin{matrix}1\end{matrix}^2)",
        R"({\begin{matrix}1\end{matrix}}^2)",
        R"(\begin{matrix}\begin{matrix}1\end{matrix}\end{matrix})",
        R"(\begin{array}{cc}1&2\end{array})",
        R"(\begin{matrix*}1\end{matrix*})",
        R"(\beginning{matrix}1\end{matrix})",
        R"(\frac{1}{2)",
        R"(x^^2)",
        R"(x_{})",
        R"(\newcommand{a}{b})"};
    for (const auto &body : inputs) {
        QVERIFY2(!QSocMath::render(body, true), qPrintable(body));
        const QString source = "$$" + body + "$$";
        QCOMPARE(rendered(source), QStringList{source});
    }
    QCOMPARE(rendered(R"($$x_1$$$)"), QStringList{R"($$x_1$$$)"});
}

void Test::dimensions()
{
    for (int size : {7, 8, 9}) {
        QCOMPARE(bool(QSocMath::render(matrix("matrix", size, 1), true)), size <= 8);
        QCOMPARE(bool(QSocMath::render(matrix("matrix", 1, size), true)), size <= 8);
        QCOMPARE(bool(QSocMath::render(matrix("matrix", 1, size, ""), true)), size <= 8);
    }
    QVERIFY(QSocMath::render(matrix("matrix", 8, 8), true));
    QVERIFY(QSocMath::render(matrix("matrix", 8, 7) + matrix("matrix", 1, 7), true));
    QVERIFY(QSocMath::render(matrix("matrix", 8, 7) + matrix("matrix", 1, 8), true));
    QVERIFY(!QSocMath::render(matrix("matrix", 8, 8) + matrix("matrix", 1, 1), true));
    QVERIFY(!QSocMath::render(matrix("matrix", 8, 1, R"(\frac{1}{2})"), true));
    QVERIFY(QSocMath::render(R"(\begin{matrix}&\\&\end{matrix})", true));
    QVERIFY(!QSocMath::render(R"(\begin{matrix}&\\\\&\end{matrix})", true));
}

void Test::budgets()
{
    for (int bytes : {4095, 4096, 4097}) {
        const QString source = "x" + QString(bytes - 1, QLatin1Char(' '));
        int           work   = -1;
        QCOMPARE(bool(QSocMath::render(source, true, 256, &work)), bytes <= 4096);
        if (bytes > 4096)
            QCOMPARE(work, 0);
    }
    for (int bytes : {4095, 4096, 4097}) {
        const QString source = QStringLiteral("α") + QString(bytes - 2, QLatin1Char(' '));
        QCOMPARE(bool(QSocMath::render(source, true)), bytes <= 4096);
    }
    for (int depth : {31, 32, 33}) {
        const QString source = QString(depth, '{') + "x" + QString(depth, '}');
        QCOMPARE(bool(QSocMath::render(source, true)), depth <= 32);
    }
    for (int width : {255, 256, 257})
        QCOMPARE(bool(QSocMath::render(QString(width, 'x'), true)), width <= 256);
    QString root = "x";
    for (int height = 2; height <= 17; ++height) {
        root             = "\\sqrt{" + root + "}";
        const auto value = QSocMath::render(root, true);
        QCOMPARE(bool(value), height <= 16);
        if (value)
            QCOMPARE(value->rows.size(), height);
    }
    int work = -1;
    QVERIFY(!QSocMath::render(QString(100000, 'x'), true, 256, &work));
    QCOMPARE(work, 0);
    const QString overflow = "$$" + QString(5000, 'x') + "$$ then $y_2$";
    QVERIFY(rendered(overflow).join('\n').endsWith("then y₂"));
}

void Test::sourceProtection()
{
    QCOMPARE(
        rendered(R"(Cost $5 and $10. Shell $HOME ${x} $(date) $?.)"),
        QStringList{R"(Cost $5 and $10. Shell $HOME ${x} $(date) $?.)"});
    QCOMPARE(rendered(R"(Escaped \$x_2\$ and $x_2$.)"), QStringList{R"(Escaped $x_2$ and x₂.)"});
    QCOMPARE(rendered(R"(π `code $x_2$` then $x_2$)"), QStringList{"π code $x_2$ then x₂"});
    QCOMPARE(
        rendered(QString::fromUcs4(U"😀 `code $x_2$` then $x_2$")),
        QStringList{QString::fromUcs4(U"😀 code $x_2$ then x₂")});
    const auto currency = QSocMarkdownRenderer::render("$5 **per unit** and $HOME **path**");
    int        boldRuns = 0;
    for (const auto &line : currency)
        for (const auto &run : line.runs)
            boldRuns += run.bold ? 1 : 0;
    QCOMPARE(boldRuns, 2);
    const QStringList protectedSources{
        "```text\n$x_2$\n```",
        "~~~text\n$x_2$\n~~~",
        "    $x_2$\n",
        R"([label $x_2$](https://example.com/$x_2$))",
        R"(https://example.com/$x_2$)",
        R"(<span title="$x_2$">text</span>)"};
    for (const auto &source : protectedSources) {
        const QString output = rendered(source).join('\n');
        QVERIFY2(!output.contains("x₂"), qPrintable(source));
    }
    const QString unsupported = "$$\\unknown_a\n```text\n**b**\n$$";
    QCOMPARE(rendered(unsupported).join('\n'), unsupported);
    QCOMPARE(rendered("unmatched ` then $x_2$"), QStringList{"unmatched ` then x₂"});
    const QString bare = R"(\begin{matrix}a&b\end{matrix})";
    QVERIFY(!rendered(bare).join('\n').contains("⎡"));
}

void Test::containersAndWidth()
{
    const QString formula = R"($$\frac{1}{2}$$)";
    QCOMPARE(rendered("> " + formula, 5), (QStringList{"│  1 ", "│ ───", "│  2 "}));
    QCOMPARE(rendered("- " + formula, 5), (QStringList{"-  1 ", "  ───", "   2 "}));
    QCOMPARE(rendered("10. " + formula, 7), (QStringList{"10.  1 ", "    ───", "     2 "}));
    const QString comparison  = "before > $$\n>x\n$$";
    const auto    comparisons = QSocMath::spans(comparison, {});
    QCOMPARE(comparisons.size(), 1);
    QCOMPARE(QSocMath::body(comparison, comparisons.first()), QString("\n>x\n"));

    QCOMPARE(rendered("> $$\n> \\frac{1}{2}\n> $$", 5), rendered("> " + formula, 5));
    QCOMPARE(rendered("> $$\r\n> \\frac{1}{2}\r\n> $$", 5), rendered("> " + formula, 5));
    for (const QString &prefix : {QString("> "), QString("- ")}) {
        const auto lines = QSocMarkdownRenderer::render(prefix + formula, 4);
        for (const auto &line : lines)
            QVERIFY(line.kind != Kind::Math);
        QVERIFY(rendered(prefix + formula, 4).join('\n').contains(formula));
    }
    const QString wide = "$$" + matrix("bmatrix", 2, 2) + "$$";
    for (int width : {1, 7, 8, 20, 80, 160}) {
        const auto lines = QSocMarkdownRenderer::render(wide, width);
        QCOMPARE(lines.first().kind == Kind::Math, width >= 8);
        if (width >= 8) {
            for (const auto &line : rendered(wide, width))
                QVERIFY(QTuiText::visualWidth(line) <= width);
        }
    }
    const auto table = rendered("| a |\n|---|\n| " + formula + " |", 80).join('\n');
    QVERIFY(table.contains(formula));
    QVERIFY(!table.contains("───  "));
}

void Test::copyAndResize()
{
    const QString          source = "$$" + matrix("bmatrix", 2, 2) + "$$";
    QTuiAssistantTextBlock block(source);
    block.layout(80);
    QCOMPARE(block.rowCount(), 3);
    QCOMPARE(block.toMarkdown(), source);
    const QString copied = block.selectedLogicalText(0, 0, 2, 100);
    QCOMPARE(copied, QStringLiteral("⎡ x  x ⎤\n⎢      ⎥\n⎣ x  x ⎦"));
    block.layout(1);
    QVERIFY(block.rowCount() > 3);
    block.layout(80);
    QCOMPARE(block.rowCount(), 3);
    QCOMPARE(block.selectedLogicalText(0, 0, 2, 100), copied);
    QTuiAssistantTextBlock text("hello world");
    text.layout(80);
    QCOMPARE(text.selectedLogicalText(0, 0, 0, 4), QString("hello"));
}

void Test::streamingCuts()
{
    const QStringList sources{
        "π $x_2$\n$$\\begin{bmatrix}1&2\\\\3&4\\end{bmatrix}$$\n",
        "$$\\unknown\n```bad\n$$\n```text\n$x_2$\n```\n",
        "$$\\frac{1}{2}\n```unclosed\n",
        "before `code $x_2$`\n$$\\sqrt{x}$$\n",
        "unmatched ` tick\n$$\\unknown\n```inside\n$$\n",
        "multiline ` code\n$$ literal dollars`\n```text\nx\n```\n"};
    for (const auto &source : sources) {
        QTuiCompositor reference;
        reference.appendAssistantChunk(source);
        reference.finishStream();
        const auto       expected = markdowns(reference);
        const QByteArray utf8     = source.toUtf8();
        for (int cut = 0; cut <= utf8.size(); ++cut) {
            if (cut < utf8.size() && (static_cast<unsigned char>(utf8[cut]) & 0xc0) == 0x80)
                continue;
            QTuiCompositor compositor;
            compositor.appendAssistantChunk(QString::fromUtf8(utf8.first(cut)));
            compositor.appendAssistantChunk(QString::fromUtf8(utf8.sliced(cut)));
            compositor.finishStream();
            QCOMPARE(markdowns(compositor), expected);
        }
        QTuiCompositor bytes;
        QStringDecoder decoder(QStringDecoder::Utf8);
        for (int index = 0; index < utf8.size(); ++index)
            bytes.appendAssistantChunk(decoder(utf8.sliced(index, 1)));
        bytes.finishStream();
        QCOMPARE(markdowns(bytes), expected);
    }
}

void Test::streamSealing()
{
    const QString  unclosed = "$$\\begin{matrix}1&2\n```inside\n";
    QTuiCompositor compositor;
    compositor.appendAssistantChunk(unclosed);
    compositor.beginToolUse("read_file", "input");
    compositor.finishToolUse(QTuiToolBlock::Status::Success, "done");
    compositor.appendAssistantChunk("```text\n$x_2$\n```\n");
    compositor.finishStream();
    const auto parts = markdowns(compositor);
    QCOMPARE(parts.first(), unclosed);
    QVERIFY(parts.last().startsWith("```text"));
    QTuiCompositor cancel;
    cancel.appendAssistantChunk(unclosed);
    cancel.finishStream();
    cancel.appendAssistantChunk("$x_2$\n");
    cancel.finishStream();
    QCOMPARE(markdowns(cancel), (QStringList{unclosed, "$x_2$\n"}));
    QTuiCompositor reasoning;
    reasoning.appendReasoningChunk(unclosed);
    reasoning.finishStream();
    QCOMPARE(markdowns(reasoning), QStringList{unclosed});
}
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmath.moc"
