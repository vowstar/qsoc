// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiinputline.h"
#include "tui/qtuiscreen.h"

#include <QString>
#include <QtTest>

namespace {
QString rowOf(const QTuiScreen &screen, int row, int width)
{
    QString out;
    out.reserve(width);
    for (int col = 0; col < width; ++col) {
        out.append(screen.at(col, row).character);
    }
    return out;
}
} // namespace

class TestQTuiInputLineWrap : public QObject
{
    Q_OBJECT

private slots:
    void asciiWrapsAtNarrowWidth()
    {
        QTuiInputLine line;
        const QString text = QStringLiteral(
            "0123456789012345678901234567890123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
            "uvwxyz");
        line.setText(text);
        line.setCursorPos(text.size());
        line.setTerminalWidth(40);

        QCOMPARE(line.lineCount(), 3);

        QTuiScreen screen(40, 5);
        line.render(screen, 0, 40);

        QCOMPARE(rowOf(screen, 0, 40), QStringLiteral("> 01234567890123456789012345678901234567"));
        QCOMPARE(rowOf(screen, 1, 40), QStringLiteral("89ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijkl"));
        QCOMPARE(rowOf(screen, 2, 40).trimmed(), QStringLiteral("mnopqrstuvwxyz"));
    }

    void cursorAtEndOfWrappedBufferLandsOnLastVisualRow()
    {
        QTuiInputLine line;
        const QString text = QStringLiteral(
            "0123456789012345678901234567890123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
            "uvwxyz");
        line.setText(text);
        line.setCursorPos(text.size());
        line.setTerminalWidth(40);

        QCOMPARE(line.cursorLine(), 2);
        QCOMPARE(line.cursorColumn(), 14);
    }

    void cursorAtBufferStartLandsAfterPrompt()
    {
        QTuiInputLine line;
        line.setText(QStringLiteral("hello world this should fit comfortably"));
        line.setCursorPos(0);
        line.setTerminalWidth(40);

        QCOMPARE(line.cursorLine(), 0);
        QCOMPARE(line.cursorColumn(), 2);
    }

    void multipleLogicalLinesGetContinuationPrompt()
    {
        QTuiInputLine line;
        const QString text = QStringLiteral("first\nsecond\nthird");
        line.setText(text);
        line.setCursorPos(text.size());
        line.setTerminalWidth(20);

        QCOMPARE(line.lineCount(), 3);

        QTuiScreen screen(20, 5);
        line.render(screen, 0, 20);

        QCOMPARE(rowOf(screen, 0, 20).trimmed(), QStringLiteral("> first"));
        QCOMPARE(rowOf(screen, 1, 20).trimmed(), QStringLiteral(". second"));
        QCOMPARE(rowOf(screen, 2, 20).trimmed(), QStringLiteral(". third"));
        QCOMPARE(line.cursorLine(), 2);
        QCOMPARE(line.cursorColumn(), 7);
    }

    void wideCharsRespectCellBoundaries()
    {
        QTuiInputLine line;
        /* Each CJK glyph is two cells; with width=10 the prompt "> " takes
         * two cells leaving 8 cells = 4 wide chars on row 0, then 5 wide
         * chars per continuation row at the full width. Ten glyphs → 3
         * visual rows: 4 + 5 + 1. */
        const QString text = QStringLiteral("你好世界中国人民万岁");
        line.setText(text);
        line.setCursorPos(text.size());
        line.setTerminalWidth(10);

        QCOMPARE(line.lineCount(), 3);

        QTuiScreen screen(10, 4);
        line.render(screen, 0, 10);

        /* QTuiScreen stores one QChar per cell, so a wide glyph occupies a
         * single cell in the buffer even though it visually spans two
         * columns; trim trailing spaces before comparing to the source. */
        QCOMPARE(rowOf(screen, 0, 10).trimmed(), QStringLiteral("> 你好世界"));
        QCOMPARE(rowOf(screen, 1, 10).trimmed(), QStringLiteral("中国人民万"));
        QCOMPARE(rowOf(screen, 2, 10).trimmed(), QStringLiteral("岁"));
    }

    void bangPromptReplacesPrimary()
    {
        QTuiInputLine line;
        line.setText(QStringLiteral("!ls -la"));
        line.setCursorPos(7);
        line.setTerminalWidth(20);

        QCOMPARE(line.lineCount(), 1);

        QTuiScreen screen(20, 2);
        line.render(screen, 0, 20);

        QCOMPARE(rowOf(screen, 0, 20).trimmed(), QStringLiteral("! ls -la"));
        QCOMPARE(line.cursorColumn(), 8);
    }

    void emptyBufferShowsPlaceholder()
    {
        QTuiInputLine line;
        line.setPlaceholder(QStringLiteral("type something"));
        line.setTerminalWidth(40);

        QCOMPARE(line.lineCount(), 1);

        QTuiScreen screen(40, 2);
        line.render(screen, 0, 40);

        QCOMPARE(rowOf(screen, 0, 40).trimmed(), QStringLiteral("> type something"));
    }

    void scrollKeepsLastVisibleLines()
    {
        QTuiInputLine line;
        QString       text;
        for (int i = 0; i < QTuiInputLine::MAX_VISIBLE_LINES + 3; ++i) {
            if (i > 0) {
                text.append(QLatin1Char('\n'));
            }
            text.append(QStringLiteral("line%1").arg(i));
        }
        line.setText(text);
        line.setCursorPos(text.size());
        line.setTerminalWidth(20);

        QCOMPARE(line.lineCount(), QTuiInputLine::MAX_VISIBLE_LINES);

        QTuiScreen screen(20, QTuiInputLine::MAX_VISIBLE_LINES);
        line.render(screen, 0, 20);

        const int last = QTuiInputLine::MAX_VISIBLE_LINES - 1;
        QCOMPARE(
            rowOf(screen, last, 20).trimmed(),
            QStringLiteral(". line%1").arg(QTuiInputLine::MAX_VISIBLE_LINES + 2));
    }
};

QSOC_TEST_MAIN(TestQTuiInputLineWrap)
#include "test_qtuiinputlinewrap.moc"
