// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsession.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

constexpr qint64 LITE_READ_BUDGET = 64 * 1024;

QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool validRecoveryClaimId(const QString &runId)
{
    return !runId.trimmed().isEmpty();
}

QString recoveryClaimDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("recovery-claims"));
}

QString recoveryClaimPath(const QString &runId)
{
    if (!validRecoveryClaimId(runId)) {
        return {};
    }
    const QString directory = recoveryClaimDir();
    if (directory.isEmpty()) {
        return {};
    }
    const QByteArray name
        = QCryptographicHash::hash(runId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(directory).filePath(QString::fromLatin1(name) + QStringLiteral(".claim"));
}

bool appendJsonLine(const QString &filePath, const nlohmann::json &line)
{
    QFileInfo fileInfo(filePath);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists() && !parentDir.mkpath(QStringLiteral("."))) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    QByteArray payload;
    try {
        payload = QByteArray::fromStdString(line.dump());
    } catch (const nlohmann::json::exception &) {
        return false;
    }
    payload.append('\n');
    return file.write(payload) == payload.size() && file.flush();
}

QString runEventName(QSocSession::RunEvent event)
{
    switch (event) {
    case QSocSession::RunEvent::Started:
        return QStringLiteral("started");
    case QSocSession::RunEvent::Checkpoint:
        return QStringLiteral("checkpoint");
    case QSocSession::RunEvent::ToolStarted:
        return QStringLiteral("tool_started");
    case QSocSession::RunEvent::Completed:
        return QStringLiteral("completed");
    case QSocSession::RunEvent::Error:
        return QStringLiteral("error");
    case QSocSession::RunEvent::Aborted:
        return QStringLiteral("aborted");
    case QSocSession::RunEvent::Invalid:
        break;
    }
    return {};
}

std::optional<QSocSession::RunEvent> parseRunEvent(const nlohmann::json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const QString raw = QString::fromStdString(value.get<std::string>());
    if (raw == QStringLiteral("started")) {
        return QSocSession::RunEvent::Started;
    }
    if (raw == QStringLiteral("checkpoint")) {
        return QSocSession::RunEvent::Checkpoint;
    }
    if (raw == QStringLiteral("tool_started")) {
        return QSocSession::RunEvent::ToolStarted;
    }
    if (raw == QStringLiteral("completed")) {
        return QSocSession::RunEvent::Completed;
    }
    if (raw == QStringLiteral("error")) {
        return QSocSession::RunEvent::Error;
    }
    if (raw == QStringLiteral("aborted")) {
        return QSocSession::RunEvent::Aborted;
    }
    return std::nullopt;
}

bool validRunContext(const QSocSession::RunRecord &record)
{
    return record.contextPresent && !record.projectRoot.trimmed().isEmpty()
           && !record.workingDir.trimmed().isEmpty()
           && (!record.registryModel || !record.modelId.trimmed().isEmpty())
           && record.remoteMode == !record.remoteName.trimmed().isEmpty();
}

bool parseRunContext(const nlohmann::json &line, QSocSession::RunRecord *record)
{
    if (!line.contains("context")) {
        record->contextPresent = false;
        return true;
    }
    const nlohmann::json &context = line["context"];
    if (!context.is_object() || !context.contains("model_id") || !context["model_id"].is_string()
        || !context.contains("effort_level") || !context["effort_level"].is_string()
        || !context.contains("reasoning_model") || !context["reasoning_model"].is_string()
        || !context.contains("registry_model") || !context["registry_model"].is_boolean()
        || !context.contains("plan_mode") || !context["plan_mode"].is_boolean()
        || !context.contains("remote_mode") || !context["remote_mode"].is_boolean()
        || !context.contains("remote_name") || !context["remote_name"].is_string()
        || !context.contains("project_root") || !context["project_root"].is_string()
        || !context.contains("working_dir") || !context["working_dir"].is_string()) {
        return false;
    }

    record->contextPresent = true;
    record->registryModel  = context["registry_model"].get<bool>();
    record->modelId        = QString::fromStdString(context["model_id"].get<std::string>());
    record->effortLevel    = QString::fromStdString(context["effort_level"].get<std::string>());
    record->reasoningModel = QString::fromStdString(context["reasoning_model"].get<std::string>());
    record->planMode       = context["plan_mode"].get<bool>();
    record->remoteMode     = context["remote_mode"].get<bool>();
    record->remoteName     = QString::fromStdString(context["remote_name"].get<std::string>());
    record->projectRoot    = QString::fromStdString(context["project_root"].get<std::string>());
    record->workingDir     = QString::fromStdString(context["working_dir"].get<std::string>());
    return validRunContext(*record);
}

