// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "common/qsocboundedcapture.h"
#include "common/qsoctokenizer.h"

#include <QMap>
#include <QSet>

namespace {
constexpr qint64 toolBatchImageBytesLimit = 16 * 1024 * 1024;
constexpr qint64 toolResultTokensLimit    = 4096;
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
    return std::min(toolBatchRemainingTokens(), toolResultTokensLimit);
}

qint64 QSocAgent::toolBatchRemainingTokens(bool reserveResults) const
{
    const auto run = activeRun_;
    if (!run || !run->toolBatchStart || !isCurrentRun(run))
        return 4096;
    json              wire = wireMessages(requestSystemPrompt());
    QSet<std::string> complete;
    for (auto index = *run->toolBatchStart + 1; index < messages.size(); ++index) {
        const auto &message = messages.at(index);
        if (message.value("role", std::string()) == "tool")
            complete.insert(message.value("tool_call_id", std::string()));
    }
    qint64        reserved    = 0;
    const QString placeholder = QStringLiteral(
        "Not executed because the tool batch was interrupted.");
    const auto &assistant = messages.at(*run->toolBatchStart);
    for (const auto &call : assistant.at("tool_calls")) {
        const auto id = call.value("id", std::string());
        if (!complete.contains(id)) {
            if (reserveResults)
                reserved = std::min(
                    qint64(std::max(0, effectiveContextTokens())),
                    reserved
                        + std::max(
                            qint64(0),
                            toolResultTokensLimit
                                - QSocRequestUsage::estimateText(
                                    placeholder, run->requestSnapshot.counter)));
            wire.push_back(
                {{"role", "tool"}, {"tool_call_id", id}, {"content", placeholder.toStdString()}});
        }
    }
    auto snapshot     = run->requestSnapshot;
    snapshot.messages = std::move(wire);
    const auto used   = QSocRequestUsage::estimateRequest(snapshot);
    const auto images
        = QSocRequestUsage::estimateHistory(run->toolBatchAttachments, snapshot.counter);
    qint64 remaining = std::max(qint64(0), qint64(effectiveContextTokens()) - used);
    remaining        = std::max(qint64(0), remaining - images);
    return std::max(qint64(0), remaining - reserved - 256);
}

QString QSocAgent::queueToolAttachments(const QList<AttachmentSpec> &attachments)
{
    const auto run = activeRun_;
    if (attachments.isEmpty() || !run || !run->toolBatchStart || !isCurrentRun(run))
        return {};
    qint64 imageBytes = 0;
    for (const auto &message : run->toolBatchAttachments)
        for (const auto &part : message.at("content"))
            if (part.value("type", std::string()) == "image_url")
                imageBytes += part.at("image_url").at("url").get_ref<const std::string &>().size();
    qint64       remaining = toolBatchRemainingTokens(true);
    json         content  = json::array({{{"type", "text"}, {"text", "Tool attachment payload:"}}});
    const qint64 envelope = QSocRequestUsage::estimateMessages(
        json::array({{{"role", "user"}, {"content", content}}}),
        run->requestSnapshot.imageTokens,
        run->requestSnapshot.counter);
    remaining                         = std::max(qint64(0), remaining - envelope);
    qint64                imageTokens = 0;
    QMap<QString, qint64> rejected;
    QString               firstSource;
    for (const auto &attachment : attachments) {
        QString reason;
        const qint64 cost = std::max(qint64(attachment.estTokens), run->requestSnapshot.imageTokens);
        const qint64 availableBytes = toolBatchImageBytesLimit - imageBytes;
        QByteArray   mime;
        QByteArray   data;
        qint64       bytes = 0;
        if (attachment.estTokens <= 0)
            reason = QStringLiteral("unknown token cost");
        else if (cost > remaining)
            reason = QStringLiteral("remaining context budget");
        else if (attachment.mime.size() > 256 || attachment.dataB64.size() > availableBytes)
            reason = QStringLiteral("batch encoded byte limit");
        else {
            mime  = attachment.mime.toUtf8();
            data  = attachment.dataB64.toUtf8();
            bytes = 13 + mime.size() + data.size();
            if (bytes > availableBytes)
                reason = QStringLiteral("batch encoded byte limit");
        }
        if (!reason.isEmpty()) {
            ++rejected[reason];
            if (firstSource.isEmpty())
                firstSource = attachment.sourceUrl.left(512);
            continue;
        }
        const QByteArray url = QByteArrayLiteral("data:") + mime + QByteArrayLiteral(";base64,")
                               + data;
        content.push_back({{"type", "image_url"}, {"image_url", {{"url", url.toStdString()}}}});
        remaining -= cost;
        imageTokens += cost;
        imageBytes += bytes;
    }
    if (imageTokens > 0)
        run->toolBatchAttachments.push_back(
            {{"role", "user"}, {"content", std::move(content)}, {"_img_tokens", imageTokens}});
    if (rejected.isEmpty())
        return {};
    QStringList reasons;
    for (auto it = rejected.cbegin(); it != rejected.cend(); ++it)
        reasons.push_back(QStringLiteral("%1: %2").arg(it.key()).arg(it.value()));
    const QString source = firstSource.isEmpty()
                               ? QString()
                               : QStringLiteral(" First tool-reported source: %1.").arg(firstSource);
    return QStringLiteral(
               "[Image attachments omitted (%1).%2 No image artifact was saved for these "
               "attachments. Read the source again only if needed and still accessible.]\n")
        .arg(reasons.join(QStringLiteral(", ")), source);
}

