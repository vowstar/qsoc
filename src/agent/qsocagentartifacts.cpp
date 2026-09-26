// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"

#include <QSet>

namespace {
QString boundedText(const QString &text, qint64 tokens)
{
    const QByteArray bytes = text.toUtf8();
    qint64           low   = 0;
    qint64           high  = bytes.size();
    while (low < high) {
        const qint64 middle = low + (high - low + 1) / 2;
        const auto   prefix = QString::fromUtf8(
            bytes.first(QSocToolResultStore::utf8End(bytes, 0, middle)));
        if (QSocRequestUsage::estimateText(prefix) <= tokens)
            low = middle;
        else
            high = middle - 1;
    }
    return QString::fromUtf8(bytes.first(QSocToolResultStore::utf8End(bytes, 0, low)));
}
} // namespace

void QSocAgent::unbindToolResultStore()
{
    if (compactionCommitting_) {
        return;
    }
    toolResultStore_.reset();
    ++bindingRevision_;
}

bool QSocAgent::bindToolResultStore(const QString &directory, const QString &owner, QString *error)
{
    if (compactionCommitting_) {
        if (error) {
            *error = QStringLiteral("Cannot change artifact storage during compaction commit.");
        }
        return false;
    }
    auto candidate = std::make_shared<QSocToolResultStore>(
        directory,
        owner,
        QSocToolResultStore::Limits{
            agentConfig.toolArtifactBytes,
            agentConfig.toolArtifactSessionBytes,
            agentConfig.toolArtifactPageBytes});
    if (!candidate->isBound()) {
        toolResultStore_.reset();
        ++bindingRevision_;
        if (error)
            *error = QStringLiteral("Tool result storage could not be bound to this session.");
        return false;
    }
    toolResultStore_ = std::move(candidate);
    ++bindingRevision_;
    return true;
}

json QSocAgent::artifactReferenceJson(const QSocToolResultStore::Reference &reference)
{
    return {
        {"artifact_id", reference.id.toStdString()},
        {"sha256", reference.sha256.toStdString()},
        {"captured_bytes", reference.capturedBytes},
        {"origin", reference.origin.toStdString()},
        {"completion", reference.completion.toStdString()},
        {"source_completeness", reference.sourceCompleteness.toStdString()}};
}

QList<QSocToolResultStore::Reference> QSocAgent::artifactReferences(const json &history)
{
    QList<QSocToolResultStore::Reference> references;
    if (!history.is_array())
        return references;
    QSet<QString> seen;
    for (const auto &message : history) {
        if (!message.is_object() || !message.contains("_qsoc_artifact_refs")
            || !message["_qsoc_artifact_refs"].is_array())
            continue;
        for (const auto &value : message["_qsoc_artifact_refs"]) {
            try {
                QSocToolResultStore::Reference reference;
                reference.id = QString::fromStdString(value.at("artifact_id").get<std::string>());
                reference.sha256 = QString::fromStdString(value.at("sha256").get<std::string>());
                reference.capturedBytes = value.at("captured_bytes").get<qint64>();
                reference.origin = QString::fromStdString(value.at("origin").get<std::string>());
                reference.completion = QString::fromStdString(
                    value.at("completion").get<std::string>());
                reference.sourceCompleteness = QString::fromStdString(
                    value.at("source_completeness").get<std::string>());
                const QString key = reference.id + QLatin1Char(':') + reference.sha256;
                if (!seen.contains(key)) {
                    seen.insert(key);
                    references.push_back(reference);
                }
            } catch (const json::exception &) {
                // Invalid references cannot grant access.
            }
        }
    }
    return references;
}

