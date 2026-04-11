// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QUuid>

#include <algorithm>

namespace {

constexpr qint64 LITE_READ_BUDGET = 64 * 1024;

QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

void appendJsonLine(const QString &filePath, const nlohmann::json &line)
{
    /* Ensure parent directory exists. JSONL is append-only so a single open
     * call per write is the simplest correct path — atomic at the OS level
     * for one write() call up to PIPE_BUF, which a single message line is
     * well within. */
    QFileInfo fileInfo(filePath);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        parentDir.mkpath(QStringLiteral("."));
    }

    QFile file(filePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    const std::string serialized = line.dump();
    file.write(serialized.data(), static_cast<qint64>(serialized.size()));
    file.write("\n", 1);
    file.close();
}

} // namespace

QSocSession::QSocSession(QString sessionId, QString filePath)
    : sessionIdValue(std::move(sessionId))
    , filePathValue(std::move(filePath))
{}

void QSocSession::appendMessage(const nlohmann::json &message)
{
    nlohmann::json line;
    line["type"] = "message";
    line["ts"]   = isoNow().toStdString();
    /* Merge user message fields directly so loaders can pass each line
     * straight through to the agent's chat array after stripping the
     * type/ts envelope. */
    for (auto it = message.begin(); it != message.end(); ++it) {
        line[it.key()] = it.value();
    }
    appendJsonLine(filePathValue, line);
}

void QSocSession::appendMeta(const QString &key, const QString &value)
{
    nlohmann::json line;
    line["type"]  = "meta";
    line["ts"]    = isoNow().toStdString();
    line["key"]   = key.toStdString();
    line["value"] = value.toStdString();
    appendJsonLine(filePathValue, line);
}

void QSocSession::rewriteMessages(const nlohmann::json &messages)
{
    QFileInfo fileInfo(filePathValue);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        parentDir.mkpath(QStringLiteral("."));
    }

    /* Truncate + rewrite. Loses any meta entries that aren't preserved by
     * the caller, so callers should re-emit meta after rewriting. */
    QFile file(filePathValue);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    if (messages.is_array()) {
        for (const auto &msg : messages) {
            nlohmann::json line;
            line["type"] = "message";
            line["ts"]   = isoNow().toStdString();
            for (auto it = msg.begin(); it != msg.end(); ++it) {
                line[it.key()] = it.value();
            }
            const std::string serialized = line.dump();
            file.write(serialized.data(), static_cast<qint64>(serialized.size()));
            file.write("\n", 1);
        }
    }
    file.close();
}

nlohmann::json QSocSession::loadMessages(const QString &filePath)
{
    nlohmann::json messages = nlohmann::json::array();
    QFile          file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return messages;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.isEmpty()) {
            continue;
        }
        try {
            nlohmann::json doc = nlohmann::json::parse(line.toStdString());
            if (!doc.is_object() || !doc.contains("type")) {
                continue;
            }
            const std::string type = doc["type"].get<std::string>();
            if (type != "message") {
                continue;
            }
            /* Strip envelope keys before handing to the agent. */
            nlohmann::json msg = doc;
            msg.erase("type");
            msg.erase("ts");
            messages.push_back(msg);
        } catch (...) {
            /* Skip malformed line — probably a torn write from a crash. */
            continue;
        }
    }
    file.close();
    return messages;
}

QSocSession::Info QSocSession::readInfo(const QString &filePath)
{
    Info info;
    info.path = filePath;

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        return info;
    }

    info.id           = fileInfo.completeBaseName();
    info.lastModified = fileInfo.lastModified();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return info;
    }

    /* Walk the file once. Sessions are typically small (a few KB) so a full
     * read is cheaper than seeking around. We bail early if we exceed the
     * lite budget — large sessions reuse the metadata that was committed
     * near the top. */
    QTextStream stream(&file);
    qint64      bytesRead = 0;
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        bytesRead += line.size() + 1;
        if (line.isEmpty()) {
            continue;
        }
        try {
            nlohmann::json doc = nlohmann::json::parse(line.toStdString());
            if (!doc.is_object() || !doc.contains("type")) {
                continue;
            }
            const std::string type = doc["type"].get<std::string>();
            if (type == "meta" && doc.contains("key") && doc.contains("value")) {
                const QString key   = QString::fromStdString(doc["key"].get<std::string>());
                const QString value = QString::fromStdString(doc["value"].get<std::string>());
                if (key == QStringLiteral("created") && info.createdAt.isNull()) {
                    info.createdAt = QDateTime::fromString(value, Qt::ISODateWithMs);
                    if (!info.createdAt.isValid()) {
                        info.createdAt = QDateTime::fromString(value, Qt::ISODate);
                    }
                } else if (key == QStringLiteral("first_prompt") && info.firstPrompt.isEmpty()) {
                    info.firstPrompt = value;
                } else if (key == QStringLiteral("branch")) {
                    info.branch = value;
                }
            } else if (type == "message") {
                info.messageCount++;
                /* Backfill first_prompt from the first user message if no
                 * explicit meta record exists. */
                if (info.firstPrompt.isEmpty() && doc.contains("role")
                    && doc["role"].get<std::string>() == "user" && doc.contains("content")
                    && doc["content"].is_string()) {
                    info.firstPrompt = QString::fromStdString(doc["content"].get<std::string>());
                }
            }
        } catch (...) {
            continue;
        }
        if (bytesRead > LITE_READ_BUDGET && info.messageCount > 0 && !info.firstPrompt.isEmpty()) {
            /* We have enough for the picker; skip the rest. */
            break;
        }
    }
    file.close();

    if (info.createdAt.isNull()) {
        info.createdAt = fileInfo.birthTime().isValid() ? fileInfo.birthTime() : info.lastModified;
    }
    return info;
}

QList<QSocSession::Info> QSocSession::listAll(const QString &projectPath)
{
    QList<Info> result;
    QDir        dir(sessionsDir(projectPath));
    if (!dir.exists()) {
        return result;
    }
    const auto entries = dir.entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    result.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        result.append(readInfo(entry.absoluteFilePath()));
    }
    /* Sort by lastModified descending so the most recent session is at the
     * top of the picker even if Qt's QDir::Time ordering disagrees on
     * filesystems with sub-second timestamps. */
    std::sort(result.begin(), result.end(), [](const Info &lhs, const Info &rhs) {
        return lhs.lastModified > rhs.lastModified;
    });
    return result;
}

QString QSocSession::resolveId(const QString &projectPath, const QString &idOrPrefix)
{
    if (idOrPrefix.isEmpty()) {
        return QString();
    }
    const auto sessions = listAll(projectPath);
    QString    match;
    int        hits = 0;
    for (const Info &info : sessions) {
        if (info.id == idOrPrefix) {
            return info.id; /* Exact wins immediately. */
        }
        if (info.id.startsWith(idOrPrefix, Qt::CaseInsensitive)) {
            match = info.id;
            hits++;
        }
    }
    if (hits == 1) {
        return match;
    }
    return QString();
}

QString QSocSession::sessionsDir(const QString &projectPath)
{
    QString base = projectPath;
    if (base.isEmpty()) {
        base = QDir::currentPath();
    }
    return QDir(base).filePath(QStringLiteral(".qsoc/sessions"));
}

QString QSocSession::generateId()
{
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return uuid;
}
