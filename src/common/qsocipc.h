// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIPC_H
#define QSOCIPC_H

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace QSocIpc {

inline constexpr int headerBytes     = 8;
inline constexpr int maxPayloadBytes = 16 * 1024 * 1024;

enum class DecodeResult { Incomplete, Complete, Invalid };

inline QByteArray frame(const QByteArray &payload, int limit = maxPayloadBytes)
{
    if (payload.isEmpty() || payload.size() > limit)
        return {};
    return QByteArray::number(payload.size(), 16).rightJustified(headerBytes, '0') + payload;
}

inline QByteArray frame(const QJsonObject &object, int limit = maxPayloadBytes)
{
    return frame(QJsonDocument(object).toJson(QJsonDocument::Compact), limit);
}

inline int payloadLength(const QByteArray &buffer, int limit = maxPayloadBytes)
{
    if (buffer.size() < headerBytes)
        return 0;
    for (char ch : buffer.first(headerBytes)) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return -1;
    }
    bool       ok     = false;
    const auto length = buffer.first(headerBytes).toUInt(&ok, 16);
    return ok && length > 0 && limit > 0 && length <= static_cast<unsigned>(limit)
               ? static_cast<int>(length)
               : -1;
}

inline DecodeResult decode(
    QByteArray &buffer, QJsonObject &message, int limit = maxPayloadBytes, QString *error = nullptr)
{
    const int length = payloadLength(buffer, limit);
    if (length < 0) {
        if (error)
            *error = QStringLiteral("Invalid IPC frame length");
        return DecodeResult::Invalid;
    }
    if (length == 0 || buffer.size() - headerBytes < length)
        return DecodeResult::Incomplete;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(buffer.mid(headerBytes, length), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid IPC JSON object");
        return DecodeResult::Invalid;
    }
    message = document.object();
    buffer.remove(0, headerBytes + length);
    return DecodeResult::Complete;
}

} // namespace QSocIpc

#endif
