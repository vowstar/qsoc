// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiwidget.h"

namespace QTuiText {

bool isWideChar(uint code)
{
    return (code >= 0x1100 && code <= 0x115F) || code == 0x2329 || code == 0x232A
           || (code >= 0x2E80 && code <= 0x303E) || (code >= 0x3040 && code <= 0x33BF)
           || (code >= 0x3400 && code <= 0x4DBF) || (code >= 0x4E00 && code <= 0x9FFF)
           || (code >= 0xA000 && code <= 0xA4CF) || (code >= 0xAC00 && code <= 0xD7AF)
           || (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFE10 && code <= 0xFE19)
           || (code >= 0xFE30 && code <= 0xFE6F) || (code >= 0xFF01 && code <= 0xFF60)
           || (code >= 0xFFE0 && code <= 0xFFE6) || (code >= 0x20000 && code <= 0x2FFFF)
           || (code >= 0x30000 && code <= 0x3FFFF);
}

int visualWidth(const QString &text)
{
    int width = 0;
    int idx   = 0;
    int len   = text.length();

    while (idx < len) {
        QChar ch = text[idx];

        /* Skip ANSI CSI sequences: ESC [ ... final_byte(0x40-0x7E) */
        if (ch == '\033' && idx + 1 < len && text[idx + 1] == '[') {
            idx += 2;
            while (idx < len) {
                ushort code = text[idx].unicode();
                ++idx;
                if (code >= 0x40 && code <= 0x7E) {
                    break;
                }
            }
            continue;
        }

        /* Skip other ESC sequences (ESC + single char) */
        if (ch == '\033') {
            idx += 2;
            continue;
        }

        /* Handle surrogate pairs */
        uint codePoint;
        int  charLen;
        if (ch.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(ch, text[idx + 1]);
            charLen   = 2;
        } else {
            codePoint = ch.unicode();
            charLen   = 1;
        }

        /* Control characters have zero width */
        if (codePoint < 0x20 || (codePoint >= 0x7F && codePoint < 0xA0)) {
            idx += charLen;
            continue;
        }

        width += isWideChar(codePoint) ? 2 : 1;
        idx += charLen;
    }

    return width;
}

QString truncate(const QString &text, int maxWidth)
{
    if (visualWidth(text) <= maxWidth) {
        return text;
    }

    int targetWidth = maxWidth - 3;
    if (targetWidth <= 0) {
        return "...";
    }

    int width = 0;
    int idx   = 0;
    int len   = text.length();

    while (idx < len) {
        QChar ch = text[idx];

        /* Preserve ANSI CSI sequences */
        if (ch == '\033' && idx + 1 < len && text[idx + 1] == '[') {
            idx += 2;
            while (idx < len) {
                ushort code = text[idx].unicode();
                ++idx;
                if (code >= 0x40 && code <= 0x7E) {
                    break;
                }
            }
            continue;
        }

        if (ch == '\033') {
            idx += 2;
            continue;
        }

        uint codePoint;
        int  charLen;
        if (ch.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(ch, text[idx + 1]);
            charLen   = 2;
        } else {
            codePoint = ch.unicode();
            charLen   = 1;
        }

        if (codePoint < 0x20 || (codePoint >= 0x7F && codePoint < 0xA0)) {
            idx += charLen;
            continue;
        }

        int charWidth = isWideChar(codePoint) ? 2 : 1;
        if (width + charWidth > targetWidth) {
            break;
        }
        width += charWidth;
        idx += charLen;
    }

    return text.left(idx) + "...";
}

QString formatNumber(qint64 value)
{
    if (value < 1000) {
        return QString::number(value);
    }
    if (value < 1000000) {
        double kilo = value / 1000.0;
        return (kilo < 10) ? QString::number(kilo, 'f', 1) + "k"
                           : QString::number(static_cast<int>(kilo)) + "k";
    }
    if (value < 1000000000) {
        double mega = value / 1000000.0;
        return (mega < 10) ? QString::number(mega, 'f', 1) + "M"
                           : QString::number(static_cast<int>(mega)) + "M";
    }
    double giga = value / 1000000000.0;
    return (giga < 10) ? QString::number(giga, 'f', 1) + "G"
                       : QString::number(static_cast<int>(giga)) + "G";
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 60) {
        return QString::number(seconds) + "s";
    }
    if (seconds < 3600) {
        int min = static_cast<int>(seconds / 60);
        int sec = static_cast<int>(seconds % 60);
        return QString("%1:%2").arg(min).arg(sec, 2, 10, QChar('0'));
    }
    if (seconds < 86400) {
        int hrs = static_cast<int>(seconds / 3600);
        int min = static_cast<int>((seconds % 3600) / 60);
        int sec = static_cast<int>(seconds % 60);
        return QString("%1:%2:%3").arg(hrs).arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    }
    int days = static_cast<int>(seconds / 86400);
    int hrs  = static_cast<int>((seconds % 86400) / 3600);
    int min  = static_cast<int>((seconds % 3600) / 60);
    int sec  = static_cast<int>(seconds % 60);
    return QString("%1d%2:%3:%4")
        .arg(days)
        .arg(hrs)
        .arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'));
}

} // namespace QTuiText