qint64 QSocAgent::toolResultBudgetTokens() const
{
    const auto run = activeRun_;
    if (!run || !run->toolBatchStart || !isCurrentRun(run))
        return 4096;
    json              wire = wireMessages(buildSystemPromptWithMemory());
    QSet<std::string> complete;
    for (auto index = *run->toolBatchStart + 1; index < messages.size(); ++index) {
        const auto &message = messages.at(index);
        if (message.value("role", std::string()) == "tool")
            complete.insert(message.value("tool_call_id", std::string()));
    }
    const auto &assistant = messages.at(*run->toolBatchStart);
    for (const auto &call : assistant.at("tool_calls")) {
        const auto id = call.value("id", std::string());
        if (!complete.contains(id))
            wire.push_back(
                {{"role", "tool"},
                 {"tool_call_id", id},
                 {"content", "Not executed because the tool batch was interrupted."}});
    }
    for (const auto &attachment : run->toolBatchAttachments) {
        json sanitized = attachment;
        sanitized.erase("_img_tokens");
        wire.push_back(std::move(sanitized));
    }
    auto snapshot     = run->requestSnapshot;
    snapshot.messages = std::move(wire);
    const auto used   = QSocRequestUsage::estimateRequest(snapshot);
    return std::clamp(qint64(effectiveContextTokens()) - used - 256, qint64(0), qint64(4096));
}

void QSocAgent::appendBoundedToolMessage(
    const QString &id, const QString &content, const QString &state, const QString &toolName)
{
    const auto   run    = activeRun_;
    const qint64 budget = toolResultBudgetTokens();
    QString      view   = content;
    json         refs   = json::array();
    if (QSocRequestUsage::estimateText(content) > budget) {
        QString                                       error;
        std::optional<QSocToolResultStore::Reference> saved;
        const auto status     = run && run->executingToolStatus ? *run->executingToolStatus
                                                                : QSocTool::classifyResult(content);
        QString    completion = state.isEmpty() ? QStringLiteral("ok") : state;
        if (state.isEmpty() && status == QSocToolResultStatus::Failed)
            completion = QStringLiteral("failed");
        else if (state.isEmpty() && status == QSocToolResultStatus::Uncertain)
            completion = QStringLiteral("uncertain");
        else if (state.isEmpty() && status == QSocToolResultStatus::Dispatched)
            completion = QStringLiteral("dispatched");
        const bool sourceTruncated = (toolName == QStringLiteral("bash")
                                      && content.endsWith("... (output truncated)"))
                                     || (toolName == QStringLiteral("web_fetch")
                                         && content.endsWith("... (content truncated)"));
        if (toolResultStore_ && toolName != QStringLiteral("tool_output_read"))
            saved = toolResultStore_->publish(
                content,
                completion,
                sourceTruncated ? QStringLiteral("truncated") : QStringLiteral("unknown"),
                &error,
                [run] { return !run || run->stop.load() == StopMode::None; });
        if (saved) {
            refs.push_back(artifactReferenceJson(*saved));
            const QString notice
                = QStringLiteral("\n[Captured tool return saved. Read with tool_output_read: %1]")
                      .arg(QString::fromStdString(refs.front().dump()));
            view = boundedText(
                       content, std::max(qint64(0), budget - QSocRequestUsage::estimateText(notice)))
                   + notice;
        } else {
            const QString notice
                = QStringLiteral(
                      "\n[Return text omitted. No readable artifact was saved. Completion: %1. %2 "
                      "Do not repeat side effects without checking state.]")
                      .arg(
                          completion,
                          error.isEmpty() ? QStringLiteral("Result storage is unavailable.")
                                          : error);
            view = boundedText(
                       content, std::max(qint64(0), budget - QSocRequestUsage::estimateText(notice)))
                   + notice;
        }
    }
    json message
        = {{"role", "tool"},
           {"tool_call_id", id.toStdString()},
           {"content", view.toStdString()},
           {"_qsoc_result_bounded", true}};
    if (!state.isEmpty())
        message["_qsoc_tool_state"] = state.toStdString();
    if (!refs.empty())
        message["_qsoc_artifact_refs"] = std::move(refs);
    messages.push_back(std::move(message));
    if (run && run->toolBatchStart && budget < 256) {
        lastStopNotice_ = QStringLiteral(
            "Tool batch stopped: results exhausted the remaining context.");
        requestStop(StopMode::Hard);
    }
}

void QSocAgent::stopForToolResultBudget()
{
    lastStopNotice_ = QStringLiteral(
        "Tool batch stopped: the remaining context cannot hold a result.");
    requestStop(StopMode::Hard);
}
