// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCNUMBERINFO_H
#define QSOCNUMBERINFO_H

#include <BigIntegerLibrary.h>
#include <QString>

/**
 * @brief QSocNumberInfo class to represent numeric literals with format information
 */
class QSocNumberInfo
{
public:
    /* Bound arbitrary-precision conversion work by the original text size. */
    static constexpr int MaximumNumericCharacters = 65536;
    static constexpr int MaximumDeclaredWidth     = 16777215;

    /**
     * @brief Numeric base enumeration
     */
    enum class Base : std::uint8_t {
        Binary      = 2,  /**< Base-2 (binary) number representation */
        Octal       = 8,  /**< Base-8 (octal) number representation */
        Decimal     = 10, /**< Base-10 (decimal) number representation */
        Hexadecimal = 16, /**< Base-16 (hexadecimal) number representation */
        Unknown     = 0   /**< Unknown or undefined numeric base */
    };

    QSocNumberInfo();
    ~QSocNumberInfo();

    QString    originalString;   /**< Original string representation */
    Base       base;             /**< Numeric base (2, 8, 10, 16) */
    BigInteger value;            /**< Actual numeric value */
    int        width;            /**< Bit width (either specified or calculated) */
    bool       hasExplicitWidth; /**< Whether width was explicitly specified */
    bool       errorDetected; /**< Whether the number is too large for quint64 or parsing failed */

    /**
     * @brief Helper function to convert BigInteger to string with a specified base
     * @param value BigInteger value to convert
     * @param base Base for conversion (2, 8, 10, 16)
     * @return String representation of the BigInteger in the specified base
     */
    static std::string bigIntegerToStringWithBase(const BigInteger &value, int base);

    /**
     * @brief Helper function to convert string to BigInteger with a specified base
     * @param str String to convert
     * @param base Base of the input string (2, 8, 10, 16)
     * @return BigInteger parsed from the string
     */
    static BigInteger stringToBigIntegerWithBase(const std::string &str, int base);

    /**
     * @brief Format the value according to its base
     * @return Formatted string (without width prefix)
     */
    QString format() const;

    /**
     * @brief Format the value with width prefix according to Verilog conventions
     * @return Complete Verilog-style formatted number string
     */
    QString formatVerilog() const;

    /**
     * @brief Format the value in C-style syntax
     * @return C-style formatted number string
     */
    QString formatC() const;

    /**
     * @brief Format the value with proper bit width (padded zeros)
     * @return Formatted string with proper bit width
     */
    QString formatVerilogProperWidth() const;

    /**
     * @brief Parse a Verilog or C-style numeric literal
     *
     * Handles formats like:
     * - Standard: 123, 0xFF, 0644
     * - Verilog: 8'b10101010, 32'hDEADBEEF
     * - With underscores: 32'h1234_5678, 16'b1010_1010
     *
     * If width is not specified, calculates a reasonable width based on the value.
     *
     * @param numStr Input string containing the numeric literal
     * @return QSocNumberInfo struct with parsed information
     */
    static QSocNumberInfo parseNumber(const QString &numStr);

    /**
     * @brief Convert BigInteger value to int64_t
     * @return int64_t representation, or 0 if conversion fails or value is too large
     */
    int64_t toInt64() const;

    /**
     * @brief Two-state number spelling classes for the strict entry
     */
    enum class Spelling : std::uint8_t {
        NotANumber,   /**< Not a complete two-state number */
        PlainDecimal, /**< Bare decimal digits without a leading zero */
        CStyle,       /**< 0x / 0b / 0o prefixed or leading-zero octal */
        Verilog       /**< Two-state Verilog literal, sized or unsized */
    };

    /**
     * @brief Generator handling for number-shaped text
     */
    enum class NumericTextKind : std::uint8_t {
        Normalize,   /**< Complete two-state number */
        PassThrough, /**< Opaque expression or non-normalized value */
        Reject       /**< Complete malformed number */
    };

    /**
     * @brief Result of deterministic numeric text classification
     */
    struct NumericText
    {
        NumericTextKind kind     = NumericTextKind::PassThrough;
        Spelling        spelling = Spelling::NotANumber;
    };

    /**
     * @brief Classify a complete number or a number-leading expression
     * @param text Candidate text
     * @return Handling class and spelling
     */
    static NumericText classifyNumericText(const QString &text);

    /**
     * @brief Normalize hexadecimal x-base aliases in an expression
     * @details Strings, comments, escaped identifiers, and unbased 'x stay unchanged.
     * @param text Candidate expression
     * @return Text with hexadecimal base markers written as h/H
     */
    static QString normalizeHexBaseAliases(const QString &text);

    /**
     * @brief Strictly classify a complete two-state number
     * @details The single gate for numeric conversion. Text longer than
     *          MaximumNumericCharacters returns before classification; the
     *          grammar consumes the whole string and requires a bounded,
     *          nonzero Verilog width.
     * @param numStr Candidate text
     * @return The spelling class, or NotANumber
     */
    static Spelling classifyTwoStateNumber(const QString &numStr);

    /**
     * @brief Truncate the stored magnitude to a bit width
     * @param bitWidth number of low bits to retain; nonpositive values do nothing
     */
    void truncateValueToWidth(int bitWidth);

    /**
     * @brief Convert the value to int64_t with explicit success status
     * @param convertedValue receives the value on success and is unchanged on failure
     * @return true when the value is valid and representable as int64_t
     */
    bool tryToInt64(int64_t &convertedValue) const;
};

#endif // QSOCNUMBERINFO_H
