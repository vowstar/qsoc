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
    /**
     * @brief Numeric base enumeration
     */
    enum class Base {
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
};

#endif // QSOCNUMBERINFO_H