void sanitizeLoadedMessage(nlohmann::json *message)
{
    if (message->is_object() && message->contains("role") && (*message)["role"] == "assistant"
        && message->contains("content") && (*message)["content"].is_null()
        && !message->contains("tool_calls")) {
        (*message)["content"] = "";
    }
}

} // namespace

QSocSession::QSocSession(QString sessionId, QString filePath)
    : sessionIdValue(std::move(sessionId))
    , filePathValue(std::move(filePath))
    , persisted(QFile::exists(filePathValue))
{}

bool QSocSession::flushPendingMeta()
{
    if (persisted || pendingMeta.isEmpty()) {
        return true;
    }
    for (const auto &kv : std::as_const(pendingMeta)) {
        nlohmann::json metaLine;
        metaLine["type"]  = "meta";
        metaLine["ts"]    = isoNow().toStdString();
        metaLine["key"]   = kv.first.toStdString();
        metaLine["value"] = kv.second.toStdString();
        if (!appendJsonLine(filePathValue, metaLine)) {
            return false;
        }
    }
    pendingMeta.clear();
    persisted = true;
    return true;
}

bool QSocSession::appendMessage(const nlohmann::json &message)
{
    if (!flushPendingMeta()) {
        return false;
    }

    nlohmann::json line;
    line["type"] = "message";
    line["ts"]   = isoNow().toStdString();
    /* Merge user message fields directly so loaders can pass each line
     * straight through to the agent's chat array after stripping the
     * type/ts envelope. */
    for (auto it = message.begin(); it != message.end(); ++it) {
        line[it.key()] = it.value();
    }
    if (!appendJsonLine(filePathValue, line)) {
        return false;
    }
    persisted = true;
    return true;
}

bool QSocSession::appendMeta(const QString &key, const QString &value)
{
    if (!persisted) {
        pendingMeta.append(qMakePair(key, value));
        return true;
    }
    nlohmann::json line;
    line["type"]  = "meta";
    line["ts"]    = isoNow().toStdString();
    line["key"]   = key.toStdString();
    line["value"] = value.toStdString();
    return appendJsonLine(filePathValue, line);
}

bool QSocSession::appendRun(const RunRecord &record)
{
    const QString event = runEventName(record.event);
    if (record.runId.trimmed().isEmpty() || event.isEmpty()
        || (record.event == RunEvent::Started && !record.contextPresent)
        || (record.event == RunEvent::Started && record.messageCount < 0)
        || (record.event == RunEvent::Started && record.historyDigest.trimmed().isEmpty())
        || (record.contextPresent && !validRunContext(record))
        || (record.event == RunEvent::ToolStarted && record.toolCallId.trimmed().isEmpty())) {
        return false;
    }
    if (!flushPendingMeta()) {
        return false;
    }

    nlohmann::json line;
    line["type"]   = "run";
    line["ts"]     = isoNow().toStdString();
    line["run_id"] = record.runId.toStdString();
    line["event"]  = event.toStdString();
    if (record.event == RunEvent::Started || !record.input.isEmpty()) {
        line["input"] = record.input.toStdString();
    }
    if (record.event == RunEvent::Started || !record.goalId.isEmpty()) {
        line["goal_id"] = record.goalId.toStdString();
    }
    if (record.event == RunEvent::Started) {
        line["message_count"]     = record.messageCount;
        line["history_digest"]    = record.historyDigest.toStdString();
        line["input_replay_safe"] = record.inputReplaySafe;
    }
    if (record.contextPresent) {
        line["context"] = {
            {"model_id", record.modelId.toStdString()},
            {"registry_model", record.registryModel},
            {"effort_level", record.effortLevel.toStdString()},
            {"reasoning_model", record.reasoningModel.toStdString()},
            {"plan_mode", record.planMode},
            {"remote_mode", record.remoteMode},
            {"remote_name", record.remoteName.toStdString()},
            {"project_root", record.projectRoot.toStdString()},
            {"working_dir", record.workingDir.toStdString()},
        };
    }
    if (!record.toolCallId.isEmpty()) {
        line["tool_call_id"] = record.toolCallId.toStdString();
    }
    if (!appendJsonLine(filePathValue, line)) {
        return false;
    }
    persisted = true;
    return true;
}

