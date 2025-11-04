// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocnumberinfo.h"

#include <QtTest>

class TestQSocNumberInfo : public QObject
{
    Q_OBJECT

private slots:
    /* Verilog format parsing */
    void parseNumber_verilogHexWithWidth();
    void parseNumber_verilogBinaryWithWidth();
    void parseNumber_verilogDecimalWithWidth();
    void parseNumber_verilogOctalWithWidth();
    void parseNumber_verilogHexWithoutWidth();
    void parseNumber_verilogWithUnderscore();

    /* C-style format parsing */
    void parseNumber_cStyleHex();
    void parseNumber_cStyleBinary();
    void parseNumber_cStyleOctal();
    void parseNumber_cStyleDecimal();

    /* Format output */
    void format_binary();
    void format_octal();
    void format_decimal();
    void format_hexadecimal();

    void formatVerilog_withWidth();
    void formatVerilog_withoutWidth();

    void formatC_binary();
    void formatC_hexadecimal();
    void formatC_octal();
    void formatC_decimal();

    void formatVerilogProperWidth_binary();
    void formatVerilogProperWidth_hexadecimal();

    /* BigInteger conversion */
    void bigIntegerConversion_binary();
    void bigIntegerConversion_octal();
    void bigIntegerConversion_hexadecimal();
    void bigIntegerConversion_decimal();

    /* toInt64 */
    void toInt64_simpleValue();
    void toInt64_zero();
    void toInt64_maxInt64();

    /* Edge cases */
    void parseNumber_emptyString();
    void parseNumber_zero();
    void parseNumber_vectorRange();
};

/* Verilog format parsing */

void TestQSocNumberInfo::parseNumber_verilogHexWithWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'hDEADBEEF");
    QCOMPARE(info.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(info.width, 32);
    QCOMPARE(info.hasExplicitWidth, true);
    QCOMPARE(info.errorDetected, false);
    QCOMPARE(info.toInt64(), 0xDEADBEEF);
}

void TestQSocNumberInfo::parseNumber_verilogBinaryWithWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("8'b10101010");
    QCOMPARE(info.base, QSocNumberInfo::Base::Binary);
    QCOMPARE(info.width, 8);
    QCOMPARE(info.hasExplicitWidth, true);
    QCOMPARE(info.toInt64(), 0xAA);
}

void TestQSocNumberInfo::parseNumber_verilogDecimalWithWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("16'd1234");
    QCOMPARE(info.base, QSocNumberInfo::Base::Decimal);
    QCOMPARE(info.width, 16);
    QCOMPARE(info.hasExplicitWidth, true);
    QCOMPARE(info.toInt64(), 1234);
}

void TestQSocNumberInfo::parseNumber_verilogOctalWithWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("12'o755");
    QCOMPARE(info.base, QSocNumberInfo::Base::Octal);
    QCOMPARE(info.width, 12);
    QCOMPARE(info.hasExplicitWidth, true);
    QCOMPARE(info.toInt64(), 0755);
}

void TestQSocNumberInfo::parseNumber_verilogHexWithoutWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("'hFF");
    QCOMPARE(info.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(info.hasExplicitWidth, false);
    QCOMPARE(info.toInt64(), 0xFF);
    /* Width should be calculated automatically (8 bits for 0xFF) */
    QCOMPARE(info.width, 8);
}

void TestQSocNumberInfo::parseNumber_verilogWithUnderscore()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'h1234_5678");
    QCOMPARE(info.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(info.width, 32);
    QCOMPARE(info.toInt64(), 0x12345678);
}

/* C-style format parsing */

void TestQSocNumberInfo::parseNumber_cStyleHex()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0xDEAD");
    QCOMPARE(info.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(info.toInt64(), 0xDEAD);
}

void TestQSocNumberInfo::parseNumber_cStyleBinary()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0b1010");
    QCOMPARE(info.base, QSocNumberInfo::Base::Binary);
    QCOMPARE(info.toInt64(), 0b1010);
}

void TestQSocNumberInfo::parseNumber_cStyleOctal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0644");
    QCOMPARE(info.base, QSocNumberInfo::Base::Octal);
    QCOMPARE(info.toInt64(), 0644);
}

void TestQSocNumberInfo::parseNumber_cStyleDecimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("1234");
    QCOMPARE(info.base, QSocNumberInfo::Base::Decimal);
    QCOMPARE(info.toInt64(), 1234);
}

/* Format output */

void TestQSocNumberInfo::format_binary()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("8'b10101010");
    QCOMPARE(info.format(), QString("'b10101010"));
}

void TestQSocNumberInfo::format_octal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("12'o755");
    QCOMPARE(info.format(), QString("'o755"));
}

