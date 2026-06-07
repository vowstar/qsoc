// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsessiontitle.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testPromptsCarryInput()
    {
        QVERIFY(!QSocSessionTitle::systemPrompt().isEmpty());
        const QString user = QSocSessionTitle::buildUserMessage(QStringLiteral("fix the APB bus"));
        QVERIFY(user.contains(QStringLiteral("fix the APB bus")));
    }

    void testSanitizeStripsQuotesAndPunctuation()
    {
        QCOMPARE(
            QSocSessionTitle::sanitize(QStringLiteral("\"Refactor APB Bridge\"")),
            QStringLiteral("Refactor APB Bridge"));
        QCOMPARE(
            QSocSessionTitle::sanitize(QStringLiteral("Add Reset Logic.")),
            QStringLiteral("Add Reset Logic"));
        QCOMPARE(
            QSocSessionTitle::sanitize(QStringLiteral("  Wire   Up   Clocks  ")),
            QStringLiteral("Wire Up Clocks"));
    }

    void testSanitizeCapsWordCount()
    {
        const QString capped = QSocSessionTitle::sanitize(
            QStringLiteral("one two three four five six seven eight nine"));
        QCOMPARE(capped.split(QLatin1Char(' ')).size(), 7);
        QCOMPARE(capped, QStringLiteral("one two three four five six seven"));
    }

    void testSanitizeEmptyStaysEmpty()
    {
        QVERIFY(QSocSessionTitle::sanitize(QString()).isEmpty());
        QVERIFY(QSocSessionTitle::sanitize(QStringLiteral("   ")).isEmpty());
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocsessiontitle.moc"
