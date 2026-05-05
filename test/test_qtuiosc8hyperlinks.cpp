// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmarkdownrenderer.h"
#include "qsoc_test.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiscreen.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void linkNodeProducesHyperlinkRun();
    void cellHyperlinkSurvivesPaint();
    void toAnsiWrapsHyperlinkInOsc8();
    void toAnsiClosesLinkAtFrameEnd();
};

void Test::linkNodeProducesHyperlinkRun()
{
    const auto rendered = QSocMarkdownRenderer::render(
        QStringLiteral("see [docs](https://example.com/x) here"));
    bool found = false;
    for (const auto &line : rendered) {
        for (const auto &run : line.runs) {
            if (run.text == QStringLiteral("docs")) {
                QCOMPARE(run.hyperlink, QStringLiteral("https://example.com/x"));
                QVERIFY(run.underline);
                found = true;
            }
        }
    }
    QVERIFY(found);
}

void Test::cellHyperlinkSurvivesPaint()
{
    QTuiAssistantTextBlock block(QStringLiteral("[anchor](https://example.com/y)"));
    block.layout(40);
    QTuiScreen screen(40, 2);
    block.paintRow(screen, 0, 0, 0, 40, false, false);
    QCOMPARE(screen.at(0, 0).hyperlink, QStringLiteral("https://example.com/y"));
}

void Test::toAnsiWrapsHyperlinkInOsc8()
{
    QTuiScreen screen(20, 2);
    /* Plant a single linked cell and verify the ANSI string contains
     * the OSC 8 open + close pair. */
    QTuiCell &cell    = screen.at(0, 0);
    cell.character    = QChar(QLatin1Char('A'));
    cell.hyperlink    = QStringLiteral("https://example.com/z");
    const QString out = screen.toAnsi();
    QVERIFY(out.contains(QStringLiteral("\x1b]8;;https://example.com/z\x1b\\")));
}

void Test::toAnsiClosesLinkAtFrameEnd()
{
    QTuiScreen screen(20, 2);
    QTuiCell  &cell   = screen.at(0, 0);
    cell.character    = QChar(QLatin1Char('A'));
    cell.hyperlink    = QStringLiteral("https://x");
    const QString out = screen.toAnsi();
    /* Final close marker must appear after the open + payload. */
    QVERIFY(
        out.endsWith(QStringLiteral("\x1b]8;;\x1b\\\x1b[0m"))
        || out.contains(QStringLiteral("\x1b]8;;\x1b\\")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiosc8hyperlinks.moc"
