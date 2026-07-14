// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPJSON_P_H
#define QSOCMCPJSON_P_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>

#include <QString>

namespace QSocMcpJson {

inline bool hasVersion(const nlohmann::json &message)
{
    return message.is_object() && message.contains("jsonrpc") && message["jsonrpc"].is_string()
           && message["jsonrpc"] == "2.0";
}

inline bool readInteger(const nlohmann::json &value, int *result)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        *result = static_cast<int>(number);
        return true;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
        return false;
    }
    *result = static_cast<int>(number);
    return true;
}

inline bool readError(const nlohmann::json &error, int *code, QString *message)
{
    if (!error.is_object() || !error.contains("code") || !error.contains("message")
        || !error["message"].is_string() || !readInteger(error["code"], code)) {
        return false;
    }
    *message = QString::fromStdString(error["message"].get<std::string>());
    return true;
}

inline std::optional<int> claimableResponseId(const nlohmann::json &message)
{
    if (!message.is_object() || message.contains("method") || !hasVersion(message)
        || !message.contains("id") || message.contains("result") == message.contains("error")) {
        return std::nullopt;
    }
    if (message.contains("error")) {
        int     code = 0;
        QString text;
        if (!readError(message["error"], &code, &text)) {
            return std::nullopt;
        }
    }
    int id = -1;
    if (!readInteger(message["id"], &id)) {
        return std::nullopt;
    }
    return id;
}

} // namespace QSocMcpJson

#endif // QSOCMCPJSON_P_H
