// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocnumberinfo.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <limits>

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
    BigUnsigned       result(0);
    const BigUnsigned baseVal(base);

    for (const char character : str) {
        int digit;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
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

    return {result};
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
        const QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
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
    }

    return format();
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
        const QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
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
        const std::string binStr = bigIntegerToStringWithBase(value, 2);
        result                   = QString::fromStdString(binStr).rightJustified(width, '0');
        return QString("%1'b%2").arg(width).arg(result);
    }
    case Base::Octal: {
        /* Calculate how many octal digits are needed */
        const int         octalDigits = (width + 2) / 3; /* Ceiling division */
        const std::string octStr      = bigIntegerToStringWithBase(value, 8);
        result = QString::fromStdString(octStr).rightJustified(octalDigits, '0');
        return QString("%1'o%2").arg(width).arg(result);
    }
    case Base::Hexadecimal: {
        /* Calculate how many hex digits are needed */
        const int         hexDigits = (width + 3) / 4; /* Ceiling division */
        const std::string hexStr    = bigIntegerToStringWithBase(value, 16);
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

QSocNumberInfo QSocNumberInfo::parseNumber(const QString &numStr)
{
    QSocNumberInfo result;
    result.originalString   = numStr;
    result.base             = QSocNumberInfo::Base::Unknown;
    result.value            = 0;
    result.width            = 0;
    result.hasExplicitWidth = false;
    result.errorDetected    = false;

    /* Remove all underscores from the string (Verilog style) */
    QString cleanStr = numStr;
    cleanStr.remove('_');

    if (cleanStr.isEmpty()) {
        qWarning() << "Empty number string";
        return result;
    }

    /* Check for Verilog-style format with vector range: [31:0] */
    const QRegularExpression      vectorWidthRegex(R"(\[(\d+)\s*:\s*(\d+)\])");
    const QRegularExpressionMatch vectorWidthMatch = vectorWidthRegex.match(cleanStr);

    if (vectorWidthMatch.hasMatch()) {
        bool      msb_ok = false;
        bool      lsb_ok = false;
        const int msb    = vectorWidthMatch.captured(1).toInt(&msb_ok);
        const int lsb    = vectorWidthMatch.captured(2).toInt(&lsb_ok);

        if (msb_ok && lsb_ok) {
            result.width            = msb - lsb + 1;
            result.hasExplicitWidth = true;

            /* Remove the vector range from the string for further processing */
            cleanStr.remove(vectorWidthRegex);
        }
    }

    /* Check for Verilog-style format: <width>'<base><value> */
    const QRegularExpression      verilogNumberRegex(R"((\d+)'([bdohxBDOHX])([0-9a-fA-F]+))");
    const QRegularExpressionMatch verilogMatch = verilogNumberRegex.match(cleanStr);

    if (verilogMatch.hasMatch()) {
        /* Extract width, base, and value from the Verilog format */
        bool      widthOk = false;
        const int width   = verilogMatch.captured(1).toInt(&widthOk);

        if (widthOk && !result.hasExplicitWidth) {
            result.width            = width;
            result.hasExplicitWidth = true;
        }

        const QChar   baseChar = verilogMatch.captured(2).at(0).toLower();
        const QString valueStr = verilogMatch.captured(3);

        /* Determine the base from the base character */
        switch (baseChar.toLatin1()) {
        case 'b': /* Binary */
            result.base = QSocNumberInfo::Base::Binary;
            try {
                result.value = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 2);
            } catch (const std::exception &e) {
                result.errorDetected = true;
                qWarning() << "Binary value error, using original string:" << numStr
                           << "Error:" << e.what();
            }
            break;
        case 'o': /* Octal */
            result.base = QSocNumberInfo::Base::Octal;
            try {
                result.value = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 8);
            } catch (const std::exception &e) {
                result.errorDetected = true;
                qWarning() << "Octal value error, using original string:" << numStr
                           << "Error:" << e.what();
            }
            break;
        case 'd': /* Decimal */
            result.base = QSocNumberInfo::Base::Decimal;
            try {
                result.value
                    = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 10);
            } catch (const std::exception &e) {
                result.errorDetected = true;
                qWarning() << "Decimal value error, using original string:" << numStr
                           << "Error:" << e.what();
            }
            break;
        case 'h': /* Hexadecimal */
        case 'x': /* Alternative for Hexadecimal */
            result.base = QSocNumberInfo::Base::Hexadecimal;
            try {
                result.value
                    = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 16);
            } catch (const std::exception &e) {
                result.errorDetected = true;
                qWarning() << "Hexadecimal value error, using original string:" << numStr
                           << "Error:" << e.what();
            }
            break;
        default:
            qWarning() << "Unknown base character in Verilog number:" << baseChar;
        }
    } else {
        /* Handle standalone Verilog-style base prefixes (without width): 'b, 'h, 'o, 'd */
        const QRegularExpression      verilogBaseRegex(R"('([bdohxBDOHX])([0-9a-fA-F]+))");
        const QRegularExpressionMatch verilogBaseMatch = verilogBaseRegex.match(cleanStr);

        if (verilogBaseMatch.hasMatch()) {
            const QChar   baseChar = verilogBaseMatch.captured(1).at(0).toLower();
            const QString valueStr = verilogBaseMatch.captured(2);

            /* Determine the base from the base character */
            switch (baseChar.toLatin1()) {
            case 'b': /* Binary */
                result.base = QSocNumberInfo::Base::Binary;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 2);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Binary value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
                break;
            case 'o': /* Octal */
                result.base = QSocNumberInfo::Base::Octal;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 8);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Octal value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
                break;
            case 'd': /* Decimal */
                result.base = QSocNumberInfo::Base::Decimal;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 10);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Decimal value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
                break;
            case 'h': /* Hexadecimal */
            case 'x': /* Alternative for Hexadecimal */
                result.base = QSocNumberInfo::Base::Hexadecimal;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 16);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Hexadecimal value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
                break;
            default:
                qWarning() << "Unknown base character in Verilog number:" << baseChar;
            }
        } else {
            /* Try C-style format */
            if (cleanStr.startsWith("0x") || cleanStr.startsWith("0X")) {
                /* Hexadecimal */
                result.base = QSocNumberInfo::Base::Hexadecimal;
                try {
                    result.value = QSocNumberInfo::stringToBigIntegerWithBase(
                        cleanStr.mid(2).toStdString(), 16);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Hexadecimal value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
            } else if (cleanStr.startsWith("0b") || cleanStr.startsWith("0B")) {
                /* Binary (C++14 style) */
                result.base = QSocNumberInfo::Base::Binary;
                try {
                    result.value = QSocNumberInfo::stringToBigIntegerWithBase(
                        cleanStr.mid(2).toStdString(), 2);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Binary value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
            } else if (cleanStr.startsWith("0") && cleanStr.length() > 1) {
                /* Octal */
                result.base = QSocNumberInfo::Base::Octal;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(cleanStr.toStdString(), 8);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Octal value error, using original string:" << numStr
                               << "Error:" << e.what();
                }
            } else {
                /* Decimal */
                result.base = QSocNumberInfo::Base::Decimal;
                try {
                    result.value
                        = QSocNumberInfo::stringToBigIntegerWithBase(cleanStr.toStdString(), 10);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    qWarning() << "Failed to parse decimal number, using original string:"
                               << cleanStr << "Error:" << e.what();
                }
            }
        }
    }

    /* Calculate width if not explicitly provided */
    if (!result.hasExplicitWidth) {
        if (result.errorDetected) {
            /* For error values, set a reasonable width based on the original string */
            if (result.originalString.toLower().contains('h')) {
                /* Hex values: each digit is 4 bits */
                const int digits = static_cast<int>(result.originalString.length());
                /* Rough estimate, removing prefix parts */
                result.width = (digits - 3) * 4; /* Assuming format like "N'h..." */
            } else if (result.originalString.toLower().contains('b')) {
                /* Binary values: each digit is 1 bit */
                const int digits = static_cast<int>(result.originalString.length());
                /* Rough estimate, removing prefix parts */
                result.width = digits - 3; /* Assuming format like "N'b..." */
            } else if (result.originalString.toLower().contains('o')) {
                /* Octal values: each digit is 3 bits */
                const int digits = static_cast<int>(result.originalString.length());
                /* Rough estimate, removing prefix parts */
                result.width = (digits - 3) * 3; /* Assuming format like "N'o..." */
            } else {
                /* Decimal values */
                if (result.originalString.length() > 20) {
                    result.width = 128; /* Very large numbers */
                } else if (result.originalString.length() > 10) {
                    result.width = 64; /* Medium large numbers */
                } else {
                    result.width = 32; /* Regular numbers */
                }
            }
        } else if (result.value == 0) {
            result.width = 1; /* Special case for zero */
        } else {
            /* Calculate minimum required width based on the value */
            BigInteger tempValue       = result.value;
            int        calculatedWidth = 0;

            /* Count how many bits are needed */
            while (tempValue != 0) {
                /* Shift right by one bit */
                if (tempValue.getSign() == BigInteger::negative) {
                    BigUnsigned magnitude = tempValue.getMagnitude();
                    magnitude             = magnitude >> 1;
                    tempValue             = BigInteger(magnitude, BigInteger::negative);
                } else {
                    BigUnsigned magnitude = tempValue.getMagnitude();
                    magnitude             = magnitude >> 1;
                    tempValue             = BigInteger(magnitude);
                }
                calculatedWidth++;
            }

            /* Use exact calculated width */
            result.width = calculatedWidth;
        }
    }

    return result;
}