bool QSocSession::appendSnapshot(const nlohmann::json &messages)
{
    if (!messages.is_array() || !flushPendingMeta()) {
        return false;
    }
    nlohmann::json line;
    line["type"]     = "snapshot";
    line["ts"]       = isoNow().toStdString();
    line["messages"] = messages;
    if (!appendJsonLine(filePathValue, line)) {
        return false;
    }
    persisted = true;
    return true;
}

bool QSocSession::rewriteMessages(const nlohmann::json &messages)
{
    const bool emptyRewrite = !messages.is_array() || messages.empty();

    /* /clear on a still-empty session is a no-op: nothing on disk to
     * truncate, and creating a zero-message file would leave the same
     * orphan the lazy-persist path is trying to avoid. */
    if (!persisted && emptyRewrite) {
        pendingMeta.clear();
        return true;
    }

    QFileInfo fileInfo(filePathValue);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists() && !parentDir.mkpath(QStringLiteral("."))) {
        return false;
    }

    QSaveFile file(filePathValue);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    if (messages.is_array()) {
        for (const auto &msg : messages) {
            nlohmann::json line;
            line["type"] = "message";
            line["ts"]   = isoNow().toStdString();
            for (auto it = msg.begin(); it != msg.end(); ++it) {
                line[it.key()] = it.value();
            }
            QByteArray payload;
            try {
                payload = QByteArray::fromStdString(line.dump());
            } catch (const nlohmann::json::exception &) {
                file.cancelWriting();
                return false;
            }
            payload.append('\n');
            if (file.write(payload) != payload.size()) {
                file.cancelWriting();
                return false;
            }
        }
    }
    if (!file.commit()) {
        return false;
    }
    persisted = !emptyRewrite;
    pendingMeta.clear();
    return true;
}

QString QSocSession::readMeta(const QString &filePath, const QString &key)
{
    QString value;
    QFile   file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return value;
    }
    QTextStream       stream(&file);
    const std::string wantKey = key.toStdString();
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.isEmpty()) {
            continue;
        }
        try {
            const nlohmann::json doc = nlohmann::json::parse(line.toStdString());
            if (!doc.is_object() || doc.value("type", std::string()) != "meta") {
                continue;
            }
            if (doc.value("key", std::string()) == wantKey && doc.contains("value")) {
                /* Latest line wins; keep scanning to the end. */
                value = QString::fromStdString(doc["value"].get<std::string>());
            }
        } catch (...) {
            continue;
        }
    }
    file.close();
    return value;
}

