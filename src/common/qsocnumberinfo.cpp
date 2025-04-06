// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocnumberinfo.h"

QSocNumberInfo::QSocNumberInfo()
    : base(Base::Unknown)
    , value(0)
    , width(0)
    , hasExplicitWidth(false)
    , errorDetected(false)
{}

QSocNumberInfo::~QSocNumberInfo() = default;

std::string QSocNumberInfo::bigIntegerToStringWithBase(const BigInteger &value, int base)
{
    std::string result;
    if (value.getSign() == BigInteger::negative) {
        result = "-";
        result += std::string(BigUnsignedInABase(value.getMagnitude(), base));
    } else {
        result = std::string(BigUnsignedInABase(value.getMagnitude(), base));
    }
    return result;
}

BigInteger QSocNumberInfo::stringToBigIntegerWithBase(const std::string &str, int base)
{
    BigUnsigned result(0);
    BigUnsigned baseVal(base);

    for (char c : str) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            /* Skip invalid characters */
            continue;
        }

        if (digit >= base) {
            /* Skip invalid digits for this base */
            continue;
        }

        result = result * baseVal + BigUnsigned(digit);
    }

    return BigInteger(result);
}

QString QSocNumberInfo::format() const
{
    /* If error was detected, return original string */
    if (errorDetected) {
        return originalString;
    }

    switch (base) {
    case Base::Binary:
        return QString("'b%1").arg(QString::fromStdString(bigIntegerToStringWithBase(value, 2)));
    case Base::Octal:
        return QString("'o%1").arg(QString::fromStdString(bigIntegerToStringWithBase(value, 8)));
    case Base::Decimal:
        return QString("'d%1").arg(QString::fromStdString(bigIntegerToStringWithBase(value, 10)));
    case Base::Hexadecimal: {
        QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
        /* Always use lowercase for hex values regardless of original casing */
        return QString("'h%1").arg(hexStr.toLower());
    }
    default:
        return QString::fromStdString(bigIntegerToStringWithBase(value, 10));
    }
}

QString QSocNumberInfo::formatVerilog() const
{
    /* If error was detected, use the original string to preserve large values */
    if (errorDetected) {
        return originalString;
    }

    if (width > 0) {
        return QString("%1%2").arg(width).arg(format());
    } else {
        return format();
    }
}

QString QSocNumberInfo::formatC() const
{
    /* If error was detected, return original string */
    if (errorDetected) {
        return originalString;
    }

    switch (base) {
    case Base::Binary:
        return QString("0b%1").arg(QString::fromStdString(bigIntegerToStringWithBase(value, 2)));
    case Base::Octal:
        return QString("0%1").arg(QString::fromStdString(bigIntegerToStringWithBase(value, 8)));
    case Base::Hexadecimal: {
        QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
        /* Always use lowercase for hex values regardless of original casing */
        return QString("0x%1").arg(hexStr.toLower());
    }
    default:
        return QString::fromStdString(bigIntegerToStringWithBase(value, 10));
    }
}

QString QSocNumberInfo::formatVerilogProperWidth() const
{
    /* If error was detected, return original string */
    if (errorDetected) {
        return originalString;
    }

    QString result;

    switch (base) {
    case Base::Binary: {
        std::string binStr = bigIntegerToStringWithBase(value, 2);
        result             = QString::fromStdString(binStr).rightJustified(width, '0');
        return QString("%1'b%2").arg(width).arg(result);
    }
    case Base::Octal: {
        /* Calculate how many octal digits are needed */
        int         octalDigits = (width + 2) / 3; /* Ceiling division */
        std::string octStr      = bigIntegerToStringWithBase(value, 8);
        result                  = QString::fromStdString(octStr).rightJustified(octalDigits, '0');
        return QString("%1'o%2").arg(width).arg(result);
    }
    case Base::Hexadecimal: {
        /* Calculate how many hex digits are needed */
        int         hexDigits = (width + 3) / 4; /* Ceiling division */
        std::string hexStr    = bigIntegerToStringWithBase(value, 16);
        /* Always use lowercase for hex values */
        result = QString::fromStdString(hexStr).rightJustified(hexDigits, '0').toLower();
        return QString("%1'h%2").arg(width).arg(result);
    }
    case Base::Decimal:
    default:
        return QString("%1'd%2").arg(width).arg(
            QString::fromStdString(bigIntegerToStringWithBase(value, 10)));
    }
}
