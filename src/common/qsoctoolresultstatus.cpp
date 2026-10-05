// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoctoolresultstatus.h"

#include <nlohmann/json.hpp>

namespace QSocToolResult {

QSocToolResultStatus classify(const QString &result)
{
    /* An explicit status must be the first line, so no amount of body text
     * can forge or hide one. */
    const QString first = result.left(result.indexOf(QLatin1Char('\n'))).trimmed();
    if (first == QStringLiteral("status: ok")) {
        return QSocToolResultStatus::Ok;
    }
    if (first == QStringLiteral("status: failed")) {
        return QSocToolResultStatus::Failed;
    }
    if (first == QStringLiteral("status: uncertain")) {
        return QSocToolResultStatus::Uncertain;
    }
    if (first == QStringLiteral("status: dispatched"))
        return QSocToolResultStatus::Dispatched;
    if (result.trimmed().startsWith(QStringLiteral("Error:"))) {
        return QSocToolResultStatus::Failed;
    }
    /* Tools that answer in JSON declare the same thing in a "status" member,
     * so read that rather than leaving their failures unstyled. Only a
     * top-level object counts, and only its own status member: this is the
     * tool's declaration, not a search of its payload. */
    const QString trimmed = result.trimmed();
    if (trimmed.startsWith(QLatin1Char('{'))) {
        const auto parsed
            = nlohmann::json::parse(trimmed.toStdString(), nullptr, /*allow_exceptions=*/false);
        if (parsed.is_object() && parsed.contains("status") && parsed["status"].is_string()) {
            const auto declared = QString::fromStdString(parsed["status"].get<std::string>());
            if (declared == QStringLiteral("error")) {
                return QSocToolResultStatus::Failed;
            }
            if (declared == QStringLiteral("uncertain")) {
                return QSocToolResultStatus::Uncertain;
            }
        }
    }
    return QSocToolResultStatus::Ok;
}

QString name(QSocToolResultStatus status)
{
    switch (status) {
    case QSocToolResultStatus::Failed:
        return QStringLiteral("failed");
    case QSocToolResultStatus::Uncertain:
        return QStringLiteral("uncertain");
    case QSocToolResultStatus::Dispatched:
        return QStringLiteral("dispatched");
    case QSocToolResultStatus::Ok:
        break;
    }
    return QStringLiteral("ok");
}

} // namespace QSocToolResult
