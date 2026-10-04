// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuicodeblock.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuitextlayout.h"
#include "tui/qtuiwidget.h"

#include <QtTest>

namespace {
class Test : public QObject
{
    Q_OBJECT
private slots:
    void screenRoundTrip()
    {
        const QString text = QString::fromUcs4(U"A🚀𝔸中Z");
        QCOMPARE(QTuiText::visualWidth(text), 7);
        QTuiScreen screen(7, 1);
        screen.putString(0, 0, text);
        QCOMPARE(screen.at(1, 0).codePoint(), char32_t(0x1f680));
        QCOMPARE(screen.at(3, 0).text(), QString::fromUcs4(U"𝔸"));
        QCOMPARE(screen.at(6, 0).character, QChar('Z'));
        QVERIFY(screen.toAnsi().contains(text));
        screen.putString(1, 0, "xy");
        QCOMPARE(screen.at(1, 0).text(), QString("x"));
        QVERIFY(!screen.toAnsi().contains(QString::fromUcs4(U"🚀")));
        screen.putString(6, 0, QString::fromUcs4(U"🚀"));
        QCOMPARE(screen.at(6, 0).character, QChar('Z'));
        screen.hline(0, '-');
        QVERIFY(!screen.toAnsi().contains(QString::fromUcs4(U"𝔸")));
    }
    void overwriteWideCells()
    {
        for (const QString &wide : {QString::fromUcs4(U"🚀"), QStringLiteral("中")}) {
            QTuiScreen screen(4, 1);
            screen.putString(0, 0, wide);
            screen.putChar(1, 0, 'x');
            QCOMPARE(screen.at(0, 0).text(), QString(" "));
            QCOMPARE(screen.at(1, 0).text(), QString("x"));
            QVERIFY(!screen.toAnsi().contains(wide));
            screen.putString(0, 0, "abcd");
            screen.putString(0, 0, wide);
            screen.putChar(0, 0, 'y');
            QCOMPARE(screen.at(1, 0).text(), QString(" "));
            QVERIFY(screen.toAnsi().contains("y cd"));
            screen.putString(1, 0, wide);
            screen.putString(0, 0, wide);
            QCOMPARE(screen.at(2, 0).text(), QString(" "));
        }
    }
    void blockRoundTrip()
    {
        const QString          source = QString::fromUcs4(U"a🚀𝔸中文z");
        QTuiAssistantTextBlock assistant(source);
        QTuiCodeBlock          code("text", source, false, -1);
        for (QTuiBlock *block :
             {static_cast<QTuiBlock *>(&assistant), static_cast<QTuiBlock *>(&code)}) {
            for (int width : {4, 8, 40}) {
                block->layout(width);
                QCOMPARE(block->toPlainText(), source);
                const QString ansi = block->toAnsi(width);
                QVERIFY(ansi.contains(QString::fromUcs4(U"🚀")));
                QVERIFY(ansi.contains(QString::fromUcs4(U"𝔸")));
                QVERIFY(!ansi.contains(QChar::ReplacementCharacter));
            }
        }
    }
    void selectionAndWrapping()
    {
        QTuiStyledRun run;
        run.text        = QString::fromUcs4(U"A🚀𝔸Z");
        const auto rows = qtuiWrapStyledRuns({run}, 0, 3);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].runs.first().text, QString::fromUcs4(U"A🚀"));
        QCOMPARE(rows[1].startColInLogical, 3);
        QCOMPARE(qtuiSelectedLogicalText(rows, {run.text}, 0, 2, 0, 2), QString::fromUcs4(U"🚀"));
        QCOMPARE(qtuiSelectedLogicalText(rows, {run.text}, 1, 0, 1, 0), QString::fromUcs4(U"𝔸"));
        QCOMPARE(qtuiSelectedLogicalText(rows, {run.text}, 0, 0, 1, 1), run.text);
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qtuiscalar.moc"
