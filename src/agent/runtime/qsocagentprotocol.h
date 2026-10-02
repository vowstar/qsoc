// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#ifndef QSOCAGENTPROTOCOL_H
#define QSOCAGENTPROTOCOL_H

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace QSocAgentProtocol {
inline constexpr int version         = 1;
inline constexpr int headerBytes     = 8;
inline constexpr int maxPayloadBytes = 16 * 1024 * 1024;

inline QByteArray frame(const QByteArray &payload)
{
    return QByteArray::number(payload.size(), 16).rightJustified(headerBytes, '0') + payload;
}
inline QByteArray frame(const QJsonObject &object)
{
    return frame(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
// Zero means incomplete; -1 means invalid. Never add an unchecked wire length.
inline int payloadLength(const QByteArray &buffer)
{
    if (buffer.size() < headerBytes)
        return 0;
    for (char ch : buffer.first(headerBytes)) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return -1;
    }
    bool       ok     = false;
    const uint length = buffer.first(headerBytes).toUInt(&ok, 16);
    return ok && length > 0 && length <= maxPayloadBytes ? static_cast<int>(length) : -1;
}
} // namespace QSocAgentProtocol
#endif
