// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiansi.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void plainTextSingleSpan()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("hello"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("hello"));
        QCOMPARE(spans[0].fg, QTuiFgColor::Default);
        QVERIFY(!spans[0].bold);
    }

    void classicColors()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[31mred\033[0mplain"));
        QCOMPARE(spans.size(), 2);
        QCOMPARE(static_cast<int>(spans[0].fg), 1);
        QCOMPARE(spans[0].text, QStringLiteral("red"));
        QCOMPARE(spans[1].fg, QTuiFgColor::Default);
    }

    void blackAvoidsDefaultSentinel()
    {
        /* Palette index 0 doubles as "Default" in the cell model, so
         * SGR 30 must land on the cube black instead. */
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[30mx"));
        QCOMPARE(static_cast<int>(spans[0].fg), 16);
    }

    void brightAndBold()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[1;92mok"));
        QCOMPARE(spans.size(), 1);
        QVERIFY(spans[0].bold);
        QCOMPARE(static_cast<int>(spans[0].fg), 10);
    }

    void palette256()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[38;5;208morange\033[48;5;236mbg"));
        QCOMPARE(spans.size(), 2);
        QCOMPARE(static_cast<int>(spans[0].fg), 208);
        QCOMPARE(static_cast<int>(spans[1].bg), 236);
    }

    void trueColorQuantized()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[38;2;255;0;0mred"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(static_cast<int>(spans[0].fg), 196);
    }

    void rgbConversion()
    {
        QCOMPARE(QTuiAnsi::rgbTo256(0, 0, 0), 16);
        QCOMPARE(QTuiAnsi::rgbTo256(255, 255, 255), 231);
        QCOMPARE(QTuiAnsi::rgbTo256(128, 128, 128), 244);
        QCOMPARE(QTuiAnsi::rgbTo256(255, 0, 0), 196);
        QCOMPARE(QTuiAnsi::rgbTo256(0, 255, 0), 46);
        QCOMPARE(QTuiAnsi::rgbTo256(-5, 300, 10), QTuiAnsi::rgbTo256(0, 255, 10));
    }

    void inverseAndDim()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[2;7mx\033[27my"));
        QCOMPARE(spans.size(), 2);
        QVERIFY(spans[0].dim);
        QVERIFY(spans[0].inverted);
        QVERIFY(spans[1].dim);
        QVERIFY(!spans[1].inverted);
    }

    void oscStripped()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033]0;title\007text"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("text"));
    }

    void oscStTerminated()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033]8;;http://x\033\\link"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("link"));
    }

    void nonSgrCsiDropped()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("a\033[2Kb\033[10;20Hc"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("abc"));
    }

    void carriageReturnDropped()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("a\rb\ac"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("abc"));
    }

    void truncatedEscapeAtEnd()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("ok\033["));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].text, QStringLiteral("ok"));
    }

    void malformedExtendedColor()
    {
        /* 38 without a complete mode payload must not crash or leak
         * garbage into the text. */
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[38;5mx\033[38;2;1;2my"));
        QString    all;
        for (const auto &span : spans) {
            all += span.text;
        }
        QCOMPARE(all, QStringLiteral("xy"));
    }

    void colonSeparatedParams()
    {
        const auto spans = QTuiAnsi::parse(QStringLiteral("\033[38:5:208mx"));
        QCOMPARE(spans.size(), 1);
        QCOMPARE(static_cast<int>(spans[0].fg), 208);
    }

    void emptyInput() { QVERIFY(QTuiAnsi::parse(QString()).isEmpty()); }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qtuiansi.moc"
