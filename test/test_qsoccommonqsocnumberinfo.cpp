// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocnumberinfo.h"
#include "qsoc_test.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
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
    void parseNumber_cStyleOctalPrefixed();
    void tryToInt64_boundaries();
    void stringToBigInteger_matchesReference();
    void stringToBigInteger_skipsInvalidCharacters();
    void stringToBigInteger_rejectsOversizedText();
    void parseNumber_maximumDecimalCompletesWithinBound();
    void classifyNumericText_contract();
    void classifyNumericText_commentsScaleLinearly();
    void normalizeHexBaseAliases_contract();
    void normalizeHexBaseAliases_commentsScaleLinearly();
    void tryToInt64_rejectsOverflow();
    void truncateValueToWidthKeepsLowBits();
    void parseNumber_cStyleDecimal();

    /* Format output */
    void format_binary();
    void format_octal();
    void format_decimal();
    void format_hexadecimal();
    void formatDecimalMaximumCompletesWithinBound();

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

void TestQSocNumberInfo::parseNumber_cStyleOctalPrefixed()
{
    QSocNumberInfo info = QSocNumberInfo::parseNumber("0o644");
    QCOMPARE(info.base, QSocNumberInfo::Base::Octal);
    QCOMPARE(info.toInt64(), 0644);
    QVERIFY(!info.errorDetected);

    QSocNumberInfo upper = QSocNumberInfo::parseNumber("0O7_7");
    QCOMPARE(upper.base, QSocNumberInfo::Base::Octal);
    QCOMPARE(upper.toInt64(), 077);
}

void TestQSocNumberInfo::stringToBigInteger_matchesReference()
{
    /* Conversion paths must agree with the original recurrence. */
    const auto reference = [](const std::string &digits, int base) {
        BigUnsigned result(0);
        for (const char character : digits) {
            int digit = 0;
            if (character >= '0' && character <= '9') {
                digit = character - '0';
            } else if (character >= 'a' && character <= 'f') {
                digit = character - 'a' + 10;
            } else {
                digit = character - 'A' + 10;
            }
            result = result * BigUnsigned(base) + BigUnsigned(digit);
        }
        return BigInteger(result);
    };

    const std::string hexDigits = "DeadBeefCafe0123456789abcdefABCDEF";
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase(hexDigits, 16) == reference(hexDigits, 16));

    const std::string decimalDigits = "123456789012345678901234567890123456789";
    QVERIFY(
        QSocNumberInfo::stringToBigIntegerWithBase(decimalDigits, 10)
        == reference(decimalDigits, 10));

    const std::string octalDigits = "76543210076543210076543210";
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase(octalDigits, 8) == reference(octalDigits, 8));

    const std::string binaryDigits = "1101010101111000010101010101010101010101011";
    QVERIFY(
        QSocNumberInfo::stringToBigIntegerWithBase(binaryDigits, 2) == reference(binaryDigits, 2));

    for (const char *decimalText :
         {"99999999",
          "999999999",
          "1000000000",
          "99999999999999999",
          "999999999999999999",
          "1000000000000000000",
          "4294967295",
          "4294967296",
          "18446744073709551615",
          "18446744073709551616"}) {
        const std::string digits(decimalText);
        QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase(digits, 10) == reference(digits, 10));
    }

    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase("102", 3) == reference("102", 3));
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase("B5", 12) == reference("B5", 12));
}

void TestQSocNumberInfo::stringToBigInteger_skipsInvalidCharacters()
{
    /* Lenient skipping is long-standing behavior other callers rely on. */
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase("Z5", 16) == BigInteger(5));
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase("19", 8) == BigInteger(1));
    QVERIFY(QSocNumberInfo::stringToBigIntegerWithBase("ZZ", 16) == BigInteger(0));
}