QMap<QString, QString> QSocSession::readMetas(const QString &filePath, const QStringList &keys)
{
    QMap<QString, QString> out;
    QFile                  file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QSet<QString> wanted(keys.begin(), keys.end());
    QTextStream   stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.isEmpty()) {
            continue;
        }
        try {
            const nlohmann::json doc = nlohmann::json::parse(line.toStdString());
            if (!doc.is_object() || doc.value("type", std::string()) != "meta"
                || !doc.contains("value")) {
                continue;
            }
            const QString key = QString::fromStdString(doc.value("key", std::string()));
            if (wanted.contains(key)) {
                /* Latest line wins; keep scanning to the end. */
                out[key] = QString::fromStdString(doc["value"].get<std::string>());
            }
        } catch (...) {
            continue;
        }
    }
    file.close();
    return out;
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
            if (type == "snapshot") {
                if (!doc.contains("messages") || !doc["messages"].is_array()) {
                    continue;
                }
                nlohmann::json snapshot = doc["messages"];
                for (auto &message : snapshot) {
                    sanitizeLoadedMessage(&message);
                }
                messages = std::move(snapshot);
                continue;
            }
            if (type != "message") {
                continue;
            }
            /* Strip envelope keys before handing to the agent. */
            nlohmann::json msg = doc;
            msg.erase("type");
            msg.erase("ts");
            sanitizeLoadedMessage(&msg);
            messages.push_back(msg);
        } catch (...) {
            /* Skip malformed lines, including a torn final write. */
            continue;
        }
    }
    file.close();
    return messages;
}

QString QSocSession::historyDigest(const nlohmann::json &messages)
{
    if (!messages.is_array()) {
        return {};
    }
    try {
        nlohmann::json normalized = messages;
        for (auto &message : normalized) {
            sanitizeLoadedMessage(&message);
        }
        const QByteArray serialized = QByteArray::fromStdString(normalized.dump());
        return QString::fromLatin1(
            QCryptographicHash::hash(serialized, QCryptographicHash::Sha256).toHex());
    } catch (const nlohmann::json::exception &) {
        return {};
    }
}

