// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qstringutils.h"

#include <QtTest>

class TestQStringUtils : public QObject
{
    Q_OBJECT

private slots:
    void truncateMiddle_shortString();
    void truncateMiddle_exactLength();
    void truncateMiddle_longString();
    void truncateMiddle_veryShortMaxLen();
    void truncateMiddle_edgeCase();
};

void TestQStringUtils::truncateMiddle_shortString()
{
    /* String shorter than maxLen should remain unchanged */
    QString result = QStringUtils::truncateMiddle("short", 10);
    QCOMPARE(result, QString("short"));
}

void TestQStringUtils::truncateMiddle_exactLength()
{
    /* String exactly at maxLen should remain unchanged */
    QString result = QStringUtils::truncateMiddle("exact_length", 12);
    QCOMPARE(result, QString("exact_length"));
}

void TestQStringUtils::truncateMiddle_longString()
{
    /* Long string should be truncated with ellipsis in the middle */
    /* maxLen=15: ellipsis=3, available=12, left=6, right=6 */
    QString result = QStringUtils::truncateMiddle("very_long_filename.txt", 15);
    QCOMPARE(result.length(), 15);
    QVERIFY(result.contains("..."));
    QVERIFY(result.startsWith("very_l"));
    QVERIFY(result.endsWith("me.txt"));
    QCOMPARE(result, QString("very_l...me.txt"));
}

void TestQStringUtils::truncateMiddle_veryShortMaxLen()
{
    /* maxLen < 4 should truncate from right without ellipsis */
    QString result = QStringUtils::truncateMiddle("longstring", 3);
    QCOMPARE(result, QString("lon"));
    QCOMPARE(result.length(), 3);
}

void TestQStringUtils::truncateMiddle_edgeCase()
{
    /* Test with maxLen = 4 (minimum for ellipsis) */
    QString result = QStringUtils::truncateMiddle("test_string", 4);
    QCOMPARE(result.length(), 4);
    QVERIFY(result.contains("..."));
    /* With maxLen=4: ellipsis=3, available=1, left=0, right=1 */
    QCOMPARE(result, QString("...g"));
}

QTEST_APPLESS_MAIN(TestQStringUtils)
#include "test_qsoccommonqstringutils.moc"
