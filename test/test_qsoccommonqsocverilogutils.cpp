// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocverilogutils.h"

#include <QtTest>

class TestQSocVerilogUtils : public QObject
{
    Q_OBJECT

private slots:
    /* cleanTypeForWireDeclaration */
    void cleanTypeForWireDeclaration_logicWithRange();
    void cleanTypeForWireDeclaration_wireWithRange();
    void cleanTypeForWireDeclaration_regWithRange();
    void cleanTypeForWireDeclaration_onlyKeyword();
    void cleanTypeForWireDeclaration_emptyString();
    void cleanTypeForWireDeclaration_rangeOnly();

    /* parseSignalBitSelect */
    void parseSignalBitSelect_simpleName();
    void parseSignalBitSelect_withRange();
    void parseSignalBitSelect_withSingleBit();
    void parseSignalBitSelect_withSpaces();

    /* formatConditionForVerilog */
    void formatConditionForVerilog_zero();
    void formatConditionForVerilog_one();
    void formatConditionForVerilog_multibit();
    void formatConditionForVerilog_expression();

    /* generateIndent */
    void generateIndent_zero();
    void generateIndent_one();
    void generateIndent_multiple();

    /* isValidVerilogIdentifier */
    void isValidIdentifier_validNames();
    void isValidIdentifier_invalidStart();
    void isValidIdentifier_reservedWords();
    void isValidIdentifier_withDollar();
    void isValidIdentifier_empty();
    void isValidIdentifier_specialChars();

    /* escapeVerilogComment */
    void escapeVerilogComment_blockCommentEnd();
    void escapeVerilogComment_lineComment();
    void escapeVerilogComment_normalText();
};

/* cleanTypeForWireDeclaration */

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_logicWithRange()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("logic [7:0]");
    QCOMPARE(result, QString("[7:0]"));
}

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_wireWithRange()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("wire [31:0]");
    QCOMPARE(result, QString("[31:0]"));
}

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_regWithRange()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("reg [15:0]");
    QCOMPARE(result, QString("[15:0]"));
}

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_onlyKeyword()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("logic");
    QCOMPARE(result, QString(""));
}

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_emptyString()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("");
    QCOMPARE(result, QString(""));
}

void TestQSocVerilogUtils::cleanTypeForWireDeclaration_rangeOnly()
{
    QString result = QSocVerilogUtils::cleanTypeForWireDeclaration("[7:0]");
    QCOMPARE(result, QString("[7:0]"));
}

/* parseSignalBitSelect */

void TestQSocVerilogUtils::parseSignalBitSelect_simpleName()
{
    QPair<QString, QString> result = QSocVerilogUtils::parseSignalBitSelect("data");
    QCOMPARE(result.first, QString("data"));
    QCOMPARE(result.second, QString());
}

void TestQSocVerilogUtils::parseSignalBitSelect_withRange()
{
    QPair<QString, QString> result = QSocVerilogUtils::parseSignalBitSelect("data[7:0]");
    QCOMPARE(result.first, QString("data"));
    QCOMPARE(result.second, QString("[7:0]"));
}

void TestQSocVerilogUtils::parseSignalBitSelect_withSingleBit()
{
    QPair<QString, QString> result = QSocVerilogUtils::parseSignalBitSelect("data[3]");
    QCOMPARE(result.first, QString("data"));
    QCOMPARE(result.second, QString("[3]"));
}

void TestQSocVerilogUtils::parseSignalBitSelect_withSpaces()
{
    QPair<QString, QString> result = QSocVerilogUtils::parseSignalBitSelect("data [ 7 : 0 ]");
    QCOMPARE(result.first, QString("data"));
    QCOMPARE(result.second, QString("[ 7 : 0 ]"));
}

/* formatConditionForVerilog */

void TestQSocVerilogUtils::formatConditionForVerilog_zero()
{
    QString result = QSocVerilogUtils::formatConditionForVerilog("0");
    QCOMPARE(result, QString("1'b0"));
}

void TestQSocVerilogUtils::formatConditionForVerilog_one()
{
    QString result = QSocVerilogUtils::formatConditionForVerilog("1");
    QCOMPARE(result, QString("1'b1"));
}

void TestQSocVerilogUtils::formatConditionForVerilog_multibit()
{
    QString result = QSocVerilogUtils::formatConditionForVerilog("5");
    /* 5 requires 3 bits (2^3 = 8 > 5) */
    QCOMPARE(result, QString("3'd5"));
}

void TestQSocVerilogUtils::formatConditionForVerilog_expression()
{
    QString result = QSocVerilogUtils::formatConditionForVerilog("signal == 1");
    QVERIFY(result.contains("1'b1"));
    QVERIFY(result.contains("signal"));
}

/* generateIndent */

void TestQSocVerilogUtils::generateIndent_zero()
{
    QString result = QSocVerilogUtils::generateIndent(0);
    QCOMPARE(result, QString(""));
}

void TestQSocVerilogUtils::generateIndent_one()
{
    QString result = QSocVerilogUtils::generateIndent(1);
    QCOMPARE(result, QString("    ")); /* 4 spaces */
}

void TestQSocVerilogUtils::generateIndent_multiple()
{
    QString result = QSocVerilogUtils::generateIndent(3);
    QCOMPARE(result, QString("            ")); /* 12 spaces */
    QCOMPARE(result.length(), 12);
}

/* isValidVerilogIdentifier */

void TestQSocVerilogUtils::isValidIdentifier_validNames()
{
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("clk"));
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("data_valid"));
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("_internal"));
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("signal123"));
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("MySignal"));
}

void TestQSocVerilogUtils::isValidIdentifier_invalidStart()
{
    /* Cannot start with digit */
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("123abc"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("0signal"));
}

void TestQSocVerilogUtils::isValidIdentifier_reservedWords()
{
    /* Verilog keywords */
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("begin"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("end"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("module"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("wire"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("reg"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("if"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("else"));
}

void TestQSocVerilogUtils::isValidIdentifier_withDollar()
{
    /* Dollar sign is allowed in Verilog identifiers */
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("signal$1"));
    QVERIFY(QSocVerilogUtils::isValidVerilogIdentifier("test$var"));
}

void TestQSocVerilogUtils::isValidIdentifier_empty()
{
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier(""));
}

void TestQSocVerilogUtils::isValidIdentifier_specialChars()
{
    /* Special characters are not allowed */
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("signal-name"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("signal.name"));
    QVERIFY(!QSocVerilogUtils::isValidVerilogIdentifier("signal@name"));
}

/* escapeVerilogComment */

void TestQSocVerilogUtils::escapeVerilogComment_blockCommentEnd()
{
    QString result = QSocVerilogUtils::escapeVerilogComment("This */ ends early");
    QCOMPARE(result, QString("This * / ends early"));
}

void TestQSocVerilogUtils::escapeVerilogComment_lineComment()
{
    QString result = QSocVerilogUtils::escapeVerilogComment("This // is nested");
    QCOMPARE(result, QString("This / / is nested"));
}

void TestQSocVerilogUtils::escapeVerilogComment_normalText()
{
    QString result = QSocVerilogUtils::escapeVerilogComment("Normal comment text");
    QCOMPARE(result, QString("Normal comment text"));
}

QTEST_APPLESS_MAIN(TestQSocVerilogUtils)
#include "test_qsoccommonqsocverilogutils.moc"
