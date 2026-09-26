// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "common/qsocconsole.h"

#include <limits>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QSet>

namespace {

int safeBoundary(const json &history, int proposed)
{
    const int count    = static_cast<int>(history.size());
    int       boundary = qBound(0, proposed, count);
    if (boundary == 0 || boundary == count) {
        return boundary;
    }
    while (boundary > 0 && history[static_cast<size_t>(boundary)].value("role", "") == "tool") {
        --boundary;
    }
    if (history[static_cast<size_t>(boundary)].value("role", "") == "assistant"
        && history[static_cast<size_t>(boundary)].contains("tool_calls")) {
        ++boundary;
        while (boundary < count
               && history[static_cast<size_t>(boundary)].value("role", "") == "tool") {
            ++boundary;
        }
    }
    return boundary;
}

bool completeToolPairs(const json &history)
{
    QSet<QString> pending;
    for (const auto &message : history) {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string()) {
            return false;
        }
        const std::string role = message["role"].get<std::string>();
        if (role == "tool") {
            const auto id = message.find("tool_call_id");
            if (id == message.end() || !id->is_string()
                || !pending.remove(QString::fromStdString(id->get<std::string>()))) {
                return false;
            }
            continue;
        }
        if (!pending.isEmpty()) {
            return false;
        }
        const auto calls = message.find("tool_calls");
        if (calls == message.end()) {
            continue;
        }
        if (role != "assistant" || !calls->is_array()) {
            return false;
        }
        for (const auto &call : *calls) {
            if (!call.is_object() || !call.contains("id") || !call["id"].is_string()) {
                return false;
            }
            const QString id = QString::fromStdString(call["id"].get<std::string>());
            if (id.isEmpty() || pending.contains(id)) {
                return false;
            }
            pending.insert(id);
        }
    }
    return pending.isEmpty();
}

QString formatSummary(const json &history, int start, int end)
{
    QString   result;
    const int count = static_cast<int>(history.size());
    for (int i = qMax(0, start); i < qMin(end, count); ++i) {
        const auto   &message = history[static_cast<size_t>(i)];
        const QString role    = QString::fromStdString(message.value("role", std::string()));
        if (role == QStringLiteral("assistant") && message.contains("tool_calls")) {
            result += QStringLiteral("[Assistant called tools: ");
            for (const auto &call : message["tool_calls"]) {
                if (call.contains("function") && call["function"].is_object()
                    && call["function"].contains("name") && call["function"]["name"].is_string()) {
                    result += QString::fromStdString(call["function"]["name"].get<std::string>())
                              + QLatin1Char(' ');
                }
            }
            result += QStringLiteral("]\n");
            continue;
        }
        const auto value = message.find("content");
        if (value == message.end() || !value->is_string()) {
            continue;
        }
        QString content = QString::fromStdString(value->get<std::string>());
        if (role == QStringLiteral("tool") && content.size() > 2000) {
            content = content.left(1600) + QStringLiteral("\n... (truncated, kept tail) ...\n")
                      + content.right(400);
        }
        result += role == QStringLiteral("tool")
                      ? QStringLiteral("[Tool result: %1]\n").arg(content)
                      : QStringLiteral("[%1]: %2\n").arg(role, content);
    }
    return result;
}

