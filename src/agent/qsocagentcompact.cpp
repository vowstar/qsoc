// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "common/qsocconsole.h"
#include "common/qsocmessageauthority.h"

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

json summaryMessage(const json &message)
{
    json result = json::object();
    for (const char *key : {"role", "tool_call_id", "name", "tool_calls"}) {
        if (message.contains(key)) {
            result[key] = message[key];
        }
    }
    const auto content = message.find("content");
    if (content == message.end()) {
        return result;
    }
    if (content->is_string() || content->is_null()) {
        result["content"] = *content;
    } else if (content->is_array()) {
        result["content"] = json::array();
        for (const auto &part : *content) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                result["content"].push_back({{"type", "text"}, {"text", part["text"]}});
            } else {
                result["content"].push_back({{"type", "omitted_nontext_content"}});
            }
        }
    }
    return result;
}

std::optional<QString> formatSummary(
    const json         &history,
    int                 start,
    int                 end,
    qint64              budget  = std::numeric_limits<qint64>::max(),
    QSocTokenizer::Mode counter = QSocTokenizer::Mode::O200k)
{
    QString   result;
    qint64    tokens = 0;
    const int count  = static_cast<int>(history.size());
    for (int i = qMax(0, start); i < qMin(end, count); ++i) {
        if (QSocMessageAuthority::isRuntimeReminder(history[static_cast<size_t>(i)])) {
            continue;
        }
        const QString line   = QString::fromStdString(
                                   summaryMessage(
                                       QSocMessageAuthority::toWire(history[static_cast<size_t>(i)]))
                                       .dump())
                               + QLatin1Char('\n');
        const qint64  needed = QSocRequestUsage::estimateText(line, counter);
        if (needed > budget - tokens) {
            return std::nullopt;
        }
        tokens += needed;
        result += line;
    }
    return result;
}

std::optional<QString> completedSummary(const json &response)
{
    const auto choices = response.find("choices");
    if (choices == response.end() || !choices->is_array() || choices->empty()) {
        return std::nullopt;
    }
    const auto &choice = choices->front();
    if (!choice.is_object()) {
        return std::nullopt;
    }
    const auto finish = choice.find("finish_reason");
    if (finish != choice.end() && !finish->is_null() && *finish != "stop") {
        return std::nullopt;
    }
    const auto message = choice.find("message");
    if (message == choice.end() || !message->is_object()) {
        return std::nullopt;
    }
    for (const char *key : {"tool_calls", "function_call", "refusal"}) {
        const auto field = message->find(key);
        if (field != message->end() && !field->is_null() && !field->empty()
            && !(field->is_string() && field->get_ref<const std::string &>().empty())) {
            return std::nullopt;
        }
    }
    const auto content = message->find("content");
    if (content == message->end() || !content->is_string()) {
        return std::nullopt;
    }
    const QString text = QString::fromStdString(content->get<std::string>());
    return text.trimmed().isEmpty() ? std::nullopt : std::optional<QString>(text);
}

/* The last user message, when it falls before the kept tail. */
QString summarizedUserRequest(const json &history, int start, int boundary)
{
    for (int i = static_cast<int>(history.size()) - 1; i >= start; --i) {
        const auto &message = history[static_cast<size_t>(i)];
        if (message.value("role", "") != "user"
            || QSocMessageAuthority::isRuntimeReminder(message)) {
            continue;
        }
        if (i >= boundary) {
            return {};
        }
        const json content = summaryMessage(message).value("content", json());
        return QStringLiteral("[latest user request]\n")
               + QString::fromStdString(
                   content.is_string() ? content.get<std::string>() : content.dump())
               + QStringLiteral("\n[/latest user request]\n");
    }
    return {};
}

bool pruneHistory(json &history, const QSocAgentConfig &config, QSocTokenizer::Mode counter)
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
            QString::fromStdString(message["content"].get<std::string>()), counter);
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
            QString::fromStdString(message["content"].get<std::string>()), counter);
        if (tokens <= 100) {
            continue;
        }
        saved += tokens
                 - QSocRequestUsage::estimateText(QStringLiteral("[output pruned]"), counter);
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
    for (const auto &message : history) {
        request.messages.push_back(QSocMessageAuthority::toWire(message));
    }
    return request;
}

