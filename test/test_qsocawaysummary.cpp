// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocawaysummary.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testPromptsCarryTranscript()
    {
        QVERIFY(!QSocAwaySummary::systemPrompt().isEmpty());
        const QString user = QSocAwaySummary::buildUserMessage(
            QStringLiteral("[user]: wire the APB bus"));
        QVERIFY(user.contains(QStringLiteral("wire the APB bus")));
    }

    void testSanitizeCollapsesWhitespace()
    {
        QCOMPARE(
            QSocAwaySummary::sanitize(
                QStringLiteral("You are debugging the\n  reset logic.\tNext: add a sync.")),
            QStringLiteral("You are debugging the reset logic. Next: add a sync."));
    }

    void testSanitizeStripsSurroundingQuotes()
    {
        QCOMPARE(
            QSocAwaySummary::sanitize(QStringLiteral("\"Wiring the bus. Next: run tests.\"")),
            QStringLiteral("Wiring the bus. Next: run tests."));
        QCOMPARE(
            QSocAwaySummary::sanitize(QStringLiteral("'One sentence recap.'")),
            QStringLiteral("One sentence recap."));
    }

    void testSanitizeKeepsInnerPunctuationAndQuotes()
    {
        /* Only one layer of matched surrounding quotes is stripped; inner
         * quotes and sentence punctuation stay intact. */
        QCOMPARE(
            QSocAwaySummary::sanitize(QStringLiteral("Editing \"foo.v\". Next: build.")),
            QStringLiteral("Editing \"foo.v\". Next: build."));
    }

    void testSanitizeEmptyStaysEmpty()
    {
        QVERIFY(QSocAwaySummary::sanitize(QString()).isEmpty());
        QVERIFY(QSocAwaySummary::sanitize(QStringLiteral("   \n\t ")).isEmpty());
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocawaysummary.moc"
