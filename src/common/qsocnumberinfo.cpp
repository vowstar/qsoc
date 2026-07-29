// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocnumberinfo.h"
#include "common/qsocconsole.h"

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
        QSocConsole::warn() << "Empty number string";
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
                QSocConsole::warn() << "Binary value error, using original string:" << numStr
                                    << "Error:" << e.what();
            }
            break;
        case 'o': /* Octal */
            result.base = QSocNumberInfo::Base::Octal;
            try {
                result.value = QSocNumberInfo::stringToBigIntegerWithBase(valueStr.toStdString(), 8);
            } catch (const std::exception &e) {
                result.errorDetected = true;
                QSocConsole::warn() << "Octal value error, using original string:" << numStr
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
                QSocConsole::warn() << "Decimal value error, using original string:" << numStr
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
                QSocConsole::warn() << "Hexadecimal value error, using original string:" << numStr
                                    << "Error:" << e.what();
            }
            break;
        default:
            QSocConsole::warn() << "Unknown base character in Verilog number:" << baseChar;
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
                    QSocConsole::warn() << "Binary value error, using original string:" << numStr
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
                    QSocConsole::warn() << "Octal value error, using original string:" << numStr
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
                    QSocConsole::warn() << "Decimal value error, using original string:" << numStr
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
                    QSocConsole::warn()
                        << "Hexadecimal value error, using original string:" << numStr
                        << "Error:" << e.what();
                }
                break;
            default:
                QSocConsole::warn() << "Unknown base character in Verilog number:" << baseChar;
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
                    QSocConsole::warn()
                        << "Hexadecimal value error, using original string:" << numStr
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
                    QSocConsole::warn() << "Binary value error, using original string:" << numStr
                                        << "Error:" << e.what();
                }
            } else if (cleanStr.startsWith("0o") || cleanStr.startsWith("0O")) {
                /* Octal with explicit prefix (Python/Rust/YAML 1.2 style) */
                result.base = QSocNumberInfo::Base::Octal;
                try {
                    result.value = QSocNumberInfo::stringToBigIntegerWithBase(
                        cleanStr.mid(2).toStdString(), 8);
                } catch (const std::exception &e) {
                    result.errorDetected = true;
                    QSocConsole::warn() << "Octal value error, using original string:" << numStr
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
                    QSocConsole::warn() << "Octal value error, using original string:" << numStr
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
                    QSocConsole::warn()
                        << "Failed to parse decimal number, using original string:" << cleanStr
                        << "Error:" << e.what();
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

namespace {

bool isAsciiDigit(QChar character)
{
    return character >= '0' && character <= '9';
}

bool isNumericExpressionBoundary(QChar character)
{
    if (character.isSpace() || character == '`') {
        return true;
    }

    switch (character.toLatin1()) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '&':
    case '|':
    case '^':
    case '~':
    case '!':
    case '<':
    case '>':
    case '=':
    case '?':
    case ':':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case ',':
    case '\'':
        return true;
    default:
        return false;
    }
}

bool startsComment(const QString &text, qsizetype position)
{
    return position + 1 < text.size() && text.at(position) == '/'
           && (text.at(position + 1) == '/' || text.at(position + 1) == '*');
}

bool skipNumericTrivia(const QString &text, qsizetype &position)
{
    while (position < text.size()) {
        while (position < text.size() && text.at(position).isSpace()) {
            ++position;
        }
        if (!startsComment(text, position)) {
            return true;
        }
        if (text.at(position + 1) == '/') {
            position += 2;
            while (position < text.size() && text.at(position) != '\n'
                   && text.at(position) != '\r') {
                ++position;
            }
            if (position >= text.size()) {
                return false;
            }
            ++position;
            continue;
        }
        const qsizetype commentEnd = text.indexOf(QStringLiteral("*/"), position + 2);
        if (commentEnd < 0) {
            return false;
        }
        position = commentEnd + 2;
    }
    return true;
}

qsizetype commentAfterWhitespace(const QString &text, qsizetype position)
{
    while (position < text.size() && text.at(position).isSpace()) {
        ++position;
    }
    return startsComment(text, position) ? position : -1;
}

bool startsCastBody(const QString &text, qsizetype position, bool allowAssignmentPattern)
{
    if (!skipNumericTrivia(text, position)) {
        return false;
    }
    if (position < text.size() && text.at(position) == '`') {
        return true;
    }
    return position < text.size()
           && (text.at(position) == '(' || (allowAssignmentPattern && text.at(position) == '{'));
}

int basedDigitKind(QChar character, QChar base)
{
    const QChar lower = character.toLower();
    if (lower == 'x' || lower == 'z' || character == '?') {
        return 1;
    }

    if (isAsciiDigit(character)) {
        const int digit = character.unicode() - QChar('0').unicode();
        const int limit = base == 'b' ? 2 : base == 'o' ? 8 : 10;
        return digit < limit || base == 'h' || base == 'x' ? 0 : -1;
    }
    return (base == 'h' || base == 'x') && lower >= 'a' && lower <= 'f' ? 0 : -1;
}

qsizetype timeUnitEnd(const QString &text, qsizetype position)
{
    if (position >= text.size()) {
        return -1;
    }
    if (text.at(position) == 's') {
        return position + 1;
    }
    if (position + 1 < text.size() && text.at(position + 1) == 's') {
        switch (text.at(position).toLatin1()) {
        case 'm':
        case 'u':
        case 'n':
        case 'p':
        case 'f':
            return position + 2;
        default:
            break;
        }
    }
    return -1;
}

} // namespace

QSocNumberInfo::NumericText QSocNumberInfo::classifyNumericText(const QString &text)
{
    const NumericText pass{NumericTextKind::PassThrough, Spelling::NotANumber};
    const NumericText reject{NumericTextKind::Reject, Spelling::NotANumber};
    if (text.isEmpty()) {
        return pass;
    }

    if (text.at(0) == '_') {
        qsizetype position = 0;
        while (position < text.size()
               && (text.at(position) == '_' || isAsciiDigit(text.at(position)))) {
            ++position;
        }
        while (position < text.size() && text.at(position).isSpace()) {
            ++position;
        }
        if (position >= text.size() || text.at(position) != '\'') {
            return pass;
        }
        return startsCastBody(text, position + 1, true) ? pass : reject;
    }

    if (text.at(0) == '.') {
        return reject;
    }
    if (!isAsciiDigit(text.at(0)) && text.at(0) != '\'') {
        return pass;
    }

    bool      exponent     = false;
    bool      real         = false;
    qsizetype position     = 0;
    bool      sawDigit     = false;
    bool      allOctal     = true;
    bool      widthTooWide = false;
    int       width        = 0;

    if (text.at(0) != '\'') {
        while (position < text.size()
               && (isAsciiDigit(text.at(position)) || text.at(position) == '_')) {
            const QChar character = text.at(position++);
            if (character == '_') {
                continue;
            }
            sawDigit = true;
            allOctal &= character <= '7';
            const int digit = character.unicode() - QChar('0').unicode();
            if (width > (MaximumDeclaredWidth - digit) / 10) {
                widthTooWide = true;
            } else if (!widthTooWide) {
                width = width * 10 + digit;
            }
        }
    }

    const qsizetype numberEnd = position;
    const qsizetype sizeEnd   = position;
    if (!skipNumericTrivia(text, position)) {
        return reject;
    }
    const bool sizeGap = position != sizeEnd;

    if (position < text.size() && text.at(position) == '\'') {
        if ((numberEnd > 0 && !sawDigit) || widthTooWide || (sawDigit && width == 0)) {
            return reject;
        }

        ++position;
        if (sawDigit && startsCastBody(text, position, false)) {
            return pass;
        }
        if (!sawDigit && position < text.size() && text.at(position) == '{') {
            return pass;
        }
        if (!sawDigit && position < text.size()) {
            const QChar unbased = text.at(position).toLower();
            if (unbased == '0' || unbased == '1' || unbased == 'x' || unbased == 'z') {
                qsizetype       suffixPosition  = position + 1;
                const qsizetype commentPosition = commentAfterWhitespace(text, suffixPosition);
                if (commentPosition >= 0) {
                    suffixPosition = commentPosition;
                    return skipNumericTrivia(text, suffixPosition) ? pass : reject;
                }
                if (suffixPosition == text.size()
                    || isNumericExpressionBoundary(text.at(suffixPosition))) {
                    return pass;
                }
            }
        }

        bool isSigned = false;
        if (position < text.size() && text.at(position).toLower() == 's') {
            isSigned = true;
            ++position;
        }
        if (position < text.size() && text.at(position) == '`') {
            return pass;
        }
        if (position >= text.size()) {
            return reject;
        }

        const QChar base = text.at(position).toLower();
        if (base != 'b' && base != 'o' && base != 'd' && base != 'h' && base != 'x') {
            return reject;
        }
        ++position;

        const qsizetype valueStart = position;
        if (!skipNumericTrivia(text, position)) {
            return reject;
        }
        const bool valueGap = position != valueStart;
        if (position < text.size() && text.at(position) == '`') {
            return pass;
        }
        if (position >= text.size() || text.at(position) == '_'
            || basedDigitKind(text.at(position), base) < 0) {
            return reject;
        }

        bool      fourState  = false;
        qsizetype valueCount = 0;
        while (position < text.size()) {
            const QChar character = text.at(position);
            if (character == '_') {
                ++position;
                continue;
            }
            const int kind = basedDigitKind(character, base);
            if (kind < 0) {
                break;
            }
            ++valueCount;
            fourState |= kind > 0;
            ++position;
        }
        if (base == 'd' && fourState && valueCount != 1) {
            return reject;
        }

        const qsizetype commentPosition = commentAfterWhitespace(text, position);
        if (commentPosition >= 0) {
            position = commentPosition;
            if (!skipNumericTrivia(text, position)) {
                return reject;
            }
            if (position >= text.size()) {
                return pass;
            }
        }
        if (position < text.size()) {
            return isNumericExpressionBoundary(text.at(position)) ? pass : reject;
        }
        if (isSigned || fourState || sizeGap || valueGap) {
            return pass;
        }
        if (text.size() > MaximumNumericCharacters) {
            return reject;
        }
        return {NumericTextKind::Normalize, Spelling::Verilog};
    }

    if (text.at(0) == '\'' || !sawDigit) {
        return numberEnd > 0 && sizeGap ? pass : reject;
    }
    if (sizeGap) {
        return pass;
    }

    if (numberEnd == 1 && text.at(0) == '0' && position < text.size()) {
        const QChar prefix = text.at(position).toLower();
        if (prefix == 'x' || prefix == 'b' || prefix == 'o') {
            ++position;
            const QChar base = prefix == 'x' ? QChar('h') : prefix;
            if (position >= text.size() || text.at(position) == '_'
                || basedDigitKind(text.at(position), base) != 0) {
                return reject;
            }
            while (position < text.size()) {
                const QChar character = text.at(position);
                if (character == '_' || basedDigitKind(character, base) == 0) {
                    ++position;
                    continue;
                }
                break;
            }
            if (position < text.size()) {
                return reject;
            }
            return text.size() > MaximumNumericCharacters
                       ? reject
                       : NumericText{NumericTextKind::Normalize, Spelling::CStyle};
        }
    }

    if (position < text.size() && text.at(position) == '.') {
        real = true;
        ++position;
        if (position < text.size() && text.at(position) == '`') {
            return pass;
        }
        if (position >= text.size() || !isAsciiDigit(text.at(position))) {
            return reject;
        }
        while (position < text.size()
               && (isAsciiDigit(text.at(position)) || text.at(position) == '_')) {
            ++position;
        }
    }
    if (position < text.size() && text.at(position).toLower() == 'e') {
        exponent = true;
        real     = true;
        ++position;
        if (position < text.size() && (text.at(position) == '+' || text.at(position) == '-')) {
            ++position;
        }
        if (position < text.size() && text.at(position) == '`') {
            return pass;
        }
        if (position >= text.size() || !isAsciiDigit(text.at(position))) {
            return reject;
        }
        while (position < text.size()
               && (isAsciiDigit(text.at(position)) || text.at(position) == '_')) {
            ++position;
        }
    }

    const qsizetype unitEnd = timeUnitEnd(text, position);
    if (unitEnd >= 0) {
        if (exponent) {
            return reject;
        }
        position = unitEnd;
        real     = true;
    }
    const qsizetype commentPosition = commentAfterWhitespace(text, position);
    if (commentPosition >= 0) {
        position = commentPosition;
        if (!skipNumericTrivia(text, position)) {
            return reject;
        }
        if (position >= text.size()) {
            return pass;
        }
    }
    if (position < text.size()) {
        return isNumericExpressionBoundary(text.at(position)) ? pass : reject;
    }
    if (real) {
        return pass;
    }
    if (text.size() > MaximumNumericCharacters) {
        return reject;
    }
    if (text.at(0) == '0' && text.size() > 1) {
        return allOctal ? NumericText{NumericTextKind::Normalize, Spelling::CStyle} : reject;
    }
    return {NumericTextKind::Normalize, Spelling::PlainDecimal};
}

QString QSocNumberInfo::normalizeHexBaseAliases(const QString &text)
{
    QString   result      = text;
    qsizetype position    = 0;
    bool      decimalSize = false;

    while (position < text.size()) {
        const QChar character = text.at(position);

        if (character.isSpace()) {
            ++position;
            continue;
        }

        if (startsComment(text, position)) {
            if (text.at(position + 1) == '/') {
                position += 2;
                while (position < text.size() && text.at(position) != '\n'
                       && text.at(position) != '\r') {
                    ++position;
                }
            } else {
                const qsizetype commentEnd = text.indexOf(QStringLiteral("*/"), position + 2);
                position                   = commentEnd < 0 ? text.size() : commentEnd + 2;
            }
            continue;
        }

        if (character == '"') {
            decimalSize = false;
            ++position;
            while (position < text.size()) {
                if (text.at(position) == '\\' && position + 1 < text.size()) {
                    position += 2;
                } else if (text.at(position++) == '"') {
                    break;
                }
            }
            continue;
        }

        if (character == '\\') {
            decimalSize = false;
            while (position < text.size() && !text.at(position).isSpace()) {
                ++position;
            }
            continue;
        }

        if (isAsciiDigit(character)) {
            do {
                ++position;
            } while (position < text.size()
                     && (isAsciiDigit(text.at(position)) || text.at(position) == '_'));
            decimalSize = true;
            continue;
        }

        if (character.isLetter() || character == '_' || character == '$') {
            decimalSize = false;
            do {
                ++position;
            } while (position < text.size()
                     && (text.at(position).isLetterOrNumber() || text.at(position) == '_'
                         || text.at(position) == '$'));
            continue;
        }

        if (character == '\'') {
            qsizetype basePosition = position + 1;
            bool      isSigned     = false;
            if (basePosition < text.size() && text.at(basePosition).toLower() == 's') {
                isSigned = true;
                ++basePosition;
            }
            if (basePosition < text.size() && text.at(basePosition).toLower() == 'x') {
                qsizetype valuePosition = basePosition + 1;
                if (skipNumericTrivia(text, valuePosition) && valuePosition < text.size()) {
                    const QChar valueStart = text.at(valuePosition);
                    const bool  hasValue   = basedDigitKind(valueStart, 'h') >= 0;
                    const bool  unbasedX   = !decimalSize && !isSigned && valueStart == '?';
                    if (hasValue && !unbasedX) {
                        result[basePosition] = text.at(basePosition) == 'x' ? QChar('h')
                                                                            : QChar('H');
                    }
                }
            }
            decimalSize = false;
            ++position;
            continue;
        }

        decimalSize = false;
        ++position;
    }

    return result;
}

QSocNumberInfo::Spelling QSocNumberInfo::classifyTwoStateNumber(const QString &numStr)
{
    if (numStr.size() > MaximumNumericCharacters) {
        return Spelling::NotANumber;
    }
    const NumericText numeric = classifyNumericText(numStr);
    return numeric.kind == NumericTextKind::Normalize ? numeric.spelling : Spelling::NotANumber;
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
                QSocConsole::warn() << "Value too large for int64_t conversion:" << originalString;
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
                QSocConsole::warn() << "Value too large for int64_t conversion:" << originalString;
                return 0;
            }

            /* Convert magnitude to string and then to int64_t */
            const std::string magnitudeStr = std::string(BigUnsignedInABase(magnitude, 10));
            const int64_t     result       = static_cast<int64_t>(std::stoull(magnitudeStr));
            return result;
        }
    } catch (const std::exception &e) {
        QSocConsole::warn() << "failed to convert to int64_t:" << originalString
                            << "Error:" << e.what();
        return 0;
    }
}
