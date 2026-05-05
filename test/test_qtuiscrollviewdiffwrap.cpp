// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"

#include <QString>
#include <QtTest>

namespace {
QString rowOf(const QTuiScreen &screen, int row, int width)
{
    /* Skip the rightmost column: the scrollview now always paints a
     * `│` (or `█`) scrollbar there, which would otherwise make every
     * row look "non-empty" and hide the actual content boundary. */
    QString   out;
    const int contentEnd = qMax(0, width - 1);
    out.reserve(contentEnd);
    for (int col = 0; col < contentEnd; ++col) {
        out.append(screen.at(col, row).character);
    }
    while (out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    return out;
}
} // namespace

class TestQTuiScrollViewDiffWrap : public QObject
{
    Q_OBJECT

private slots:
    void diffAddContinuationGetsSpaceGutter()
    {
        QTuiScrollView view;
        view.appendLine(
            QStringLiteral(
                "+output_signal <= input_a & input_b & enable & ready & valid & "
                "ready_for_next_stage & gate_clk_en;"),
            QTuiScrollView::DiffAdd);

        const int  width = 40;
        QTuiScreen screen(width, 8);
        view.render(screen, 0, 8, width);

        /* Find first non-empty row to make the test resilient to viewport
         * placement (scrollview anchors content to the bottom). */
        int firstRow = 0;
        while (firstRow < 8 && rowOf(screen, firstRow, width).isEmpty()) {
            ++firstRow;
        }
        QVERIFY(firstRow < 8);

        const QString head = rowOf(screen, firstRow, width);
        const QString cont = rowOf(screen, firstRow + 1, width);
        QVERIFY2(head.startsWith(QLatin1Char('+')), qPrintable(head));
        /* The 1-char gutter aligns continuation under the column right
         * after the '+' marker, so the content's leading character is
         * at column 1 on continuation rows just like on the first row. */
        QVERIFY2(cont.startsWith(QLatin1Char(' ')), qPrintable(cont));
        QVERIFY2(!cont.startsWith(QLatin1Char('+')), qPrintable(cont));
        QVERIFY2(!cont.startsWith(QLatin1Char('-')), qPrintable(cont));
    }

    void diffDelContinuationGetsSpaceGutter()
    {
        QTuiScrollView view;
        view.appendLine(
            QStringLiteral(
                "-output_signal <= input_a & input_b & enable & ready & valid & "
                "ready_for_next_stage;"),
            QTuiScrollView::DiffDel);

        const int  width = 40;
        QTuiScreen screen(width, 8);
        view.render(screen, 0, 8, width);

        int firstRow = 0;
        while (firstRow < 8 && rowOf(screen, firstRow, width).isEmpty()) {
            ++firstRow;
        }
        QVERIFY(firstRow < 8);

        const QString head = rowOf(screen, firstRow, width);
        const QString cont = rowOf(screen, firstRow + 1, width);
        QVERIFY2(head.startsWith(QLatin1Char('-')), qPrintable(head));
        QVERIFY2(cont.startsWith(QLatin1Char(' ')), qPrintable(cont));
    }

    void normalLineWrapHasNoGutter()
    {
        QTuiScrollView view;
        view.appendLine(
            QStringLiteral(
                "plain text that is long enough to wrap onto multiple visual rows on a narrow "
                "pane"),
            QTuiScrollView::Normal);

        const int  width = 40;
        QTuiScreen screen(width, 8);
        view.render(screen, 0, 8, width);

        int firstRow = 0;
        while (firstRow < 8 && rowOf(screen, firstRow, width).isEmpty()) {
            ++firstRow;
        }
        QVERIFY(firstRow < 8);

        const QString cont = rowOf(screen, firstRow + 1, width);
        /* No diff style means no continuation prefix: continuation row
         * starts with a real content character, not a forced space. */
        QVERIFY2(!cont.isEmpty(), qPrintable(cont));
        QVERIFY2(!cont.startsWith(QLatin1Char(' ')), qPrintable(cont));
    }

    void continuationGutterShrinksAvailableWidth()
    {
        QTuiScrollView view;
        /* 39 visible content chars (after the '+'); on a 40-col pane
         * this fits row 1 exactly. The point of the test is the next
         * line: with a 1-char continuation gutter, capacity drops to 39
         * so a 40-char chunk must spill again. */
        view.appendLine(
            QStringLiteral("+abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"),
            QTuiScrollView::DiffAdd);

        const int  width = 40;
        QTuiScreen screen(width, 8);
        view.render(screen, 0, 8, width);

        int firstRow = 0;
        while (firstRow < 8 && rowOf(screen, firstRow, width).isEmpty()) {
            ++firstRow;
        }

        const QString head = rowOf(screen, firstRow, width);
        const QString cont = rowOf(screen, firstRow + 1, width);
        /* Sanity: head must be at most width cells. */
        QVERIFY(head.size() <= width);
        QVERIFY(cont.size() <= width);
        QVERIFY2(cont.startsWith(QLatin1Char(' ')), qPrintable(cont));
    }
};

QSOC_TEST_MAIN(TestQTuiScrollViewDiffWrap)
#include "test_qtuiscrollviewdiffwrap.moc"