bool exposesArtifactReader(const json &tools)
{
    for (const auto &tool : tools) {
        const auto function = tool.find("function");
        if (function != tool.end() && function->is_object()
            && function->value("name", std::string()) == "tool_output_read") {
            return true;
        }
    }
    return false;
}

void attachArtifactIndex(json &history, const json &references, bool summarized, bool readerAvailable)
{
    if (references.empty()) {
        return;
    }
    QString text = readerAvailable
                       ? QStringLiteral(
                             "\n\nEarlier context remains available with tool_output_read:\n")
                       : QStringLiteral("\n\nLocal archives (tool_output_read unavailable):\n");
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
    return formatHistoryForSummary(messages, start, end);
}

QString QSocAgent::formatHistoryForSummary(const json &history, int start, int end)
{
    return formatSummary(history, start, end).value_or(QString());
}

QSocRequestSnapshot QSocAgent::compactionRequest(const json &history) const
{
    const QPointer<const QSocAgent> owner(this);
    json                            wire   = json::array();
    const QString                   prompt = requestSystemPrompt();
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
    const ActiveRunPtr        run = activeRun_;
    if (!run) {
        maintenanceStop_ = std::stop_source();
    }
    const std::stop_token maintenance = run ? std::stop_token{} : maintenanceStop_.get_token();
    const auto            stopped     = [owner, run, maintenance] {
        return owner.isNull() || maintenance.stop_requested()
               || (run && (!owner->isCurrentRun(run) || run->stopSource.stop_requested()));
    };
    compactionInFlight_                 = true;
    const QByteArray previousNoProgress = lastNoProgressVersion_;
    const auto       release            = qScopeGuard([owner, previousNoProgress] {
        if (owner) {
            owner->compactionInFlight_   = false;
            owner->compactionCommitting_ = false;
            /* A cancelled attempt says nothing about progress. */
            if (owner->lastCompactionStatus_ == CompactionStatus::Cancelled) {
                owner->lastNoProgressVersion_ = previousNoProgress;
            }
        }
    });
    lastCompactionStatus_               = CompactionStatus::NoProgress;
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
        && !requestExceeds(
            request,
            qint64(window * qMin(agentConfig.pruneThreshold, agentConfig.compactThreshold)))) {
        return 0;
    }
    lastNoProgressVersion_ = candidate.sourceRequestVersion;
    if (force || requestExceeds(request, qint64(window * agentConfig.pruneThreshold))) {
        pruneHistory(candidate.candidateMessages, agentConfig, request.counter);
    }
    const bool summarize = force
                           || requestExceeds(
                               requestWithHistory(request, candidate.candidateMessages),
                               qint64(window * agentConfig.compactThreshold));
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    const json prunedHistory = candidate.candidateMessages;
    bool       summarized    = false;
    json       recentTail    = candidate.candidateMessages;
    /* Only overflow recovery forces without the user asking. */
    QString    fallbackReason;
    const bool overflow = force && !manual;
    if (summarize) {
        const auto summary = summarizeHistory(
            source, candidate.candidateMessages, &recentTail, overflow ? &fallbackReason : nullptr);
        if (!current()) {
            if (owner) {
                owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
            }
            return 0;
        }
        if (summary) {
            const QString text = QString::fromStdString(summary->front().at("content"));
            const QString body = text.mid(QStringLiteral("[Conversation Summary]\n").size());
            if (QSocRequestUsage::estimateText(body, request.counter)
                > qMax<qint64>(1, window / 4)) {
                return 0;
            }
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
    /* Restored context must keep the upper bound under the capacity limit. */
    const qint64 restoreBudget
        = qint64(double(capacityLimit) / QSocRequestUsage::margin(request.counter)) - after;
    if (summarized && agentConfig.contextRestoreEnabled && contextRestoreProvider_
        && restoreBudget > 0) {
        const auto provider = contextRestoreProvider_;
        try {
            candidate.restoreNotice = provider(recentTail, restoreBudget);
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
    if (summarized) {
        if (auto reminder = turnContextMessage(candidate.candidateMessages, false, false)) {
            candidate.candidateMessages.push_back(std::move(*reminder));
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
    if (candidate.afterTokens >= candidate.beforeTokens || candidate.afterTokens > capacityLimit
        || requestExceeds(candidateRequest, capacityLimit)
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
    attachArtifactIndex(
        candidate.candidateMessages,
        candidate.artifactRefs,
        summarized,
        exposesArtifactReader(request.tools));
    const auto archivedRequest = requestWithHistory(request, candidate.candidateMessages);
    if (!current()) {
        if (owner) {
            owner->lastCompactionStatus_ = CompactionStatus::Cancelled;
        }
        return 0;
    }
    candidate.afterTokens = QSocRequestUsage::estimateRequest(archivedRequest);
    if (candidate.afterTokens >= candidate.beforeTokens || candidate.afterTokens > capacityLimit
        || requestExceeds(archivedRequest, capacityLimit)) {
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
    refreshSystemSnapshot(true);
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
    if (owner && !fallbackReason.isEmpty()) {
        emit compactionFellBack(fallbackReason);
    }
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
std::optional<json> QSocAgent::summarizeHistory(
    const json &summarySource, const json &retainedSource, json *recentTail, QString *fallbackReason)
{
    const ActiveRunPtr        run = activeRun_;
    const QPointer<QSocAgent> owner(this);
    const std::stop_token     maintenance = run ? std::stop_token{} : maintenanceStop_.get_token();
    const auto                stopped     = [owner, run, maintenance]() {
        return owner.isNull() || maintenance.stop_requested()
               || (run && (!owner->isCurrentRun(run) || run->stopSource.stop_requested()));
    };
    if (stopped()) {
        return std::nullopt;
    }

    int msgCount = static_cast<int>(retainedSource.size());

    /* Carry the previous summary as an anchor. */
    QString       previousSummary;
    int           summarizeStart = 0;
    const QString summaryMarker  = QStringLiteral("[Conversation Summary]\n");
    if (msgCount > 0) {
        const auto &first = summarySource[0];
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

    const qint64 tailBudget = qBound(1, effectiveContextTokens() / 4, 8000);
    const int    hardCap
        = qMin(agentConfig.keepRecentMessages, msgCount - summarizeStart - minToSummarize);
    int    boundary   = msgCount;
    qint64 tailTokens = 0;
    while (msgCount - boundary < hardCap) {
        int groupStart = boundary - 1;
        while (groupStart > summarizeStart
               && retainedSource[static_cast<size_t>(groupStart)].value("role", "") == "tool") {
            --groupStart;
        }
        if (groupStart < summarizeStart + minToSummarize) {
            break;
        }
        json group = json::array();
        for (int index = groupStart; index < boundary; ++index) {
            group.push_back(retainedSource[static_cast<size_t>(index)]);
        }
        const qint64 groupTokens = QSocRequestUsage::estimateHistory(group, tokenCounter());
        if (boundary < msgCount && groupTokens > tailBudget - tailTokens) {
            break;
        }
        tailTokens += groupTokens;
        boundary = groupStart;
    }
    const qint64 summaryBudget = qMax<qint64>(1, effectiveContextTokens() / 4);

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
        const qint64 summaryWindow = qMax(0, endpoint->contextTokens);
        const qint64 outputReserve
            = endpoint->maxOutputTokens > 0
                  ? endpoint->maxOutputTokens
                  : qBound(qint64(0), qint64(agentConfig.reservedOutputTokens), summaryWindow / 2);
        const qint64 inputBudget = qMax<qint64>(0, summaryWindow - outputReserve);
        const auto   oldContent
            = formatSummary(summarySource, summarizeStart, boundary, inputBudget, tokenCounter());
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
            "## Active User Requirements\n"
            "- [current goals and constraints]\n"
            "## Next Steps\n"
            "- [(none)]\n"
            "</template>\n\n"
            "Rules:\n"
            "- Keep every section, even when empty - write \"(none)\".\n"
            "- Terse bullets, not prose paragraphs.\n"
            "- Preserve exact file paths, commands, error strings, identifiers.\n"
            "- Preserve active user requirements and unresolved work. Merge repeated "
            "requirements.\n"
            "- Remove superseded decisions and completed routine details.\n"
            "- Do not mention the summary process or that context was compacted.\n\n"
            "## Conversation to summarize:\n%1\n\n");

        const QString detailBlock = QStringLiteral(
            "This summary will replace the entire conversation; anything left out is lost. "
            "Before writing, go back over the conversation from the start, including every tool "
            "call and tool result. Keep each todo item with its id and latest status, every "
            "concrete value the user set or the files showed (numbers, addresses, names, paths, "
            "commands) with where it came from, each user requirement in its latest form, each "
            "constraint the user added or lifted, and each decision with its reason.\n");

        const QString summaryPrompt
            = noToolsPreamble + anchorBlock + detailBlock
              + QStringLiteral("Keep the summary within %1 estimated tokens.\n").arg(summaryBudget)
              + templateBlock.arg(oldContent.value_or(QString())) + noToolsPreamble;

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
        summaryRequest.counter  = QSocRequestUsage::counterFor(endpoint->tokenizer);
        if (oldContent && QSocRequestUsage::estimateRequest(summaryRequest) <= inputBudget) {
            const auto generation = summaryRequestUsage_.begin(std::move(summaryRequest));
            ++summaryAttempts_;
            const std::stop_token stopToken = run ? run->stopSource.get_token() : maintenance;
            const json            response  = compactLlm->sendChatCompletionTo(
                *endpoint, summaryMessages, json::array(), 0.1, stopToken, effort);
            if (!owner) {
                return std::nullopt;
            }
            summaryRequestUsage_.complete(generation, response.value("usage", json::object()));
            summaryRequestUsage_.invalidateAnchor();
            const auto choices         = response.find("choices");
            const bool hasFinishReason = choices != response.end() && choices->is_array()
                                         && !choices->empty() && choices->front().is_object()
                                         && choices->front().contains("finish_reason")
                                         && !choices->front()["finish_reason"].is_null();
            if (!hasFinishReason) {
                ++summaryMissingFinishReasons_;
            }
            if (stopped()) {
                return std::nullopt;
            }
            const auto completed = completedSummary(response);
            const auto error     = response.find("error");
            QString    failure;
            if (!completed) {
                failure = error != response.end() && error->is_string()
                              ? QString::fromStdString(error->get<std::string>())
                              : QStringLiteral("empty or invalid summary");
            } else if (
                fallbackReason
                && QSocRequestUsage::estimateText(*completed, tokenCounter()) > summaryBudget) {
                failure = QStringLiteral("summary over budget");
            } else {
                summary    = *completed;
                llmSuccess = true;
            }
            if (!llmSuccess && !fallbackReason) {
                lastCompactionStatus_ = CompactionStatus::Failed;
                return std::nullopt;
            }
            if (!llmSuccess) {
                *fallbackReason = failure;
            }
        }
    }

    /* Mechanical summary when no endpoint can accept the input. */
    if (!llmSuccess) {
        QString carryAnchor;
        if (!previousSummary.isEmpty()) {
            /* Retain the existing anchor. */
            carryAnchor = QStringLiteral("[carried anchor]\n") + previousSummary
                          + QStringLiteral("\n[/carried anchor]\n");
        }
        summary                 = "[Previous conversation summary: " + carryAnchor;
        const QString truncated = QStringLiteral("...(truncated)]");
        if (QSocRequestUsage::estimateText(summary + truncated, tokenCounter()) > summaryBudget) {
            lastCompactionStatus_ = CompactionStatus::Failed;
            return std::nullopt;
        }
        const QString request = fallbackReason
                                    ? summarizedUserRequest(summarySource, summarizeStart, boundary)
                                    : QString();
        if (QSocRequestUsage::estimateText(summary + request + truncated, tokenCounter())
            <= summaryBudget) {
            summary += request;
        }
        for (int i = summarizeStart; i < boundary; i++) {
            const auto extracted = summaryMessage(summarySource[static_cast<size_t>(i)]);
            QString    content   = QString::fromStdString(extracted.at("role").dump())
                                   + QStringLiteral(": ");
            for (const char *field : {"content", "tool_calls", "tool_call_id", "name"}) {
                if (!extracted.contains(field)) {
                    continue;
                }
                if (std::string_view(field) != "content") {
                    content += QString::fromLatin1(field) + QLatin1Char('=');
                }
                content += QString::fromStdString(extracted[field].dump()) + QLatin1Char(' ');
            }
            if (content.length() > 400) {
                content = content.left(280) + QStringLiteral(" ... ") + content.right(80);
            }
            if (QSocRequestUsage::estimateText(summary + content + truncated, tokenCounter())
                > summaryBudget) {
                summary += QStringLiteral("...(truncated)");
                break;
            }
            summary += content + QLatin1Char('\n');
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
        newMessages.push_back(retainedSource[static_cast<size_t>(i)]);
        recentTail->push_back(retainedSource[static_cast<size_t>(i)]);
    }

    return newMessages;
}