void TestQSocNumberInfo::stringToBigInteger_rejectsOversizedText()
{
    const std::string oversized(QSocNumberInfo::MaximumNumericCharacters + 1, '7');
    bool              thrown = false;
    try {
        QSocNumberInfo::stringToBigIntegerWithBase(oversized, 10);
    } catch (const std::invalid_argument &) {
        thrown = true;
    }
    QVERIFY(thrown);

    /* And parseNumber surfaces it as errorDetected instead of a value. */
    const QSocNumberInfo parsed = QSocNumberInfo::parseNumber(
        QString::fromStdString(
            "0x" + std::string(QSocNumberInfo::MaximumNumericCharacters + 1, 'F')));
    QVERIFY(parsed.errorDetected);

    const QString maximumRaw = "0x1" + QString(QSocNumberInfo::MaximumNumericCharacters - 3, '_');
    QCOMPARE(maximumRaw.size(), QSocNumberInfo::MaximumNumericCharacters);
    const QSocNumberInfo maximumParsed = QSocNumberInfo::parseNumber(maximumRaw);
    QVERIFY(!maximumParsed.errorDetected);
    QCOMPARE(maximumParsed.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(maximumParsed.width, 1);
    QVERIFY(maximumParsed.value == BigInteger(1));

    const QString        oversizedRaw = maximumRaw + "_";
    const QSocNumberInfo rejected     = QSocNumberInfo::parseNumber(oversizedRaw);
    QVERIFY(rejected.errorDetected);
    QCOMPARE(rejected.originalString, oversizedRaw);
    QCOMPARE(rejected.base, QSocNumberInfo::Base::Unknown);
    QCOMPARE(rejected.width, 0);
    QVERIFY(!rejected.hasExplicitWidth);
    QVERIFY(rejected.value == BigInteger(0));

    const QString        underscoreBypass = "0x1" + QString(70000, '_');
    const QSocNumberInfo bypassRejected   = QSocNumberInfo::parseNumber(underscoreBypass);
    QVERIFY(bypassRejected.errorDetected);
    QCOMPARE(bypassRejected.base, QSocNumberInfo::Base::Unknown);
    QCOMPARE(bypassRejected.width, 0);
    QVERIFY(bypassRejected.value == BigInteger(0));

    bool underscoreThrown = false;
    try {
        QSocNumberInfo::stringToBigIntegerWithBase(
            std::string(QSocNumberInfo::MaximumNumericCharacters + 1, '_'), 16);
    } catch (const std::invalid_argument &) {
        underscoreThrown = true;
    }
    QVERIFY(underscoreThrown);
    QCOMPARE(
        QSocNumberInfo::classifyTwoStateNumber(oversizedRaw), QSocNumberInfo::Spelling::NotANumber);

    const QSocNumberInfo hexAlias = QSocNumberInfo::parseNumber("8'xF");
    QCOMPARE(hexAlias.base, QSocNumberInfo::Base::Hexadecimal);
    QCOMPARE(hexAlias.width, 8);
    QCOMPARE(hexAlias.toInt64(), int64_t(15));

    const QString underscoredWidth = "1_______6'hF";
    QCOMPARE(
        QSocNumberInfo::classifyTwoStateNumber(underscoredWidth), QSocNumberInfo::Spelling::Verilog);
    const QSocNumberInfo underscoredParsed = QSocNumberInfo::parseNumber(underscoredWidth);
    QVERIFY(!underscoredParsed.errorDetected);
    QCOMPARE(underscoredParsed.width, 16);
    QCOMPARE(underscoredParsed.toInt64(), int64_t(15));

    for (const QString &leadingZeroWidth : {QStringLiteral("08'h1"), QStringLiteral("0_8'h1")}) {
        QCOMPARE(
            QSocNumberInfo::classifyTwoStateNumber(leadingZeroWidth),
            QSocNumberInfo::Spelling::Verilog);
        const QSocNumberInfo leadingZeroParsed = QSocNumberInfo::parseNumber(leadingZeroWidth);
        QVERIFY(!leadingZeroParsed.errorDetected);
        QCOMPARE(leadingZeroParsed.width, 8);
        QCOMPARE(leadingZeroParsed.toInt64(), int64_t(1));
    }
}

void TestQSocNumberInfo::parseNumber_maximumDecimalCompletesWithinBound()
{
    const QString maximumDecimal(QSocNumberInfo::MaximumNumericCharacters, '9');

    QElapsedTimer timer;
    timer.start();
    for (int iteration = 0; iteration < 3; ++iteration) {
        const QSocNumberInfo parsed = QSocNumberInfo::parseNumber(maximumDecimal);
        QVERIFY(!parsed.errorDetected);
        QCOMPARE(parsed.base, QSocNumberInfo::Base::Decimal);
        QCOMPARE(parsed.width, 217706);
    }
    QVERIFY2(timer.elapsed() < 5000, "maximum decimal parsing exceeded the deadline");
}

void TestQSocNumberInfo::classifyNumericText_contract()
{
    using Kind     = QSocNumberInfo::NumericTextKind;
    using Spelling = QSocNumberInfo::Spelling;

    struct NumericCase
    {
        const char *text;
        Kind        kind;
        Spelling    spelling;
    };

    const QList<NumericCase> cases{
        {"abc123", Kind::PassThrough, Spelling::NotANumber},
        {"`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"1+`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"8'hFF+`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"8'xFF+`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"8'xFF+1", Kind::PassThrough, Spelling::NotANumber},
        {"1+8'xF", Kind::PassThrough, Spelling::NotANumber},
        {"8'xF+8'x1", Kind::PassThrough, Spelling::NotANumber},
        {"8'(x)", Kind::PassThrough, Spelling::NotANumber},
        {"8' (x)", Kind::PassThrough, Spelling::NotANumber},
        {"8 ' (x)", Kind::PassThrough, Spelling::NotANumber},
        {"8' /* gap */ (x)", Kind::PassThrough, Spelling::NotANumber},
        {"8' `CAST", Kind::PassThrough, Spelling::NotANumber},
        {"8/* gap */'(1)", Kind::PassThrough, Spelling::NotANumber},
        {"8'// gap\n(1)", Kind::PassThrough, Spelling::NotANumber},
        {"_8'(1)", Kind::PassThrough, Spelling::NotANumber},
        {"__8 '(1)", Kind::PassThrough, Spelling::NotANumber},
        {"_8' /* gap */ (1)", Kind::PassThrough, Spelling::NotANumber},
        {"___'{default:0}", Kind::PassThrough, Spelling::NotANumber},
        {"8'h/* gap */FF", Kind::PassThrough, Spelling::NotANumber},
        {"8'x/* gap */FF", Kind::PassThrough, Spelling::NotANumber},
        {"8/* gap */'hFF", Kind::PassThrough, Spelling::NotANumber},
        {"8/* gap */'xFF", Kind::PassThrough, Spelling::NotANumber},
        {"8// gap\n'hFF", Kind::PassThrough, Spelling::NotANumber},
        {"8'h// gap\nFF", Kind::PassThrough, Spelling::NotANumber},
        {"8'hFF// gap\n+1", Kind::PassThrough, Spelling::NotANumber},
        {"8'hF/* gap */", Kind::PassThrough, Spelling::NotANumber},
        {"8'hF /* gap */", Kind::PassThrough, Spelling::NotANumber},
        {"8'xF /* gap */", Kind::PassThrough, Spelling::NotANumber},
        {"8'hF // gap\n+1", Kind::PassThrough, Spelling::NotANumber},
        {"8'h8'(1)", Kind::PassThrough, Spelling::NotANumber},
        {"'{default:'0}", Kind::PassThrough, Spelling::NotANumber},
        {"'x?1:0", Kind::PassThrough, Spelling::NotANumber},
        {"'x/* gap */", Kind::PassThrough, Spelling::NotANumber},
        {"'x /* gap */", Kind::PassThrough, Spelling::NotANumber},
        {"'z/* gap */+1", Kind::PassThrough, Spelling::NotANumber},
        {"1e9999", Kind::PassThrough, Spelling::NotANumber},
        {"1e-9999", Kind::PassThrough, Spelling::NotANumber},
        {"1.5ns", Kind::PassThrough, Spelling::NotANumber},
        {"8'sd5", Kind::PassThrough, Spelling::NotANumber},
        {"8'sxFF", Kind::PassThrough, Spelling::NotANumber},
        {"8'hZ?", Kind::PassThrough, Spelling::NotANumber},
        {"8'xZ?", Kind::PassThrough, Spelling::NotANumber},
        {"8'dx", Kind::PassThrough, Spelling::NotANumber},
        {"8'd?", Kind::PassThrough, Spelling::NotANumber},
        {"8 'h1", Kind::PassThrough, Spelling::NotANumber},
        {"8 'h 1", Kind::PassThrough, Spelling::NotANumber},
        {"8 'shF", Kind::PassThrough, Spelling::NotANumber},
        {"1 'hF", Kind::PassThrough, Spelling::NotANumber},
        {"1 'shF", Kind::PassThrough, Spelling::NotANumber},
        {"8 'x Z?", Kind::PassThrough, Spelling::NotANumber},
        {"123", Kind::Normalize, Spelling::PlainDecimal},
        {"1_", Kind::Normalize, Spelling::PlainDecimal},
        {"0x1F", Kind::Normalize, Spelling::CStyle},
        {"0o644", Kind::Normalize, Spelling::CStyle},
        {"0644", Kind::Normalize, Spelling::CStyle},
        {"0", Kind::Normalize, Spelling::CStyle},
        {"00", Kind::Normalize, Spelling::CStyle},
        {"0_0", Kind::Normalize, Spelling::CStyle},
        {"8'hF_", Kind::Normalize, Spelling::Verilog},
        {"8_'h1", Kind::Normalize, Spelling::Verilog},
        {"08'h1", Kind::Normalize, Spelling::Verilog},
        {"0_8'h1", Kind::Normalize, Spelling::Verilog},
        {"8'xFF", Kind::Normalize, Spelling::Verilog},
        {"0x+`VALUE", Kind::Reject, Spelling::NotANumber},
        {"0xFF+1", Kind::Reject, Spelling::NotANumber},
        {"8'h`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"8'x`VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"8'`CAST", Kind::PassThrough, Spelling::NotANumber},
        {"8's`BASE_VALUE", Kind::PassThrough, Spelling::NotANumber},
        {"1.`FRACTION", Kind::PassThrough, Spelling::NotANumber},
        {"1e`EXPONENT", Kind::PassThrough, Spelling::NotANumber},
        {"1e+`EXPONENT", Kind::PassThrough, Spelling::NotANumber},
        {".5", Kind::Reject, Spelling::NotANumber},
        {".e3", Kind::Reject, Spelling::NotANumber},
        {"'?", Kind::Reject, Spelling::NotANumber},
        {"'x// gap", Kind::Reject, Spelling::NotANumber},
        {"'x/* gap", Kind::Reject, Spelling::NotANumber},
        {"1step", Kind::Reject, Spelling::NotANumber},
        {"8' h1", Kind::Reject, Spelling::NotANumber},
        {"8's h1", Kind::Reject, Spelling::NotANumber},
        {"8'/* gap */Q", Kind::Reject, Spelling::NotANumber},
        {"8/* gap */'GG", Kind::Reject, Spelling::NotANumber},
        {"8'h/* gap */GG", Kind::Reject, Spelling::NotANumber},
        {"8'h// gap", Kind::Reject, Spelling::NotANumber},
        {"8'h/* gap", Kind::Reject, Spelling::NotANumber},
        {"8'hF/* gap", Kind::Reject, Spelling::NotANumber},
        {"8'hF /* gap", Kind::Reject, Spelling::NotANumber},
        {"8'hF // gap", Kind::Reject, Spelling::NotANumber},
        {"_8'h1", Kind::Reject, Spelling::NotANumber},
        {"__8 'h1", Kind::Reject, Spelling::NotANumber},
        {"___ 'h1", Kind::Reject, Spelling::NotANumber},
        {"16'd", Kind::Reject, Spelling::NotANumber},
        {"'d", Kind::Reject, Spelling::NotANumber},
        {"8'", Kind::Reject, Spelling::NotANumber},
        {"0x", Kind::Reject, Spelling::NotANumber},
        {"0678", Kind::Reject, Spelling::NotANumber},
        {"4'b12", Kind::Reject, Spelling::NotANumber},
        {"8'd12x", Kind::Reject, Spelling::NotANumber},
        {"8'd1?", Kind::Reject, Spelling::NotANumber},
        {"8'dx1", Kind::Reject, Spelling::NotANumber},
        {"8'd1x2", Kind::Reject, Spelling::NotANumber},
        {"0x___", Kind::Reject, Spelling::NotANumber},
        {"16'h___", Kind::Reject, Spelling::NotANumber},
        {"0'h1", Kind::Reject, Spelling::NotANumber},
        {"16777216'h1", Kind::Reject, Spelling::NotANumber},
        {"1.2e+_3", Kind::Reject, Spelling::NotANumber},
        {"1e2ns", Kind::Reject, Spelling::NotANumber},
        {"1.0e2ns", Kind::Reject, Spelling::NotANumber},
        {"1.0 /* gap", Kind::Reject, Spelling::NotANumber},
        {"1ns /* gap", Kind::Reject, Spelling::NotANumber},
        {"1e2 /* gap", Kind::Reject, Spelling::NotANumber},
        {"'x /* gap", Kind::Reject, Spelling::NotANumber},
        {"123abc", Kind::Reject, Spelling::NotANumber},
    };

    for (const NumericCase &numericCase : cases) {
        const QSocNumberInfo::NumericText result = QSocNumberInfo::classifyNumericText(
            QString::fromLatin1(numericCase.text));
        QVERIFY2(result.kind == numericCase.kind, numericCase.text);
        QCOMPARE(result.spelling, numericCase.spelling);
    }

    const QString oversizedTwoState   = "262144'h"
                                        + QString(QSocNumberInfo::MaximumNumericCharacters, 'F');
    const QString oversizedFourState  = "262144'h"
                                        + QString(QSocNumberInfo::MaximumNumericCharacters, 'x');
    const QString oversizedSigned     = "262144'sh"
                                        + QString(QSocNumberInfo::MaximumNumericCharacters, 'F');
    const QString oversizedExpression = "1+"
                                        + QString(QSocNumberInfo::MaximumNumericCharacters, ' ')
                                        + "2";
    const QString oversizedMalformed  = "8'h"
                                        + QString(QSocNumberInfo::MaximumNumericCharacters - 3, 'F')
                                        + "Q";
    QCOMPARE(QSocNumberInfo::classifyNumericText(oversizedTwoState).kind, Kind::Reject);
    QCOMPARE(QSocNumberInfo::classifyNumericText(oversizedFourState).kind, Kind::PassThrough);
    QCOMPARE(QSocNumberInfo::classifyNumericText(oversizedSigned).kind, Kind::PassThrough);
    QCOMPARE(QSocNumberInfo::classifyNumericText(oversizedExpression).kind, Kind::PassThrough);
    QCOMPARE(QSocNumberInfo::classifyNumericText(oversizedMalformed).kind, Kind::Reject);
}

void TestQSocNumberInfo::classifyNumericText_commentsScaleLinearly()
{
    QString text = "8";
    text.reserve(1600004);
    for (int index = 0; index < 400000; ++index) {
        text += "//x\n";
    }
    text += "'h1";

    QElapsedTimer timer;
    timer.start();
    QCOMPARE(
        QSocNumberInfo::classifyNumericText(text).kind,
        QSocNumberInfo::NumericTextKind::PassThrough);
    QVERIFY2(timer.elapsed() < 5000, "numeric comment scan exceeded the linear-time deadline");
}

void TestQSocNumberInfo::normalizeHexBaseAliases_contract()
{
    struct AliasCase
    {
        const char *input;
        const char *expected;
    };

    const QList<AliasCase> cases{
        {"8'xF + 1", "8'hF + 1"},
        {"1 + 8'xF", "1 + 8'hF"},
        {"8'xF + 8'X1", "8'hF + 8'H1"},
        {"8'sxF + 'sx1", "8'shF + 'sh1"},
        {"8'SXF", "8'SHF"},
        {"'xF + 'X1", "'hF + 'H1"},
        {"8/* size */'x/* value */FF", "8/* size */'h/* value */FF"},
        {"8'x// value\nFF", "8'h// value\nFF"},
        {"'x", "'x"},
        {"'x?1:0", "'x?1:0"},
        {"'x /* condition */ ? 1 : 0", "'x /* condition */ ? 1 : 0"},
        {"\"8'xF\"", "\"8'xF\""},
        {"\"escaped \\\"8'xF\\\"\"", "\"escaped \\\"8'xF\\\"\""},
        {"\\escaped8'xF  + 8'x1", "\\escaped8'xF  + 8'h1"},
        {"// 8'xF\n8'x1", "// 8'xF\n8'h1"},
        {"/* 8'xF */ 8'x1", "/* 8'xF */ 8'h1"},
        {"8'xF + \"8'x1\" + 8'x2", "8'hF + \"8'x1\" + 8'h2"},
        {"8'x/* unterminated", "8'x/* unterminated"},
    };

    for (const AliasCase &aliasCase : cases) {
        QCOMPARE(
            QSocNumberInfo::normalizeHexBaseAliases(QString::fromLatin1(aliasCase.input)),
            QString::fromLatin1(aliasCase.expected));
    }
}

void TestQSocNumberInfo::normalizeHexBaseAliases_commentsScaleLinearly()
{
    QString text;
    text.reserve(1600000);
    for (int index = 0; index < 100000; ++index) {
        text += "8'xF+/*8'x0*/";
    }

    QElapsedTimer timer;
    timer.start();
    const QString normalized = QSocNumberInfo::normalizeHexBaseAliases(text);
    QVERIFY2(timer.elapsed() < 5000, "x-base alias scan exceeded the linear-time deadline");
    QCOMPARE(normalized.count(QStringLiteral("8'hF")), 100000);
    QCOMPARE(normalized.count(QStringLiteral("8'x0")), 100000);
}

void TestQSocNumberInfo::tryToInt64_boundaries()
{
    const QSocNumberInfo maximum        = QSocNumberInfo::parseNumber("0x7FFFFFFFFFFFFFFF");
    int64_t              convertedValue = 0;
    QVERIFY(maximum.tryToInt64(convertedValue));
    QCOMPARE(convertedValue, std::numeric_limits<int64_t>::max());

    QSocNumberInfo minimum;
    minimum.value = BigInteger(
        BigUnsigned(std::numeric_limits<int64_t>::max()) + BigUnsigned(1), BigInteger::negative);
    QVERIFY(minimum.tryToInt64(convertedValue));
    QCOMPARE(convertedValue, std::numeric_limits<int64_t>::min());
}

void TestQSocNumberInfo::tryToInt64_rejectsOverflow()
{
    const QSocNumberInfo overflow       = QSocNumberInfo::parseNumber("0x8000000000000000");
    int64_t              convertedValue = 17;
    QVERIFY(!overflow.tryToInt64(convertedValue));
    QCOMPARE(convertedValue, int64_t(17));
}

void TestQSocNumberInfo::truncateValueToWidthKeepsLowBits()
{
    QSocNumberInfo number = QSocNumberInfo::parseNumber("16'hFFFFF");
    number.truncateValueToWidth(number.width);

    int64_t convertedValue = 0;
    QVERIFY(number.tryToInt64(convertedValue));
    QCOMPARE(convertedValue, int64_t(0xFFFF));

    /* A value already inside the width is untouched. */
    QSocNumberInfo small = QSocNumberInfo::parseNumber("16'h1F");
    small.truncateValueToWidth(small.width);
    QVERIFY(small.tryToInt64(convertedValue));
    QCOMPARE(convertedValue, int64_t(0x1F));
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

void TestQSocNumberInfo::formatDecimalMaximumCompletesWithinBound()
{
    if (qEnvironmentVariableIsSet("QSOC_TEST_DECIMAL_FORMAT_CHILD")) {
        const QString        digits(QSocNumberInfo::MaximumNumericCharacters, '9');
        const QSocNumberInfo parsed = QSocNumberInfo::parseNumber(digits);
        QVERIFY(!parsed.errorDetected);
        const QString expected = "217706'd" + digits;
        for (int iteration = 0; iteration < 3; ++iteration) {
            QCOMPARE(parsed.formatVerilog(), expected);
        }
        return;
    }

    QTemporaryDir runDirectory;
    QVERIFY(runDirectory.isValid());

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QSOC_TEST_DECIMAL_FORMAT_CHILD", "1");
    environment.insert("TMPDIR", runDirectory.path());

    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.setWorkingDirectory(runDirectory.path());
    child.start(QCoreApplication::applicationFilePath(), {"formatDecimalMaximumCompletesWithinBound"});
    QVERIFY(child.waitForStarted(5000));
    const bool stopped = child.waitForFinished(5000);
    if (!stopped) {
        child.kill();
        child.waitForFinished(1000);
    }
    const QByteArray childOutput = child.readAll();
    QVERIFY2(stopped, childOutput.constData());
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        childOutput.constData());
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
    for (const char *decimalText :
         {"0",
          "1",
          "999999999",
          "1000000000",
          "1000000001",
          "999999999999999999",
          "1000000000000000000",
          "1000000000000000001",
          "4294967295",
          "4294967296",
          "4294967297",
          "18446744073709551615",
          "18446744073709551616",
          "18446744073709551617",
          "00000123456789"}) {
        const BigInteger  value    = QSocNumberInfo::stringToBigIntegerWithBase(decimalText, 10);
        const std::string expected = std::string(BigUnsignedInABase(value.getMagnitude(), 10));
        QCOMPARE(QSocNumberInfo::bigIntegerToStringWithBase(value, 10), expected);
    }

    const BigInteger negative(BigUnsigned(1000000001), BigInteger::negative);
    QCOMPARE(QSocNumberInfo::bigIntegerToStringWithBase(negative, 10), std::string("-1000000001"));
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

QSOC_TEST_MAIN(TestQSocNumberInfo)
#include "test_qsoccommonqsocnumberinfo.moc"