void TestQSocNumberInfo::format_decimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("16'd1234");
    QCOMPARE(info.format(), QString("'d1234"));
}

void TestQSocNumberInfo::format_hexadecimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'hDEADBEEF");
    /* Hex should be lowercase */
    QCOMPARE(info.format(), QString("'hdeadbeef"));
}

void TestQSocNumberInfo::formatVerilog_withWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'hDEAD");
    QCOMPARE(info.formatVerilog(), QString("32'hdead"));
}

void TestQSocNumberInfo::formatVerilog_withoutWidth()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("'hFF");
    /* Width is auto-calculated to 8 */
    QCOMPARE(info.formatVerilog(), QString("8'hff"));
}

void TestQSocNumberInfo::formatC_binary()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("8'b1010");
    QCOMPARE(info.formatC(), QString("0b1010"));
}

void TestQSocNumberInfo::formatC_hexadecimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'hDEAD");
    QCOMPARE(info.formatC(), QString("0xdead"));
}

void TestQSocNumberInfo::formatC_octal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("12'o755");
    QCOMPARE(info.formatC(), QString("0755"));
}

void TestQSocNumberInfo::formatC_decimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("1234");
    QCOMPARE(info.formatC(), QString("1234"));
}

void TestQSocNumberInfo::formatVerilogProperWidth_binary()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("8'b1010");
    /* Should pad to 8 bits: 00001010 */
    QCOMPARE(info.formatVerilogProperWidth(), QString("8'b00001010"));
}

void TestQSocNumberInfo::formatVerilogProperWidth_hexadecimal()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("32'hDEAD");
    /* 32 bits = 8 hex digits: 0000DEAD */
    QCOMPARE(info.formatVerilogProperWidth(), QString("32'h0000dead"));
}

/* BigInteger conversion */

void TestQSocNumberInfo::bigIntegerConversion_binary()
{
    BigInteger  val = QSocNumberInfo::stringToBigIntegerWithBase("10101010", 2);
    std::string str = QSocNumberInfo::bigIntegerToStringWithBase(val, 2);
    QCOMPARE(QString::fromStdString(str), QString("10101010"));
}

void TestQSocNumberInfo::bigIntegerConversion_octal()
{
    BigInteger  val = QSocNumberInfo::stringToBigIntegerWithBase("755", 8);
    std::string str = QSocNumberInfo::bigIntegerToStringWithBase(val, 8);
    QCOMPARE(QString::fromStdString(str), QString("755"));
}

void TestQSocNumberInfo::bigIntegerConversion_hexadecimal()
{
    BigInteger  val = QSocNumberInfo::stringToBigIntegerWithBase("DEADBEEF", 16);
    std::string str = QSocNumberInfo::bigIntegerToStringWithBase(val, 16);
    /* BigInteger output is uppercase, we need to compare case-insensitively */
    QCOMPARE(QString::fromStdString(str).toLower(), QString("deadbeef"));
}

void TestQSocNumberInfo::bigIntegerConversion_decimal()
{
    BigInteger  val = QSocNumberInfo::stringToBigIntegerWithBase("123456789", 10);
    std::string str = QSocNumberInfo::bigIntegerToStringWithBase(val, 10);
    QCOMPARE(QString::fromStdString(str), QString("123456789"));
}

/* toInt64 */

void TestQSocNumberInfo::toInt64_simpleValue()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("1234");
    QCOMPARE(info.toInt64(), static_cast<int64_t>(1234));
}

void TestQSocNumberInfo::toInt64_zero()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0");
    QCOMPARE(info.toInt64(), static_cast<int64_t>(0));
}

void TestQSocNumberInfo::toInt64_maxInt64()
{
    /* Test with a large but valid int64_t value */
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0x7FFFFFFFFFFFFFFF");
    QCOMPARE(info.toInt64(), std::numeric_limits<int64_t>::max());
}

/* Edge cases */

void TestQSocNumberInfo::parseNumber_emptyString()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("");
    QCOMPARE(info.base, QSocNumberInfo::Base::Unknown);
}

void TestQSocNumberInfo::parseNumber_zero()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0");
    QCOMPARE(info.base, QSocNumberInfo::Base::Decimal);
    QCOMPARE(info.toInt64(), static_cast<int64_t>(0));
    QCOMPARE(info.width, 1); /* Special case for zero */
}

void TestQSocNumberInfo::parseNumber_vectorRange()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("[31:0]");
    QCOMPARE(info.width, 32);
    QCOMPARE(info.hasExplicitWidth, true);
}

QTEST_APPLESS_MAIN(TestQSocNumberInfo)
#include "test_qsoccommonqsocnumberinfo.moc"
