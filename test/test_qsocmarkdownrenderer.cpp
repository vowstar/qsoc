// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmarkdownrenderer.h"
#include "qsoc_test.h"
#include "tui/qtuiwidget.h"

#include <QString>
#include <QtTest>

using Kind = QSocMarkdownRenderer::Kind;

namespace {

QString concat(const QSocMarkdownRenderer::RenderedLine &line)
{
    QString out;
    for (const auto &run : line.runs) {
        out += run.text;
    }
    return out;
}

bool anyRunIsBold(const QSocMarkdownRenderer::RenderedLine &line)
{
    for (const auto &run : line.runs) {
        if (run.bold) {
            return true;
        }
    }
    return false;
}

bool anyRunIsItalic(const QSocMarkdownRenderer::RenderedLine &line)
{
    for (const auto &run : line.runs) {
        if (run.italic) {
            return true;
        }
    }
    return false;
}

class TestQSocMarkdownRenderer : public QObject
{
    Q_OBJECT

private slots:
    void emptyInputProducesNoLines()
    {
        const auto out = QSocMarkdownRenderer::render(QString());
        QCOMPARE(out.size(), 0);
    }

    /* h1 must be bold AND italic per the renderer's own contract; h2-h6
     * are bold only. Heading kind + level are populated for callers
     * that want to color or prefix differently. */
    void heading1IsBoldItalic()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("# Hello"));
        QVERIFY(out.size() >= 1);
        QCOMPARE(out[0].kind, Kind::Heading);
        QCOMPARE(out[0].headingLevel, 1);
        QCOMPARE(concat(out[0]), QStringLiteral("Hello"));
        QVERIFY(anyRunIsBold(out[0]));
        QVERIFY(anyRunIsItalic(out[0]));
    }

    void heading3IsBoldOnly()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("### Title"));
        QVERIFY(out.size() >= 1);
        QCOMPARE(out[0].kind, Kind::Heading);
        QCOMPARE(out[0].headingLevel, 3);
        QVERIFY(anyRunIsBold(out[0]));
        QVERIFY(!anyRunIsItalic(out[0]));
    }

    /* Inline emphasis runs land on Plain lines with the corresponding
     * styled-run flags only on the emphasized substring, not the whole
     * line. */
    void boldInsideParagraphSplitsRuns()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("This is **bold** prose"));
        QVERIFY(out.size() >= 1);
        const auto &line = out[0];
        QCOMPARE(line.kind, Kind::Plain);
        QCOMPARE(concat(line), QStringLiteral("This is bold prose"));
        bool sawBoldRun  = false;
        bool sawPlainRun = false;
        for (const auto &run : line.runs) {
            if (run.text == QStringLiteral("bold")) {
                QVERIFY(run.bold);
                sawBoldRun = true;
            }
            if (run.text == QStringLiteral("This is ")) {
                QVERIFY(!run.bold);
                sawPlainRun = true;
            }
        }
        QVERIFY(sawBoldRun);
        QVERIFY(sawPlainRun);
    }

    void italicAndInlineCodeStyling()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("Run *now* and `make build`."));
        QVERIFY(out.size() >= 1);
        const auto &line      = out[0];
        bool        sawItalic = false;
        bool        sawCode   = false;
        for (const auto &run : line.runs) {
            if (run.text == QStringLiteral("now") && run.italic) {
                sawItalic = true;
            }
            if (run.text == QStringLiteral("make build")) {
                /* Inline code is colored, not bold/italic. */
                QCOMPARE(run.fg, QTuiFgColor::Yellow);
                sawCode = true;
            }
        }
        QVERIFY(sawItalic);
        QVERIFY(sawCode);
    }

    /* Blockquotes: each interior line gets a "│ " dim gutter prefix
     * and inherits italic styling. The kind is BlockQuote so the TUI
     * can layer additional styling on top if needed. */
    void blockquoteHasGutterAndItalic()
    {
        const auto out   = QSocMarkdownRenderer::render(QStringLiteral("> a quote"));
        bool       found = false;
        for (const auto &line : out) {
            if (line.kind == Kind::BlockQuote) {
                QVERIFY(concat(line).contains(QStringLiteral("│")));
                QVERIFY(concat(line).contains(QStringLiteral("a quote")));
                QVERIFY(anyRunIsItalic(line));
                found = true;
            }
        }
        QVERIFY(found);
    }

    /* Bullet list: each item is a ListItem line with the bullet
     * prefixed as a dim run; nested list adds two-space indent. */
    void unorderedListEmitsBullets()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("- alpha\n- beta\n  - gamma"));
        QStringList itemTexts;
        for (const auto &line : out) {
            if (line.kind == Kind::ListItem) {
                itemTexts.append(concat(line));
            }
        }
        QCOMPARE(itemTexts.size(), 3);
        QCOMPARE(itemTexts[0], QStringLiteral("- alpha"));
        QCOMPARE(itemTexts[1], QStringLiteral("- beta"));
        QCOMPARE(itemTexts[2], QStringLiteral("  - gamma"));
    }

    void orderedListNumbersIncrement()
    {
        const auto out = QSocMarkdownRenderer::render(
            QStringLiteral("1. first\n2. second\n3. third"));
        QStringList itemTexts;
        for (const auto &line : out) {
            if (line.kind == Kind::ListItem) {
                itemTexts.append(concat(line));
            }
        }
        QCOMPARE(itemTexts.size(), 3);
        QCOMPARE(itemTexts[0], QStringLiteral("1. first"));
        QCOMPARE(itemTexts[1], QStringLiteral("2. second"));
        QCOMPARE(itemTexts[2], QStringLiteral("3. third"));
    }

    /* Fenced code block: language flows through to codeLanguage,
     * each interior line is a CodeBlock kind. The renderer prefixes
     * a one-line ┄ banner with the language label and a `▎ ` gutter
     * on each body line; the test verifies both shape and body
     * content stripped of the gutter. */
    void codeBlockPreservesContentAndLanguage()
    {
        const auto out = QSocMarkdownRenderer::render(
            QStringLiteral("```cpp\nint main() {\n    return 0;\n}\n```"));
        QStringList codeLines;
        QString     lang;
        for (const auto &line : out) {
            if (line.kind == Kind::CodeBlock) {
                codeLines.append(concat(line));
                lang = line.codeLanguage;
            }
        }
        QCOMPARE(codeLines.size(), 4);
        QVERIFY(codeLines[0].contains(QStringLiteral("cpp")));
        QCOMPARE(codeLines[1], QStringLiteral("▎ int main() {"));
        QCOMPARE(codeLines[2], QStringLiteral("▎     return 0;"));
        QCOMPARE(codeLines[3], QStringLiteral("▎ }"));
        QCOMPARE(lang, QStringLiteral("cpp"));
    }

    /* Hard line break inside a paragraph splits the line; soft break
     * (raw '\n' in source) becomes a single space. */
    void hardBreakSplitsLineSoftBreakIsSpace()
    {
        const auto out = QSocMarkdownRenderer::render(
            QStringLiteral("first line  \nsecond line\nthird line"));
        QStringList plainTexts;
        for (const auto &line : out) {
            if (line.kind == Kind::Plain) {
                plainTexts.append(concat(line));
            }
        }
        QVERIFY(plainTexts.size() >= 2);
        QCOMPARE(plainTexts[0], QStringLiteral("first line"));
        QVERIFY(plainTexts[1].contains(QStringLiteral("second line third line")));
    }

    /* Strikethrough must NOT be enabled: an LLM that emits ` ~100ns ` in
     * timing analysis prose would otherwise turn into struck-through
     * text. The literal ~ characters must survive in the rendered runs. */
    void strikethroughIsNotEnabled()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("delay ~100ns"));
        QVERIFY(out.size() >= 1);
        QVERIFY(concat(out[0]).contains(QStringLiteral("~100ns")));
    }

    /* Top-level blocks separated by blank lines: blank-line markers
     * are emitted between blocks but never adjacent. */
    void blanksBetweenTopLevelBlocks()
    {
        const auto out = QSocMarkdownRenderer::render(QStringLiteral("# H\n\npara one\n\npara two"));
        int blankCount   = 0;
        int lastWasBlank = false;
        for (const auto &line : out) {
            if (line.kind == Kind::BlankLine) {
                blankCount++;
                /* Adjacent blanks must collapse. */
                QVERIFY(!lastWasBlank);
                lastWasBlank = true;
            } else {
                lastWasBlank = false;
            }
        }
        QVERIFY(blankCount >= 1);
    }

    /* Basic GFM table: 2 columns, 1 header + 2 body rows; expect three
     * border lines plus three content lines, all Kind::Table. */
    void simpleTableEmitsBorderAndCells()
    {
        const QString md = QStringLiteral(
            "| Name | Type |\n"
            "|------|------|\n"
            "| clk  | wire |\n"
            "| rst  | reg  |\n");
        const auto out = QSocMarkdownRenderer::render(md, /*width=*/80);

        QStringList tableLines;
        for (const auto &line : out) {
            if (line.kind == Kind::Table) {
                tableLines.append(concat(line));
            }
        }
        QCOMPARE(tableLines.size(), 6);
        QVERIFY2(
            tableLines[0].startsWith(QChar(0x250C)) && tableLines[0].contains(QChar(0x252C))
                && tableLines[0].endsWith(QChar(0x2510)),
            qPrintable(tableLines[0]));
        QVERIFY2(tableLines[1].contains(QStringLiteral("Name")), qPrintable(tableLines[1]));
        QVERIFY2(
            tableLines[2].startsWith(QChar(0x251C)) && tableLines[2].contains(QChar(0x253C))
                && tableLines[2].endsWith(QChar(0x2524)),
            qPrintable(tableLines[2]));
        QVERIFY2(tableLines[3].contains(QStringLiteral("clk")), qPrintable(tableLines[3]));
        QVERIFY2(tableLines[4].contains(QStringLiteral("rst")), qPrintable(tableLines[4]));
        QVERIFY2(
            tableLines[5].startsWith(QChar(0x2514)) && tableLines[5].contains(QChar(0x2534))
                && tableLines[5].endsWith(QChar(0x2518)),
            qPrintable(tableLines[5]));
    }

    /* Header cells inherit bold styling from `**...**`; bodies stay
     * plain. The header-vs-body split surfaces in run flags so the
     * scrollview can color rows differently. */
    void tableHeaderRowKeepsBoldStyling()
    {
        const QString md = QStringLiteral(
            "| **Sig** | Width |\n"
            "|---------|-------|\n"
            "| clk     | 1     |\n");
        const auto out           = QSocMarkdownRenderer::render(md, 80);
        bool       sawHeaderBold = false;
        for (const auto &line : out) {
            if (line.kind != Kind::Table) {
                continue;
            }
            for (const auto &run : line.runs) {
                if (run.text.contains(QStringLiteral("Sig")) && run.bold) {
                    sawHeaderBold = true;
                }
            }
        }
        QVERIFY(sawHeaderBold);
    }

    /* When the table is wider than the terminal, columns shrink and
     * cells word-wrap. The border + cell row count grows because each
     * row may emit several lines. The test pins that:
     *   1) every output line stays within terminalWidth visual cells
     *   2) at least one body row produced multiple lines */
    void tableShrinksAndWordWrapsCellsOnNarrowTerminal()
    {
        const QString md = QStringLiteral(
            "| Description                                  | Note          |\n"
            "|----------------------------------------------|---------------|\n"
            "| short                                        | very long note text here that wraps "
            "|\n");
        const int  terminalWidth = 40;
        const auto out           = QSocMarkdownRenderer::render(md, terminalWidth);

        bool sawMultiLineBody = false;
        int  bodyRowsRendered = 0;
        for (const auto &line : out) {
            if (line.kind == Kind::Table) {
                const QString text = concat(line);
                /* No table line may exceed the terminal width. */
                QVERIFY2(
                    QTuiText::visualWidth(text) <= terminalWidth,
                    qPrintable(
                        QStringLiteral("len=%1: %2").arg(QTuiText::visualWidth(text)).arg(text)));
                if (text.contains(QStringLiteral("note")) || text.contains(QStringLiteral("text"))
                    || text.contains(QStringLiteral("wraps"))) {
                    bodyRowsRendered++;
                }
            }
        }
        sawMultiLineBody = (bodyRowsRendered >= 2);
        QVERIFY(sawMultiLineBody);
    }

    /* CJK glyphs count as 2 cells; the column planner must size columns
     * by visual width, not QChar count, or the alignment falls apart. */
    void tableHandlesCjkCellWidth()
    {
        const QString md = QStringLiteral(
            "| 名称 | 描述         |\n"
            "|------|--------------|\n"
            "| 时钟 | 主时钟信号   |\n"
            "| 复位 | 异步复位     |\n");
        const auto  out = QSocMarkdownRenderer::render(md, 80);
        QStringList tableTexts;
        for (const auto &line : out) {
            if (line.kind == Kind::Table) {
                tableTexts.append(concat(line));
            }
        }
        QVERIFY(tableTexts.size() >= 5);
        /* Every table line should have the same total visual width for
         * borders + bars to stack correctly. */
        const int firstWidth = QTuiText::visualWidth(tableTexts.first());
        for (const QString &line : tableTexts) {
            QCOMPARE(QTuiText::visualWidth(line), firstWidth);
        }
    }

    /* Smoke test on a realistic mixed document: walker must survive
     * heading + bold + list + code + quote interleaving without
     * crashing or emitting bogus lines. */
    void mixedDocumentRoundTripsContent()
    {
        const QString md = QStringLiteral(
            "# Title\n\n"
            "Some **bold** prose with `inline` code.\n\n"
            "- bullet\n"
            "- another with *emph*\n\n"
            "> quoted line\n\n"
            "```python\nprint('hi')\n```\n");
        const auto out = QSocMarkdownRenderer::render(md);
        QVERIFY(out.size() >= 6);

        bool sawHeading = false, sawList = false, sawQuote = false, sawCode = false;
        for (const auto &line : out) {
            switch (line.kind) {
            case Kind::Heading:
                sawHeading = true;
                break;
            case Kind::ListItem:
                sawList = true;
                break;
            case Kind::BlockQuote:
                sawQuote = true;
                break;
            case Kind::CodeBlock:
                sawCode = true;
                break;
            default:
                break;
            }
        }
        QVERIFY(sawHeading);
        QVERIFY(sawList);
        QVERIFY(sawQuote);
        QVERIFY(sawCode);
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocMarkdownRenderer)
#include "test_qsocmarkdownrenderer.moc"