bool pruneHistory(json &history, const QSocAgentConfig &config)
{
    qint64 protectedTokens = 0;
    int    boundary        = 0;
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
        const auto &message = history[static_cast<size_t>(i)];
        if (message.value("role", "") != "tool" || !message.contains("content")
            || !message["content"].is_string() || message.value("_qsoc_result_bounded", false)) {
            continue;
        }
        protectedTokens += QSocRequestUsage::estimateText(
            QString::fromStdString(message["content"].get<std::string>()));
        if (protectedTokens >= config.pruneProtectTokens) {
            boundary = i;
            break;
        }
    }
    json   proposed = history;
    qint64 saved    = 0;
    for (int i = 0; i < boundary; ++i) {
        auto &message = proposed[static_cast<size_t>(i)];
        if (message.value("role", "") != "tool" || !message.contains("content")
            || !message["content"].is_string() || message.value("_qsoc_result_bounded", false)) {
            continue;
        }
        const auto tokens = QSocRequestUsage::estimateText(
            QString::fromStdString(message["content"].get<std::string>()));
        if (tokens <= 100) {
            continue;
        }
        saved += tokens - QSocRequestUsage::estimateText(QStringLiteral("[output pruned]"));
        message["content"] = "[output pruned]";
    }
    if (saved <= 0 || saved < config.pruneMinimumSavings) {
        return false;
    }
    history = std::move(proposed);
    return true;
}

QSocRequestSnapshot requestWithHistory(const QSocRequestSnapshot &source, const json &history)
{
    QSocRequestSnapshot request = source;
    request.messages            = json::array({source.messages.front()});
    for (auto message : history) {
        for (const char *key :
             {"_usage",
              "_img_tokens",
              "_qsoc_tool_state",
              "_qsoc_artifact_refs",
              "_qsoc_result_bounded"}) {
            message.erase(key);
        }
        request.messages.push_back(std::move(message));
    }
    return request;
}

void attachArtifactIndex(json &history, const json &references, bool summarized)
{
    if (references.empty()) {
        return;
    }
    QString text = QStringLiteral("\n\nEarlier context remains available with tool_output_read:\n");
    for (const auto &reference : references) {
        text += QString::fromStdString(reference.at("artifact_id").get<std::string>())
                + QLatin1Char('\n');
    }
    if (summarized) {
        auto &summary                  = history.front();
        summary["_qsoc_artifact_refs"] = references;
        summary["content"]             = summary["content"].get<std::string>() + text.toStdString();
    } else {
        history.insert(
            history.begin(),
            json{
                {"role", "user"},
                {"content", text.toStdString()},
                {"_qsoc_artifact_refs", references}});
    }
}

} // namespace

void QSocAgent::setCompactionCommitter(CompactionCommitter committer)
{
    if (!compactionCommitting_) {
        compactionCommitter_ = std::move(committer);
    }
}

void QSocAgent::setCandidateRestoreProvider(
    std::function<QSocContextRestore(const json &, qint64)> provider)
{
    if (!compactionCommitting_) {
        contextRestoreProvider_ = std::move(provider);
    }
}

int QSocAgent::findSafeBoundary(int proposedIndex) const
{
    return safeBoundary(messages, proposedIndex);
}

QString QSocAgent::formatMessagesForSummary(int start, int end) const
{
    return formatSummary(messages, start, end);
}