bool QSocSession::createRecoveryClaim(const QString &runId)
{
    const QString path = recoveryClaimPath(runId);
    if (path.isEmpty()) {
        return false;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return false;
    }
    QFile::setPermissions(
        directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray content = runId.toUtf8();
    if (file.write(content) != content.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool QSocSession::hasRecoveryClaim(const QString &runId)
{
    const QString   path = recoveryClaimPath(runId);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || info.isSymLink()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return file.readAll() == runId.toUtf8();
}

bool QSocSession::removeRecoveryClaim(const QString &runId)
{
    const QString path = recoveryClaimPath(runId);
    return !path.isEmpty() && QFileInfo::exists(path) && QFile::remove(path);
}

std::optional<QSocSession::RunRecord> QSocSession::latestRun(const QString &filePath)
{
    std::optional<RunRecord> current;
    QFile                    file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return current;
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString rawLine = stream.readLine();
        if (rawLine.isEmpty()) {
            continue;
        }
        nlohmann::json line;
        try {
            line = nlohmann::json::parse(rawLine.toStdString());
        } catch (const nlohmann::json::exception &) {
            if (current.has_value()) {
                RunRecord invalid;
                invalid.runId = current->runId;
                invalid.event = RunEvent::Invalid;
                current       = invalid;
            }
            continue;
        }
        if (!line.is_object() || !line.contains("type") || !line["type"].is_string()) {
            if (current.has_value()) {
                RunRecord invalid;
                invalid.runId = current->runId;
                invalid.event = RunEvent::Invalid;
                current       = invalid;
            }
            continue;
        }
        if (line["type"].get_ref<const std::string &>() != "run") {
            continue;
        }

        RunRecord invalid;
        invalid.event = RunEvent::Invalid;
        if (!line.contains("run_id") || !line["run_id"].is_string() || !line.contains("event")) {
            current = invalid;
            continue;
        }
        const QString runId = QString::fromStdString(line["run_id"].get<std::string>());
        const auto    event = parseRunEvent(line["event"]);
        invalid.runId       = runId;
        if (runId.trimmed().isEmpty() || !event.has_value()) {
            current = invalid;
            continue;
        }

        if (*event == RunEvent::Started) {
            RunRecord started;
            started.runId = runId;
            started.event = *event;
            if (line.contains("input") && line["input"].is_string()) {
                started.input = QString::fromStdString(line["input"].get<std::string>());
            } else if (line.contains("input")) {
                current = invalid;
                continue;
            }
            if (line.contains("goal_id") && line["goal_id"].is_string()) {
                started.goalId = QString::fromStdString(line["goal_id"].get<std::string>());
            } else if (line.contains("goal_id")) {
                current = invalid;
                continue;
            }
            if (line.contains("message_count")) {
                const nlohmann::json &count = line["message_count"];
                if (count.is_number_unsigned()) {
                    const auto value = count.get<std::uint64_t>();
                    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                        current = invalid;
                        continue;
                    }
                    started.messageCount = static_cast<int>(value);
                } else if (count.is_number_integer()) {
                    const auto value = count.get<std::int64_t>();
                    if (value < 0 || value > std::numeric_limits<int>::max()) {
                        current = invalid;
                        continue;
                    }
                    started.messageCount = static_cast<int>(value);
                } else {
                    current = invalid;
                    continue;
                }
            }
            if (line.contains("history_digest") && line["history_digest"].is_string()) {
                started.historyDigest = QString::fromStdString(
                    line["history_digest"].get<std::string>());
                if (started.historyDigest.trimmed().isEmpty()) {
                    current = invalid;
                    continue;
                }
            } else if (line.contains("history_digest")) {
                current = invalid;
                continue;
            }
            if (line.contains("input_replay_safe")) {
                if (!line["input_replay_safe"].is_boolean()) {
                    current = invalid;
                    continue;
                }
                started.inputReplaySafe = line["input_replay_safe"].get<bool>();
            }
            if (!parseRunContext(line, &started)) {
                current = invalid;
                continue;
            }
            current = started;
            continue;
        }

        if (!current.has_value() || current->event == RunEvent::Invalid || current->runId != runId) {
            current = invalid;
            continue;
        }
        if (line.contains("context") && !parseRunContext(line, &*current)) {
            current = invalid;
            continue;
        }
        if (line.contains("input")) {
            if (!line["input"].is_string()) {
                current = invalid;
                continue;
            }
            current->input = QString::fromStdString(line["input"].get<std::string>());
        }
        if (line.contains("goal_id")) {
            if (!line["goal_id"].is_string()) {
                current = invalid;
                continue;
            }
            current->goalId = QString::fromStdString(line["goal_id"].get<std::string>());
        }
        current->event = *event;
        current->toolCallId.clear();
        if (*event == RunEvent::Checkpoint) {
            current->startedToolCallIds.clear();
        }
        if (*event == RunEvent::ToolStarted) {
            if (!line.contains("tool_call_id") || !line["tool_call_id"].is_string()) {
                current = invalid;
                continue;
            }
            current->toolCallId = QString::fromStdString(line["tool_call_id"].get<std::string>());
            if (current->toolCallId.trimmed().isEmpty()) {
                current = invalid;
            } else if (!current->startedToolCallIds.contains(current->toolCallId)) {
                current->startedToolCallIds.append(current->toolCallId);
            }
        }
    }
    return current;
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
     * lite budget. Large sessions reuse the metadata that was committed
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
                } else if (key == QStringLiteral("title")) {
                    /* A manual /rename always wins over an auto title. */
                    info.title = value;
                } else if (key == QStringLiteral("auto_title")) {
                    /* Generated title fills in only when no manual title is
                     * present (regardless of meta-line order). */
                    if (info.title.isEmpty()) {
                        info.title = value;
                    }
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
            } else if (type == "snapshot" && doc.contains("messages") && doc["messages"].is_array()) {
                info.messageCount = static_cast<int>(doc["messages"].size());
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
            return info.id; /* Exact id wins immediately. */
        }
        /* Beyond an id prefix, match a title or branch substring so a
         * session can be resumed by its human-readable name. */
        const bool byId     = info.id.startsWith(idOrPrefix, Qt::CaseInsensitive);
        const bool byTitle  = !info.title.isEmpty()
                              && info.title.contains(idOrPrefix, Qt::CaseInsensitive);
        const bool byBranch = !info.branch.isEmpty()
                              && info.branch.contains(idOrPrefix, Qt::CaseInsensitive);
        if (byId || byTitle || byBranch) {
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