int64_t QSocNumberInfo::toInt64() const
{
    if (errorDetected) {
        return 0;
    }

    try {
        /* Convert BigInteger to string, then to int64_t */
        const std::string valueStr = bigIntegerToStringWithBase(value, 10);

        /* Check if the value is negative */
        if (value.getSign() == BigInteger::negative) {
            /* For negative values, we need to handle the conversion carefully */
            const BigUnsigned magnitude = value.getMagnitude();

            /* Check if magnitude fits in int64_t range */
            if (magnitude > BigUnsigned(std::numeric_limits<int64_t>::max())) {
                qWarning() << "Value too large for int64_t conversion:" << originalString;
                return 0;
            }

            /* Convert magnitude to string and then to int64_t, then negate */
            const std::string magnitudeStr = std::string(BigUnsignedInABase(magnitude, 10));
            const int64_t     result       = -static_cast<int64_t>(std::stoull(magnitudeStr));
            return result;
        } else {
            /* For positive values */
            const BigUnsigned magnitude = value.getMagnitude();

            /* Check if magnitude fits in int64_t range */
            if (magnitude > BigUnsigned(std::numeric_limits<int64_t>::max())) {
                qWarning() << "Value too large for int64_t conversion:" << originalString;
                return 0;
            }

            /* Convert magnitude to string and then to int64_t */
            const std::string magnitudeStr = std::string(BigUnsignedInABase(magnitude, 10));
            const int64_t     result       = static_cast<int64_t>(std::stoull(magnitudeStr));
            return result;
        }
    } catch (const std::exception &e) {
        qWarning() << "Error converting to int64_t:" << originalString << "Error:" << e.what();
        return 0;
    }
}