QSocRequestSnapshot QSocAgent::compactionRequest(const json &history) const
{
    const QPointer<const QSocAgent> owner(this);
    json                            wire   = json::array();
    const QString                   prompt = buildSystemPromptWithMemory();
    /* Tool permission callbacks can destroy the agent. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!owner) {
        return {};
    }
    wire.push_back({{"role", "system"}, {"content", prompt.toStdString()}});
    injectPerTurnReminders(wire);
    /* The focus probe can destroy the agent. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!owner) {
        return {};
    }
    const QPointer<QLLMService> service = activeRun_ ? activeRun_->llm : llmService;
    const auto                  tools   = getEffectiveToolDefinitions();
    /* Tool definition callbacks can destroy the agent. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!owner) {
        return {};
    }
    return requestWithHistory(requestSnapshot(wire, tools, service.data()), history);
}

QByteArray QSocAgent::compactionRequestVersion(const QSocRequestSnapshot &request) const
{
    const json version
        = {{"history_revision", historyRevision_},
           {"binding_revision", bindingRevision_},
           {"policy_revision", policyRevision_},
           {"messages", request.messages},
           {"tools", request.tools},
           {"route", request.route.toStdString()},
           {"effort", request.effort.toStdString()},
           {"image_tokens", request.imageTokens},
           {"budget", effectiveContextTokens()},
           {"keep", agentConfig.keepRecentMessages},
           {"prune", agentConfig.pruneProtectTokens},
           {"summary_model", agentConfig.compactionModel.toStdString()}};
    return QCryptographicHash::hash(
        QByteArray::fromStdString(version.dump()), QCryptographicHash::Sha256);
}

int QSocAgent::compact()
{
    return performCompaction(true, true);
}

int QSocAgent::compactIfNeeded()
{
    return performCompaction(false, false);
}

int QSocAgent::performCompaction(bool force, bool manual)
{
    if (compactionInFlight_) {
        return 0;
    }
    const QPointer<QSocAgent> owner(this);
    const ActiveRunPtr        run     = activeRun_;
    const auto                stopped = [owner, run] {
        return owner.isNull()
               || (run && (!owner->isCurrentRun(run) || run->stopSource.stop_requested()));
    };
    compactionInFlight_   = true;
    const auto release    = qScopeGuard([owner] {
        if (owner) {
            owner->compactionInFlight_   = false;
            owner->compactionCommitting_ = false;
        }
    });
    lastCompactionStatus_ = CompactionStatus::NoProgress;
    if (stopped()) {
        lastCompactionStatus_ = CompactionStatus::Cancelled;
        return 0;
    }
    const json source = messages;
    if (source.empty()) {
        return 0;
    }
    if (!completeToolPairs(source)) {
        lastCompactionStatus_ = CompactionStatus::Failed;
        return 0;
    }
    CompactionCandidate candidate;
    candidate.sourceRevision  = historyRevision_;
    candidate.bindingRevision = bindingRevision_;
    candidate.policyRevision  = policyRevision_;
    const auto request        = compactionRequest(source);
    if (stopped()) {
        return 0;
    }
    candidate.sourceRequestVersion = compactionRequestVersion(request) + (force ? 'F' : 'A');
    candidate.beforeTokens         = QSocRequestUsage::estimateRequest(request);
    if (!manual && candidate.sourceRequestVersion == lastNoProgressVersion_) {
        return 0;
    }
    const auto current = [&] {
        return !stopped() && candidate.sourceRevision == owner->historyRevision_
               && candidate.bindingRevision == owner->bindingRevision_
               && candidate.policyRevision == owner->policyRevision_ && source == owner->messages;
    };
    candidate.candidateMessages = source;
    const qint64 window         = effectiveContextTokens();
    const qint64 capacityLimit  = window - window / 10;
    if (!force
        && requestUsage_.estimateNext(request)
               <= window * qMin(agentConfig.pruneThreshold, agentConfig.compactThreshold)) {
        return 0;
    }
    lastNoProgressVersion_ = candidate.sourceRequestVersion;
    if (force || requestUsage_.estimateNext(request) > window * agentConfig.pruneThreshold) {
        pruneHistory(candidate.candidateMessages, agentConfig);
    }
    const qint64 prunedTokens = QSocRequestUsage::estimateRequest(
        requestWithHistory(request, candidate.candidateMessages));
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    const json prunedHistory = candidate.candidateMessages;
    bool       summarized    = false;
    json       recentTail    = candidate.candidateMessages;
    if (force || prunedTokens > window * agentConfig.compactThreshold) {
        const auto summary = summarizeHistory(candidate.candidateMessages, &recentTail);
        if (!current()) {
            if (owner) {
                owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
            }
            return 0;
        }
        if (summary) {
            candidate.candidateMessages = *summary;
            summarized                  = true;
        } else if (lastCompactionStatus_ == CompactionStatus::Failed) {
            return 0;
        }
    }
    for (const auto &reference : artifactReferences(source)) {
        candidate.artifactRefs.push_back(artifactReferenceJson(reference));
    }
    qint64 after = QSocRequestUsage::estimateRequest(
        requestWithHistory(request, candidate.candidateMessages));
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    if (summarized && agentConfig.contextRestoreEnabled && contextRestoreProvider_
        && after < capacityLimit) {
        const auto provider = contextRestoreProvider_;
        try {
            candidate.restoreNotice = provider(recentTail, capacityLimit - after);
        } catch (...) {
            if (owner) {
                owner->lastCompactionStatus_ = CompactionStatus::Failed;
            }
            return 0;
        }
        if (!current()) {
            if (owner) {
                owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
            }
            return 0;
        }
        for (const auto &message : QSocContextRestoreBuilder::toMessages(candidate.restoreNotice)) {
            candidate.candidateMessages.push_back(message);
        }
    }
    const auto candidateRequest = requestWithHistory(request, candidate.candidateMessages);
    candidate.afterTokens       = QSocRequestUsage::estimateRequest(candidateRequest);
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    if (candidate.afterTokens >= candidate.beforeTokens
        || qMax(candidate.afterTokens, requestUsage_.estimateNext(candidateRequest)) > capacityLimit
        || !completeToolPairs(candidate.candidateMessages)) {
        lastNoProgressVersion_ = candidate.sourceRequestVersion;
        return 0;
    }
    const auto store = toolResultStore_;
    if (!store || !store->isBound()) {
        lastCompactionStatus_ = CompactionStatus::Failed;
        return 0;
    }
    for (const auto &reference : artifactReferences(source)) {
        const auto page = store->read(reference.id, 0, 1);
        if (!page || page->reference.sha256 != reference.sha256
            || page->reference.capturedBytes != reference.capturedBytes) {
            lastCompactionStatus_ = CompactionStatus::Failed;
            return 0;
        }
    }
    json         removed         = json::array();
    const size_t summarizedCount = summarized ? source.size() - recentTail.size() : 0;
    for (size_t index = 0; index < source.size(); ++index) {
        if (index < summarizedCount || source[index] != prunedHistory[index]) {
            removed.push_back(source[index]);
        }
    }
    QString    archiveError;
    const auto archive = store->publish(
        QString::fromStdString(removed.dump()),
        QStringLiteral("ok"),
        QStringLiteral("unknown"),
        &archiveError,
        current);
    if (!archive) {
        if (owner) {
            owner->lastCompactionStatus_ = current() ? CompactionStatus::Failed
                                                     : CompactionStatus::Cancelled;
        }
        return 0;
    }
    bool       retainArchive  = false;
    const auto discardArchive = qScopeGuard([&] {
        if (!retainArchive && !store->discardPublished(*archive, &archiveError)) {
            QSocConsole::warn() << "Compaction artifact cleanup failed:" << archiveError;
        }
    });
    candidate.artifactRefs.push_back(artifactReferenceJson(*archive));
    attachArtifactIndex(candidate.candidateMessages, candidate.artifactRefs, summarized);
    const auto archivedRequest = requestWithHistory(request, candidate.candidateMessages);
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    candidate.afterTokens = QSocRequestUsage::estimateRequest(archivedRequest);
    if (candidate.afterTokens >= candidate.beforeTokens
        || qMax(candidate.afterTokens, requestUsage_.estimateNext(archivedRequest))
               > capacityLimit) {
        lastNoProgressVersion_ = candidate.sourceRequestVersion;
        return 0;
    }
    const auto finalRequest = compactionRequest(source);
    if (!current()
        || compactionRequestVersion(finalRequest) + (force ? 'F' : 'A')
               != candidate.sourceRequestVersion) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    compactionCommitting_ = true;
    const auto committer  = compactionCommitter_;
    bool       saved      = true;
    try {
        retainArchive = bool(committer);
        saved         = !committer || committer(candidate);
    } catch (...) {
        saved = false;
    }
    retainArchive = retainArchive || saved;
    if (!owner) {
        return 0;
    }
    compactionCommitting_ = false;
    if (!saved) {
        lastNoProgressVersion_ = candidate.sourceRequestVersion;
        lastCompactionStatus_  = CompactionStatus::Failed;
        return 0;
    }
    messages = std::move(candidate.candidateMessages);
    ++historyRevision_;
    ++historyAccountingRevision_;
    streamPrevTokensEstimate = estimateMessagesTokens();
    requestUsage_.invalidateAnchor();
    lastApplied_ = std::move(candidate.restoreNotice);
    lastNoProgressVersion_.clear();
    lastCompactionStatus_ = CompactionStatus::Committed;
    const int before      = static_cast<int>(
        qMin<qint64>(candidate.beforeTokens, std::numeric_limits<int>::max()));
    const int afterTokens = static_cast<int>(
        qMin<qint64>(candidate.afterTokens, std::numeric_limits<int>::max()));
    emit compacting(summarized ? 2 : 1, before, afterTokens);
    /* Signal handlers can destroy the agent. */
    // cppcheck-suppress knownConditionTrueFalse
    if (owner && !lastApplied_.isEmpty()) {
        emit contextRestored();
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (owner && run && run->stopSource.stop_requested()) {
        owner->abort();
    }
    return static_cast<int>(
        qMin<qint64>(candidate.beforeTokens - candidate.afterTokens, std::numeric_limits<int>::max()));
}

void QSocAgent::compressHistoryIfNeeded(const ActiveRunPtr &run)
{
    if (isCurrentRun(run) && !run->stopSource.stop_requested()) {
        performCompaction(false, false);
    }
}
std::optional<json> QSocAgent::summarizeHistory(const json &sourceMessages, json *recentTail)
{
    const ActiveRunPtr        run = activeRun_;
    const QPointer<QSocAgent> owner(this);
    const auto                stopped = [owner, run]() {
        return owner.isNull()
               || (run && (!owner->isCurrentRun(run) || run->stopSource.stop_requested()));
    };
    if (stopped()) {
        return std::nullopt;
    }

    int msgCount = static_cast<int>(sourceMessages.size());

    /* Carry the previous summary as an anchor. */
    QString       previousSummary;
    int           summarizeStart = 0;
    const QString summaryMarker  = QStringLiteral("[Conversation Summary]\n");
    if (msgCount > 0) {
        const auto &first = sourceMessages[0];
        if (first.contains("role") && first["role"] == "user" && first.contains("content")
            && first["content"].is_string()) {
            const QString firstContent = QString::fromStdString(first["content"].get<std::string>());
            if (firstContent.startsWith(summaryMarker)) {
                previousSummary = firstContent.mid(summaryMarker.size());
                summarizeStart  = 1;
            }
        }
    }

    /* A handful of huge tool results can push tokens past the threshold
     * with fewer than keepRecentMessages messages total. Shrink the keep
     * window dynamically so something is always available to summarize,
     * and only bail when there genuinely is not enough to split. */
    const int minToSummarize = 3;
    if (msgCount - summarizeStart < minToSummarize + 1) {
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Layer 2: Cannot compact, only %1 new messages]")
                                   .arg(msgCount - summarizeStart));
            if (stopped()) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /* Token-budget tail walk: count back from the end until we hit
     * either keepRecentMessages or the recent-zone token budget. A
     * single 50KB tool output should not be allowed to dominate the
     * post-compact context just because it lives in the last N
     * positions. */
    const int tailBudget = qBound(2000, agentConfig.maxContextTokens / 4, 8000);
    const int hardCap
        = qMin(agentConfig.keepRecentMessages, msgCount - summarizeStart - minToSummarize);
    int effectiveKeep = 0;
    int tailTokens    = 0;
    for (int i = msgCount - 1; i >= summarizeStart && effectiveKeep < hardCap; --i) {
        const auto &msg = sourceMessages[static_cast<size_t>(i)];
        QString     approx;
        if (msg.contains("content") && msg["content"].is_string()) {
            approx = QString::fromStdString(msg["content"].get<std::string>());
        }
        const int approxTokens = estimateTokens(approx);
        if (effectiveKeep >= 1 && tailTokens + approxTokens > tailBudget) {
            break;
        }
        tailTokens += approxTokens;
        effectiveKeep++;
    }
    if (effectiveKeep < 1) {
        effectiveKeep = qMin(1, hardCap);
    }

    /* Determine boundary: keep recent messages */
    int proposedBoundary = msgCount - effectiveKeep;
    int boundary         = safeBoundary(sourceMessages, proposedBoundary);

    if (boundary <= summarizeStart) {
        return std::nullopt;
    }

    /* Format old messages for summarization (skip the anchor itself) */
    QString oldContent = formatSummary(sourceMessages, summarizeStart, boundary);

    /* Try LLM summarization if service is available and not circuit-broken */
    QString summary;
    bool    llmSuccess = false;

    const QPointer<QLLMService>   compactLlm = run ? run->llm : llmService;
    const QString                 effort     = agentConfig.effortLevel;
    std::optional<LLMModelConfig> endpoint;
    if (!compactLlm.isNull() && compactLlm->hasEndpoint()) {
        endpoint = compactLlm->getCurrentModelConfig();
    }
    if (!agentConfig.compactionModel.isEmpty()) {
        if (compactLlm.isNull()
            || !compactLlm->availableModels().contains(agentConfig.compactionModel)) {
            QSocConsole::warn() << "Unknown compaction model:" << agentConfig.compactionModel;
            lastCompactionStatus_ = CompactionStatus::Failed;
            return std::nullopt;
        }
        endpoint = compactLlm->getModelConfig(agentConfig.compactionModel);
    }
    if (!compactLlm.isNull() && endpoint.has_value()) {
        const QString noToolsPreamble = QStringLiteral(
            "CRITICAL: Respond with TEXT ONLY. Do NOT call any tools.\n"
            "You already have all context above. Tool calls will be rejected\n"
            "and waste this turn. Reply must be plain markdown.\n\n");

        QString anchorBlock;
        if (!previousSummary.isEmpty()) {
            anchorBlock
                = QString(
                      "Update the anchored summary below using the conversation history above.\n"
                      "Preserve still-true details, remove stale details, merge in new facts.\n"
                      "<previous-summary>\n%1\n</previous-summary>\n\n")
                      .arg(previousSummary);
        } else {
            anchorBlock = QStringLiteral(
                "Create a new anchored summary from the conversation history above.\n\n");
        }

        const QString templateBlock = QStringLiteral(
            "Output exactly the Markdown structure shown inside <template>.\n"
            "Do not include the <template> tags in your response.\n\n"
            "<template>\n"
            "## Task Overview\n"
            "- [single-sentence summary]\n"
            "## Current State\n"
            "- [(none)]\n"
            "## Key Files and Paths\n"
            "- [path: why it matters, or (none)]\n"
            "## Errors and Fixes\n"
            "- [error: fix, or (none)]\n"
            "## Decisions Made\n"
            "- [decision and why, or (none)]\n"
            "## Important Context\n"
            "- [(none)]\n"
            "## Actions Already Completed\n"
            "- [tool call: outcome, or (none)]\n"
            "## All User Messages\n"
            "- [verbatim, oldest first]\n"
            "## Next Steps\n"
            "- [(none)]\n"
            "</template>\n\n"
            "Rules:\n"
            "- Keep every section, even when empty - write \"(none)\".\n"
            "- Terse bullets, not prose paragraphs.\n"
            "- Preserve exact file paths, commands, error strings, identifiers.\n"
            "- Reproduce every user message verbatim - they are short and critical.\n"
            "- Do not mention the summary process or that context was compacted.\n\n"
            "## Conversation to summarize:\n%1\n\n");

        const QString summaryPrompt = noToolsPreamble + anchorBlock + templateBlock.arg(oldContent)
                                      + noToolsPreamble;

        /* Build messages for the summarization request */
        json summaryMessages = json::array();
        summaryMessages.push_back(
            {{"role", "system"},
             {"content",
              "You are a precise conversation summarizer. Output only plain markdown - "
              "never call tools."}});
        summaryMessages.push_back({{"role", "user"}, {"content", summaryPrompt.toStdString()}});

        QSocRequestSnapshot summaryRequest;
        summaryRequest.messages = summaryMessages;
        summaryRequest.effort   = effort;
        const int summaryWindow = qMax(1024, endpoint->contextTokens);
        const int outputReserve = qBound(
            0, qMax(agentConfig.reservedOutputTokens, endpoint->maxOutputTokens), summaryWindow / 2);
        if (QSocRequestUsage::estimateRequest(summaryRequest) <= summaryWindow - outputReserve) {
            const std::stop_token stopToken = run ? run->stopSource.get_token() : std::stop_token{};
            const json            response  = compactLlm->sendChatCompletionTo(
                *endpoint, summaryMessages, json::array(), 0.1, stopToken, effort);
            if (stopped()) {
                return std::nullopt;
            }
            if (response.contains("choices") && response["choices"].is_array()
                && !response["choices"].empty()) {
                const auto &choice = response["choices"][0];
                if (choice.contains("message") && choice["message"].is_object()) {
                    const auto &message = choice["message"];
                    if (message.contains("content") && message["content"].is_string()) {
                        summary    = QString::fromStdString(message["content"].get<std::string>());
                        llmSuccess = !message.contains("tool_calls")
                                     || message["tool_calls"].empty();
                    }
                }
            }
            if (!llmSuccess || summary.trimmed().isEmpty()) {
                lastCompactionStatus_ = CompactionStatus::Failed;
                return std::nullopt;
            }
        }
    }

    /* Mechanical summary when no endpoint can accept the input. */
    if (!llmSuccess) {
        const int summaryBudgetTokens
            = qMax(2048, static_cast<int>(agentConfig.maxContextTokens / 4));
        const int summaryBudgetChars = summaryBudgetTokens * 4; /* coarse token->char */
        QString   carryAnchor;
        if (!previousSummary.isEmpty()) {
            /* Retain the existing anchor. */
            carryAnchor = QStringLiteral("[carried anchor]\n") + previousSummary
                          + QStringLiteral("\n[/carried anchor]\n");
        }
        summary = "[Previous conversation summary: " + carryAnchor;
        for (int i = summarizeStart; i < boundary; i++) {
            if (summary.size() >= summaryBudgetChars) {
                summary += "...(truncated)";
                break;
            }
            const auto &msg = sourceMessages[static_cast<size_t>(i)];
            if (msg.contains("role") && msg.contains("content") && msg["content"].is_string()) {
                QString role    = QString::fromStdString(msg["role"].get<std::string>());
                QString content = QString::fromStdString(msg["content"].get<std::string>());
                /* Keep head + tail so file paths and error tails
                 * survive the truncation; left(100) alone routinely
                 * decapitated commands and stack traces. */
                if (content.length() > 400) {
                    content = content.left(280) + " ... " + content.right(80);
                }
                summary += role + ": " + content + "; ";
            }
        }
        summary += "]";
    }

    /* Build new message history: summary + recent messages */
    json newMessages = json::array();
    newMessages.push_back(
        {{"role", "user"},
         {"content", QString("[Conversation Summary]\n%1").arg(summary).toStdString()}});

    *recentTail = json::array();
    for (int i = boundary; i < msgCount; i++) {
        newMessages.push_back(sourceMessages[static_cast<size_t>(i)]);
        recentTail->push_back(sourceMessages[static_cast<size_t>(i)]);
    }

    return newMessages;
}
