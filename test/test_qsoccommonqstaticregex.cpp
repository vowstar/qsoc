// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qstaticregex.h"

#include <QtTest>

class TestQStaticRegex : public QObject
{
    Q_OBJECT

private slots:
    /* isNameRegexValid */
    void isNameRegexValid_validPattern();
    void isNameRegexValid_emptyPattern();
    void isNameRegexValid_whitespaceOnly();
    void isNameRegexValid_invalidPattern();

    /* isNameRegularExpression */
    void isNameRegularExpression_plainText();
    void isNameRegularExpression_withStar();
    void isNameRegularExpression_withPlus();
    void isNameRegularExpression_withQuestion();
    void isNameRegularExpression_withBrackets();
    void isNameRegularExpression_withEscapeSequence();
    void isNameRegularExpression_withDot();
    void isNameRegularExpression_withAnchor();

    /* isNameExactMatch */
    void isNameExactMatch_plainTextMatch();
    void isNameExactMatch_plainTextNoMatch();
    void isNameExactMatch_regexPatternMatch();
    void isNameExactMatch_regexPatternNoMatch();
    void isNameExactMatch_emptyPattern();
    void isNameExactMatch_specialCharactersInPlainText();
    void isNameExactMatch_partialMatch();
};

/* isNameRegexValid */

void TestQStaticRegex::isNameRegexValid_validPattern()
{
    QRegularExpression regex("^test.*");
    QVERIFY(QStaticRegex::isNameRegexValid(regex));
}

void TestQStaticRegex::isNameRegexValid_emptyPattern()
{
    QRegularExpression regex("");
    QVERIFY(!QStaticRegex::isNameRegexValid(regex));
}

void TestQStaticRegex::isNameRegexValid_whitespaceOnly()
{
    QRegularExpression regex("   ");
    QVERIFY(!QStaticRegex::isNameRegexValid(regex));
}

void TestQStaticRegex::isNameRegexValid_invalidPattern()
{
    /* Unmatched bracket makes pattern invalid */
    QRegularExpression regex("[abc");
    QVERIFY(!QStaticRegex::isNameRegexValid(regex));
}

/* isNameRegularExpression */

void TestQStaticRegex::isNameRegularExpression_plainText()
{
    /* Plain identifiers should not be detected as regex */
    QVERIFY(!QStaticRegex::isNameRegularExpression("counter"));
    QVERIFY(!QStaticRegex::isNameRegularExpression("u_cpu_0"));
    QVERIFY(!QStaticRegex::isNameRegularExpression("data_valid"));
}

void TestQStaticRegex::isNameRegularExpression_withStar()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("test*"));
    QVERIFY(QStaticRegex::isNameRegularExpression(".*"));
}

void TestQStaticRegex::isNameRegularExpression_withPlus()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("test+"));
    QVERIFY(QStaticRegex::isNameRegularExpression("a+b"));
}

void TestQStaticRegex::isNameRegularExpression_withQuestion()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("test?"));
    QVERIFY(QStaticRegex::isNameRegularExpression("colou?r"));
}

void TestQStaticRegex::isNameRegularExpression_withBrackets()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("[abc]"));
    QVERIFY(QStaticRegex::isNameRegularExpression("test[0-9]"));
    QVERIFY(QStaticRegex::isNameRegularExpression("(group)"));
    QVERIFY(QStaticRegex::isNameRegularExpression("{3,5}"));
}

void TestQStaticRegex::isNameRegularExpression_withEscapeSequence()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("\\d+"));
    QVERIFY(QStaticRegex::isNameRegularExpression("\\w*"));
    QVERIFY(QStaticRegex::isNameRegularExpression("\\s"));
    QVERIFY(QStaticRegex::isNameRegularExpression("\\b"));
}

void TestQStaticRegex::isNameRegularExpression_withDot()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("test.txt"));
    QVERIFY(QStaticRegex::isNameRegularExpression(".+"));
}

void TestQStaticRegex::isNameRegularExpression_withAnchor()
{
    QVERIFY(QStaticRegex::isNameRegularExpression("^start"));
    QVERIFY(QStaticRegex::isNameRegularExpression("end$"));
}

/* isNameExactMatch */

void TestQStaticRegex::isNameExactMatch_plainTextMatch()
{
    /* Plain text pattern should match exactly */
    QRegularExpression regex("counter");
    QVERIFY(QStaticRegex::isNameExactMatch("counter", regex));
}

void TestQStaticRegex::isNameExactMatch_plainTextNoMatch()
{
    QRegularExpression regex("counter");
    QVERIFY(!QStaticRegex::isNameExactMatch("counter_0", regex));
    QVERIFY(!QStaticRegex::isNameExactMatch("u_counter", regex));
}

void TestQStaticRegex::isNameExactMatch_regexPatternMatch()
{
    /* Regex pattern should match */
    QRegularExpression regex("u_.*_0");
    QVERIFY(QStaticRegex::isNameExactMatch("u_counter_0", regex));
    QVERIFY(QStaticRegex::isNameExactMatch("u_timer_0", regex));
}

void TestQStaticRegex::isNameExactMatch_regexPatternNoMatch()
{
    QRegularExpression regex("u_.*_0");
    QVERIFY(!QStaticRegex::isNameExactMatch("u_counter_1", regex));
    QVERIFY(!QStaticRegex::isNameExactMatch("counter_0", regex));
}

void TestQStaticRegex::isNameExactMatch_emptyPattern()
{
    QRegularExpression regex("");
    QVERIFY(!QStaticRegex::isNameExactMatch("anything", regex));
}

void TestQStaticRegex::isNameExactMatch_specialCharactersInPlainText()
{
    /* Special characters in plain text should be escaped */
    QRegularExpression regex("test.txt");
    /* "test.txt" is treated as regex (contains '.'), so it matches "test_txt" */
    QVERIFY(QStaticRegex::isNameExactMatch("test.txt", regex));
    QVERIFY(QStaticRegex::isNameExactMatch("test_txt", regex));
}

void TestQStaticRegex::isNameExactMatch_partialMatch()
{
    /* Should NOT match partial strings */
    QRegularExpression regex("counter");
    QVERIFY(!QStaticRegex::isNameExactMatch("u_counter_0", regex));
    QVERIFY(!QStaticRegex::isNameExactMatch("mycounter", regex));
}

QTEST_APPLESS_MAIN(TestQStaticRegex)
#include "test_qsoccommonqstaticregex.moc"
