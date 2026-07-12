// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPSTDIOPARSER_P_H
#define QSOCMCPSTDIOPARSER_P_H

#include <QByteArray>

#include <cstdint>

namespace QSocMcpStdioInternal {

enum class LineStatus : std::uint8_t { NeedMore, Message, TooLarge, Unterminated };

struct LineResult
{
    LineStatus status = LineStatus::NeedMore;
    QByteArray message;
};

inline LineResult takeLine(
    QByteArray &buffer, qsizetype &scanOffset, qsizetype maximumBytes, bool atEnd)
{
    const qsizetype lineEnd = buffer.indexOf('\n', scanOffset);
    if (lineEnd < 0) {
        scanOffset                = buffer.size();
        const bool      pendingCr = buffer.endsWith('\r');
        const qsizetype size      = buffer.size() - (pendingCr ? 1 : 0);
        if (size > maximumBytes) {
            return {LineStatus::TooLarge, {}};
        }
        if (atEnd && !buffer.isEmpty()) {
            return {LineStatus::Unterminated, {}};
        }
        return {};
    }

    const bool      hasCr = lineEnd > 0 && buffer.at(lineEnd - 1) == '\r';
    const qsizetype size  = lineEnd - (hasCr ? 1 : 0);
    if (size > maximumBytes) {
        return {LineStatus::TooLarge, {}};
    }

    LineResult result{LineStatus::Message, buffer.left(size)};
    buffer.remove(0, lineEnd + 1);
    scanOffset = 0;
    return result;
}

} // namespace QSocMcpStdioInternal

#endif // QSOCMCPSTDIOPARSER_P_H