void QSocAgent::appendBoundedToolMessage(
    const QString &id, const QString &content, const QString &state, const QString &toolName)
{
    const auto    run        = activeRun_;
    const qint64  budget     = toolResultBudgetTokens();
    QString       view       = content;
    json          refs       = json::array();
    const auto    status     = run && run->executingToolStatus ? *run->executingToolStatus
                                                               : QSocTool::classifyResult(content);
    const QString completion = state.isEmpty() ? QSocTool::statusName(status) : state;
    if (QSocRequestUsage::estimateText(content, tokenCounter()) > budget) {
        QString                                       error;
        std::optional<QSocToolResultStore::Reference> saved;
        /* A return the store cannot hold whole is saved as its head and tail. */
        const QString stored = QSocBoundedCapture::bound(content, agentConfig.toolArtifactBytes);
        const bool    sourceTruncated = QSocBoundedCapture::isElided(stored)
                                        || (toolName == QStringLiteral("web_fetch")
                                            && content.endsWith("... (content truncated)"));
        if (toolResultStore_ && toolName != QStringLiteral("tool_output_read"))
            saved = toolResultStore_->publish(
                stored,
                completion,
                sourceTruncated ? QStringLiteral("truncated") : QStringLiteral("unknown"),
                &error,
                [run] { return !run || run->stop.load() == StopMode::None; });
        QString notice;
        if (saved) {
            refs.push_back(artifactReferenceJson(*saved));
            notice = QStringLiteral(
                         "\n[Middle of the output omitted; read artifact %1 (%2 bytes) with "
                         "tool_output_read.]\n")
                         .arg(saved->id)
                         .arg(saved->capturedBytes);
        } else {
            notice = QStringLiteral(
                         "\n[Middle of the output omitted. No readable artifact was saved. "
                         "Completion: %1. %2 Do not repeat side effects without checking state.]\n")
                         .arg(
                             completion,
                             error.isEmpty() ? QStringLiteral("Result storage is unavailable.")
                                             : error);
        }
        view = QSocTokenizer::elideMiddle(content, budget, notice, tokenCounter());
    }
    json message
        = {{"role", "tool"},
           {"tool_call_id", id.toStdString()},
           {"content", view.toStdString()},
           {"_qsoc_status", completion.toStdString()}};
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
