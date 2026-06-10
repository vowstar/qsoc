// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"

#include "agent/qsocgoal.h"
#include "agent/qsocgoalprompt.h"
#include "agent/qsochookmanager.h"
#include "agent/qsochooktypes.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/qlongtaskmonitor.h"
#include "common/qsocconsole.h"

#include <QElapsedTimer>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSet>
#include <QSysInfo>
#include <QTextStream>

namespace {

/* Re-injected as a system message every turn while in plan mode. States
 * the read-only constraint and the explore -> clarify -> exit loop. */
const char *const kPlanModeReminder
    = "Plan mode is active. You MUST NOT modify anything: no file edits, "
      "no shell writes, no commits, no config changes. This supersedes "
      "other instructions. Work the loop: (1) explore read-only (read "
      "files, search, run read-only shell, spawn read-only sub-agents); "
      "(2) when the approach is ambiguous or needs a non-trivial choice, "
      "call ask_user to clarify with the user, repeating as many rounds "
      "as needed; (3) only once the plan is concrete and the user's "
      "intent is pinned, call exit_plan_mode to present it for approval. "
      "End every turn with either ask_user or exit_plan_mode.";

/* Injected each turn while the terminal is unfocused (user not watching).
 * Steers away from blocking ask_user prompts so an unattended run keeps
 * moving. Never persisted. */
const char *const kNotWatchingReminder
    = "The user is not actively watching the terminal right now. Do not "
      "pause for non-critical clarifications: prefer the most reasonable, "
      "reversible default, state the assumption, and keep going. Reserve "
      "ask_user for a genuinely blocking, irreversible decision.";

} // namespace

QSocAgent::QSocAgent(
    QObject *parent, QLLMService *llmService, QSocToolRegistry *toolRegistry, QSocAgentConfig config)
    : QObject(parent)
    , llmService(llmService)
    , toolRegistry(toolRegistry)
    , agentConfig(std::move(config))
    , messages(json::array())
    , heartbeatTimer(new QTimer(this))
{
    /* UI keepalive: every 5 s while streaming, emit heartbeat +
     * token usage so the status line can tick. Stall detection lives
     * in the per-iteration QLongTaskMonitor, not here. */
    heartbeatTimer->setInterval(5000);
    connect(heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (isStreaming) {
            int  elapsed = static_cast<int>(runElapsedTimer.elapsed() / 1000);
            emit heartbeat(streamIteration, elapsed);
            emit tokenUsage(totalInputTokens.load(), totalOutputTokens.load());
        }
    });
}

QSocAgent::~QSocAgent()
{
    /* Qt automatically disconnects signals when either sender or receiver is destroyed */
    /* No manual disconnect needed - doing so can cause crashes if llmService is already destroyed */
}

QString QSocAgent::toolDenyReason(const QString &name) const
{
    /* Sub-agents must never spawn further sub-agents: closes the
     * recursion door regardless of allowlist. */
    if (agentConfig.isSubAgent && name == QStringLiteral("agent")) {
        return QStringLiteral("sub-agents cannot spawn further sub-agents");
    }
    /* Plan mode is owned by the main agent only: a child (which shares
     * the registry and its callbacks) must not enter or approve/exit
     * plan mode on the parent's behalf. */
    if (agentConfig.isSubAgent
        && (name == QStringLiteral("enter_plan_mode") || name == QStringLiteral("exit_plan_mode"))) {
        return QStringLiteral("plan mode is controlled by the main agent, not sub-agents");
    }
    /* exit_plan_mode is only meaningful inside plan mode; enter_plan_mode
     * only outside it. Keep each off the menu when irrelevant. */
    if (name == QStringLiteral("exit_plan_mode") && !agentConfig.planMode) {
        return QStringLiteral("plan mode is not active");
    }
    if (name == QStringLiteral("enter_plan_mode") && agentConfig.planMode) {
        return QStringLiteral("plan mode is already active");
    }
    /* Allowlist gate: empty list inherits everything. */
    if (!agentConfig.toolsAllow.isEmpty() && !agentConfig.toolsAllow.contains(name)) {
        return QStringLiteral("not in this agent's allowed tool list");
    }
    /* Denylist gate: applied AFTER allowlist. Used to subtract a
     * couple of tools from "inherit everything" or from a broad
     * allowlist. */
    if (agentConfig.toolsDeny.contains(name)) {
        return QStringLiteral("denied by this agent's tool denylist");
    }
    /* Plan-mode gate: only read-only tools run. The shell passes here
     * (its per-command safety is LLM-judged at dispatch) and the spawn
     * tool passes (children inherit planMode and are read-only too); the
     * plan tools and every other read-only tool pass via isReadOnly().
     * Everything that can mutate is rejected. */
    if (agentConfig.planMode) {
        const bool shellJudged
            = (name == QStringLiteral("bash") || name == QStringLiteral("remote_shell_bash"));
        const bool      spawnOk = (name == QStringLiteral("agent"));
        const QSocTool *tool    = toolRegistry != nullptr ? toolRegistry->getTool(name) : nullptr;
        if (!shellJudged && !spawnOk && (tool == nullptr || !tool->isReadOnly())) {
            return QStringLiteral(
                "plan mode is read-only; call exit_plan_mode to get approval first");
        }
    }
    return {};
}

bool QSocAgent::isToolAllowed(const QString &name) const
{
    return toolDenyReason(name).isEmpty();
}

nlohmann::json QSocAgent::getEffectiveToolDefinitions() const
{
    if (toolRegistry == nullptr) {
        return json::array();
    }
    return filterAllowedTools(toolRegistry->getToolDefinitions());
}

nlohmann::json QSocAgent::filterAllowedTools(const nlohmann::json &defs) const
{
    /* Fast path only when no gate applies: no allowlist, not a sub-agent,
     * and not in plan mode. Plan mode must run the per-tool loop so
     * mutating tools are dropped from the sent list. */
    if (agentConfig.toolsAllow.isEmpty() && !agentConfig.isSubAgent && !agentConfig.planMode) {
        return defs;
    }
    json filtered = json::array();
    for (const auto &def : defs) {
        if (!def.contains("function") || !def["function"].contains("name")) {
            filtered.push_back(def);
            continue;
        }
        const QString name = QString::fromStdString(def["function"]["name"].get<std::string>());
        if (isToolAllowed(name)) {
            filtered.push_back(def);
        }
    }
    return filtered;
}

QString QSocAgent::run(const QString &userQuery)
{
    QString prompt = userQuery;
    QString blockReason;
    if (!firePromptSubmitHook(&prompt, &blockReason)) {
        return QStringLiteral("[user_prompt_submit hook blocked: %1]").arg(blockReason);
    }

    /* Add user message to history */
    addMessage("user", prompt);

    planNudgeUsed = false;

    /* Rank relevant memories for this turn (synchronous, before the
     * loop). Filled into recallBlock_ and injected as a non-persisted
     * reminder each iteration. */
    computeRecallForTurn(prompt);

    /* Agent loop */
    int       iteration = 0;
    const int turnCap   = agentConfig.maxTurnsOverride > 0 ? agentConfig.maxTurnsOverride
                                                           : agentConfig.maxIterations;

    /* Snapshot the rolling token estimate so each iteration's
     * delta can be charged against the active goal's budget. */
    int           prevTokensEstimate = estimateMessagesTokens();
    QElapsedTimer iterationTimer;
    iterationTimer.start();

    while (iteration < turnCap) {
        iteration++;

        /* Check and compress history if needed */
        compressHistoryIfNeeded();

        int currentTokens = estimateMessagesTokens();

        if (agentConfig.verbose) {
            QString info = QString("[Iteration %1 | Tokens: %2/%3 (%4%) | Messages: %5]")
                               .arg(iteration)
                               .arg(currentTokens)
                               .arg(agentConfig.maxContextTokens)
                               .arg(100.0 * currentTokens / agentConfig.maxContextTokens, 0, 'f', 1)
                               .arg(messages.size());
            emit    verboseOutput(info);
        }

        /* Process one iteration */
        bool isComplete = processIteration();

        accountGoalUsageForIteration(prevTokensEstimate, iterationTimer);

        if (isComplete) {
            QString finalText;
            if (!messages.empty()) {
                auto lastMessage = messages.back();
                if (lastMessage["role"] == "assistant" && lastMessage.contains("content")
                    && lastMessage["content"].is_string()) {
                    finalText = QString::fromStdString(lastMessage["content"].get<std::string>());
                } else {
                    finalText = QStringLiteral("[Agent completed without final message]");
                }
            } else {
                finalText = QStringLiteral("[Agent completed without final message]");
            }
            if (maybeQueueGoalContinuation()) {
                /* The continuation prompt was appended as a user
                 * message; loop back so processIteration handles
                 * the next turn. Token budget enforcement caps the
                 * total spend; turnCap caps the iteration count. */
                continue;
            }
            if (maybeQueuePlanModeNudge()) {
                continue;
            }
            return finalText;
        }
    }

    /* Cap source decides the wording: per-agent override gets the
     * "max turns limit" message; the default safety cap retains the
     * legacy phrasing so existing logs still match. */
    if (agentConfig.maxTurnsOverride > 0) {
        return QString("Reached max turns limit (%1)").arg(turnCap);
    }
    return QString("[Agent safety limit reached (%1 iterations)]").arg(turnCap);
}

void QSocAgent::runStream(const QString &userQuery)
{
    if (!llmService || !toolRegistry) {
        emit runError("LLM service or tool registry not configured");
        return;
    }

    fireSessionStartHookOnce();

    QString prompt = userQuery;
    QString blockReason;
    if (!firePromptSubmitHook(&prompt, &blockReason)) {
        emit runError(QStringLiteral("user_prompt_submit hook blocked: %1").arg(blockReason));
        return;
    }

    /* Add user message to history */
    addMessage("user", prompt);

    /* Setup streaming */
    isStreaming = true;
    ++streamEpoch_; /* invalidate any deferred retry from a prior run */
    streamIteration           = 0;
    currentRetryCount         = 0;
    contextOverflowRetryCount = 0;
    emptyResponseRetryCount   = 0;
    planNudgeUsed             = false;
    streamFinalContent.clear();
    abortRequested = false;

    /* Rank relevant memories for this turn. Runs after streamEpoch_ is
     * bumped and abortRequested cleared, so the synchronous selector's
     * nested event loop cannot dispatch a stale deferred retry from a
     * prior run. Fills recallBlock_, injected as a non-persisted reminder
     * each iteration. */
    computeRecallForTurn(prompt);

    /* Reset token counters for this run */
    totalInputTokens  = 0;
    totalOutputTokens = 0;

    /* Snapshot the rolling token estimate and start the wall-clock
     * timer so each stream iteration's delta can be charged against
     * the active goal's budget. */
    streamPrevTokensEstimate = estimateMessagesTokens();
    streamIterationTimer.start();

    /* Start timing */
    runElapsedTimer.start();
    heartbeatTimer->start();

    /* Connect to LLM streaming signals (use member functions for UniqueConnection) */
    connect(
        llmService,
        &QLLMService::streamChunk,
        this,
        &QSocAgent::handleStreamChunk,
        Qt::UniqueConnection);

    connect(
        llmService,
        &QLLMService::streamComplete,
        this,
        &QSocAgent::handleStreamComplete,
        Qt::UniqueConnection);

    connect(
        llmService,
        &QLLMService::streamError,
        this,
        &QSocAgent::handleStreamError,
        Qt::UniqueConnection);

    connect(
        llmService,
        &QLLMService::streamReasoningChunk,
        this,
        &QSocAgent::handleReasoningChunk,
        Qt::UniqueConnection);

    /* Start first iteration */
    processStreamIteration();
}

void QSocAgent::handleStreamChunk(const QString &chunk)
{
    if (streamMonitor != nullptr) {
        streamMonitor->notifyProgress();
    }

    /* Estimate output tokens from this chunk */
    int chunkTokens = estimateTokens(chunk);
    totalOutputTokens.fetch_add(chunkTokens);

    emit contentChunk(chunk);
}

void QSocAgent::handleReasoningChunk(const QString &chunk)
{
    if (streamMonitor != nullptr) {
        streamMonitor->notifyProgress();
    }
    int chunkTokens = estimateTokens(chunk);
    totalOutputTokens.fetch_add(chunkTokens);
    emit reasoningChunk(chunk);
}

QSocAgent::RetryKind QSocAgent::classifyRetry(const QString &error)
{
    /* Provider pushback: wait longer. QLLMService prefixes HTTP errors
     * with `[HTTP <code>] `, so dispatch on the code, not free text.
     * 529 is a non-standard provider "overloaded" status. */
    if (error.startsWith(QStringLiteral("[HTTP 429]"))
        || error.startsWith(QStringLiteral("[HTTP 503]"))
        || error.startsWith(QStringLiteral("[HTTP 529]"))) {
        return RetryKind::RateLimited;
    }
    /* For any other HTTP-coded error, classify ONLY by the code, never
     * by the body: a 401/403/400 whose provider message happens to
     * contain "timeout"/"network"/"connection" must NOT be retried.
     * 5xx is a transient server fault worth a short backoff; remaining
     * 4xx (auth, bad request) is a client error that surfaces at once. */
    if (error.startsWith(QStringLiteral("[HTTP 5"))) {
        return RetryKind::Transient;
    }
    if (error.startsWith(QStringLiteral("[HTTP "))) {
        return RetryKind::None;
    }
    /* No HTTP prefix: a transport-level failure (no response arrived).
     * Here the free-text match is the only signal available. */
    if (error.contains(QStringLiteral("overloaded"), Qt::CaseInsensitive)) {
        return RetryKind::RateLimited;
    }
    if (error.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("network"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("connection"), Qt::CaseInsensitive)) {
        return RetryKind::Transient;
    }
    return RetryKind::None;
}

int QSocAgent::backoffDelayMs(int attempt, bool rateLimit)
{
    /* Exponential base * 2^(attempt-1), capped, plus jitter. Rate-limit
     * waits start higher because the server explicitly asked us to slow
     * down. Jitter de-synchronizes many sub-agents that hit the same
     * limit on the same tick, avoiding a thundering-herd re-retry. */
    const int        safeAttempt = qMax(attempt, 1);
    const qint64     baseMs      = rateLimit ? 2000 : 500;
    constexpr qint64 capMs       = 30000;
    const int        shift       = qMin(safeAttempt - 1, 16); /* avoid overflow */
    const qint64     delay       = qMin(baseMs * (qint64{1} << shift), capMs);
    const int        jitter = QRandomGenerator::global()->bounded(static_cast<int>(baseMs / 2) + 1);
    return static_cast<int>(delay) + jitter;
}

void QSocAgent::handleStreamError(const QString &error)
{
    /* Check if this error was caused by user abort */
    if (abortRequested) {
        if (hasPendingRequests()) {
            /* ESC with input queued: drop this iteration's reply and
             * continue the run with the queued message. */
            abortRequested = false;
            processStreamIteration();
            return;
        }
        isStreaming = false;
        heartbeatTimer->stop();
        teardownStreamMonitor(QStringLiteral("user abort"));
        abortRequested = false;
        emit runAborted(streamFinalContent);
        return;
    }

    /* Reactive compaction: when the server rejects the request because
     * the prompt overshoots the context window, force a compact and
     * retry once. qllmservice prefixes the error with "[HTTP <code>] ",
     * so we dispatch on the status code, never on free-text substring.
     * HTTP 413 is always overflow; 400 only counts when the body says
     * so (some providers, e.g. DeepSeek, return 400 for over-long
     * prompts with a generic "Bad Request" header but a specific body).
     * Auth/permission failures (401/403) must surface to the user
     * verbatim; retrying a bad key is pointless. */
    static constexpr int maxCompactRetries = 2;
    const bool hasContextPhrase  = error.contains("Entity Too Large", Qt::CaseInsensitive)
                                   || error.contains("context_length_exceeded", Qt::CaseInsensitive)
                                   || error.contains("maximum context length", Qt::CaseInsensitive)
                                   || error.contains("prompt is too long", Qt::CaseInsensitive);
    const bool isHttp413         = error.startsWith("[HTTP 413]");
    const bool isHttp400         = error.startsWith("[HTTP 400]");
    const bool isContextOverflow = isHttp413 || (isHttp400 && hasContextPhrase)
                                   || (hasContextPhrase && !error.startsWith("[HTTP "));
    if (isContextOverflow && contextOverflowRetryCount < maxCompactRetries) {
        contextOverflowRetryCount++;
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Context overflow %1/%2 — forcing compact and retrying]")
                                   .arg(contextOverflowRetryCount)
                                   .arg(maxCompactRetries));
        }
        emit compacting(2, estimateMessagesTokens(), 0);
        compact();
        emit compacting(2, 0, estimateMessagesTokens());
        processStreamIteration();
        return;
    }

    /* Classify the error. Rate-limit (429/503/529/overloaded) and
     * transient (timeout/network/connection) errors are retried; the
     * provider's 429 backpressure plus this client-side backoff is the
     * real flow-control mechanism once sub-agent concurrency is
     * unbounded. */
    const RetryKind kind = classifyRetry(error);

    if (kind != RetryKind::None && currentRetryCount < agentConfig.maxRetries) {
        currentRetryCount++;
        const bool rateLimited = (kind == RetryKind::RateLimited);
        const int  delayMs     = backoffDelayMs(currentRetryCount, rateLimited);

        /* Always emit retrying signal for UI feedback */
        emit retrying(currentRetryCount, agentConfig.maxRetries, error);

        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Retry %1/%2 in %3ms: %4]")
                                   .arg(currentRetryCount)
                                   .arg(agentConfig.maxRetries)
                                   .arg(delayMs)
                                   .arg(error));
        }

        /* Tear down the failed iteration's watchdog now so its stall /
         * wall-clock timers do not fire a spurious "stuck" during the
         * backoff wait; processStreamIteration() arms a fresh one. */
        teardownStreamMonitor(QString());

        /* Defer the retry on the event loop: never sleep the single Qt
         * thread (it would freeze the UI, heartbeat, abort, and every
         * sibling sub-agent). The run may be aborted during the wait. If
         * so, finalize it as aborted here, because no network callback
         * is in flight to do it: otherwise the run would hang in
         * isStreaming forever (a background sub-agent would never reach
         * a terminal state, leak, and never notify its parent). */
        const quint64 epoch = streamEpoch_;
        QTimer::singleShot(delayMs, this, [this, epoch]() {
            /* Drop the retry if the run it belonged to is gone: either
             * streaming stopped, or it was aborted and a new run started
             * (epoch bumped) - retrying then would double-drive the new
             * run's stream loop. */
            if (!isStreaming || streamEpoch_ != epoch) {
                return;
            }
            if (abortRequested) {
                if (hasPendingRequests()) {
                    abortRequested = false;
                    processStreamIteration();
                    return;
                }
                isStreaming = false;
                heartbeatTimer->stop();
                teardownStreamMonitor(QStringLiteral("user abort"));
                abortRequested = false;
                emit runAborted(streamFinalContent);
                return;
            }
            processStreamIteration();
        });
        return;
    }

    /* No more retries or non-retryable error */
    isStreaming = false;
    heartbeatTimer->stop();
    teardownStreamMonitor(QString());
    currentRetryCount         = 0;
    contextOverflowRetryCount = 0;
    emptyResponseRetryCount   = 0;
    emit runError(error);
}

void QSocAgent::computeRecallForTurn(const QString &query)
{
    recallBlock_.clear();

    /* Recall is a parent/interactive concern: disabled, sub-agent, or no
     * memory manager means nothing to inject. */
    if (!agentConfig.memoryRecallEnabled || agentConfig.isSubAgent || !memoryManager) {
        return;
    }
    if (!QSocMemoryRecall::queryIsRecallable(query)) {
        return;
    }

    const QList<QSocMemoryManager::MemoryHeader> headers = memoryManager->scanHeaders("all");
    if (headers.isEmpty()) {
        return;
    }

    QSocMemoryRecall::Config recallCfg;
    recallCfg.maxFiles   = agentConfig.memoryRecallMaxFiles;
    recallCfg.perFileCap = agentConfig.memoryRecallPerFileCap;
    recallCfg.turnBudget = agentConfig.memoryRecallTurnBudget;
    const QSocMemoryRecall recall(recallCfg);

    QSocMemoryManager *manager = memoryManager;
    const auto         reader  = [manager](const QString &scope, const QString &name) {
        return manager->readTopicFile(scope, name);
    };

    /* Small-memory fast path: when candidates are no more than we would
     * select anyway, skip the selector LLM round trip and inject them all
     * (still subject to per-file cap and turn budget). */
    if (headers.size() <= recallCfg.maxFiles) {
        recallBlock_ = recall.assembleBlock(headers, reader);
        return;
    }

    /* Selection path: ask a cheap model to rank headers by relevance.
     * Lazily clone the service onto the recall model; fall back to the
     * primary service when no model is set or the id is unknown. */
    QLLMService *selector = llmService;
    if (!agentConfig.memoryRecallModel.isEmpty()) {
        if (!recallLlm_) {
            recallLlm_ = llmService->clone(this);
            if (!recallLlm_->setCurrentModel(agentConfig.memoryRecallModel)) {
                recallLlm_->deleteLater();
                recallLlm_ = nullptr;
            }
        }
        if (recallLlm_) {
            selector = recallLlm_;
        }
    }

    json selMessages = json::array();
    selMessages.push_back(
        {{"role", "system"}, {"content", QSocMemoryRecall::selectorSystemPrompt().toStdString()}});
    selMessages.push_back(
        {{"role", "user"}, {"content", recall.buildSelectorPrompt(headers, query).toStdString()}});

    QStringList selectedNames;
    bool        selectorOk = false;
    try {
        /* Synchronous: safe here because no stream is in flight yet. */
        const json resp = selector->sendChatCompletion(selMessages, json::array(), 0.1);
        if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty()) {
            /* Got a response; guard every nested key (const operator[] on a
             * missing key is UB, not an exception). */
            selectorOk         = true;
            const json &choice = resp["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")
                && choice["message"]["content"].is_string()) {
                selectedNames = QSocMemoryRecall::parseSelection(
                    QString::fromStdString(choice["message"]["content"].get<std::string>()));
            }
        }
    } catch (const std::exception &) {
        selectorOk = false;
    }

    /* Selector unreachable or errored: fall back to the most recent topics
     * so memory still surfaces under a flaky cheap model. A successful but
     * empty selection is respected (the model judged nothing relevant). */
    if (!selectorOk) {
        if (agentConfig.verbose) {
            emit verboseOutput(
                QStringLiteral("[memory recall: selector unavailable, using recent topics]"));
        }
        recallBlock_
            = recall.assembleBlock(headers.mid(0, agentConfig.memoryRecallMaxFiles), reader);
        return;
    }

    if (selectedNames.isEmpty()) {
        return;
    }

    /* Map selected names back to headers, preserving mtime order. Dedup by
     * name so a topic present in both scopes is injected once (freshest). */
    QList<QSocMemoryManager::MemoryHeader> selected;
    QSet<QString>                          seenNames;
    for (const auto &header : headers) {
        if (selectedNames.contains(header.name) && !seenNames.contains(header.name)) {
            seenNames.insert(header.name);
            selected.append(header);
        }
    }

    recallBlock_ = recall.assembleBlock(selected, reader);
}

void QSocAgent::processStreamIteration()
{
    if (!isStreaming) {
        return;
    }

    /* Check for abort request. With input queued, ESC cuts only the
     * current iteration: the queue drain below feeds the model the new
     * message and the run continues (the whole run ends only when
     * nothing is queued). */
    if (abortRequested) {
        if (hasPendingRequests()) {
            abortRequested = false;
        } else {
            isStreaming = false;
            heartbeatTimer->stop();
            abortRequested = false;
            teardownStreamMonitor(QStringLiteral("user abort"));
            emit runAborted(streamFinalContent);
            return;
        }
    }

    /* Each iteration owns its watchdog: discard the previous one (so
     * its timer stops) and build a fresh one scoped to this round. */
    teardownStreamMonitor(QString());
    armStreamMonitor();

    /* Check for pending requests - inject them into conversation */
    {
        QMutexLocker locker(&queueMutex);
        while (!requestQueue.isEmpty()) {
            QueuedRequest item       = requestQueue.takeFirst();
            QString       newRequest = item.text;
            int           remaining  = requestQueue.size();
            locker.unlock();

            if (item.taskNotification) {
                emit processingQueuedRequest(QStringLiteral("[task notification]"), remaining);
                addMessage("user", newRequest);
                locker.relock();
                continue;
            }

            QString blockReason;
            if (!firePromptSubmitHook(&newRequest, &blockReason)) {
                emit processingQueuedRequest(
                    QStringLiteral("[blocked by user_prompt_submit: %1]").arg(blockReason),
                    remaining);
                locker.relock();
                continue;
            }

            emit processingQueuedRequest(newRequest, remaining);

            /* Add new user message - this will cause LLM to reconsider */
            addMessage("user", newRequest);

            locker.relock();
        }
    }

    streamIteration++;

    const int streamCap = agentConfig.maxTurnsOverride > 0 ? agentConfig.maxTurnsOverride
                                                           : agentConfig.maxIterations;
    if (streamIteration > streamCap) {
        isStreaming = false;
        heartbeatTimer->stop();
        teardownStreamMonitor(QString());
        const QString msg
            = agentConfig.maxTurnsOverride > 0
                  ? QString("Reached max turns limit (%1)").arg(streamCap)
                  : QString("[Agent safety limit reached (%1 iterations)]").arg(streamCap);
        emit runError(msg);
        return;
    }

    /* Check and compress history if needed */
    compressHistoryIfNeeded();

    if (agentConfig.verbose) {
        int     currentTokens = estimateTotalTokens();
        QString info = QString("[Iteration %1 | Tokens: %2/%3 (%4%) | Messages: %5]")
                           .arg(streamIteration)
                           .arg(currentTokens)
                           .arg(agentConfig.maxContextTokens)
                           .arg(100.0 * currentTokens / agentConfig.maxContextTokens, 0, 'f', 1)
                           .arg(messages.size());
        emit    verboseOutput(info);
    }

    /* Build messages with system prompt (includes auto-injected memory) */
    json messagesWithSystem = json::array();

    QString fullSystemPrompt = buildSystemPromptWithMemory();
    if (!fullSystemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", fullSystemPrompt.toStdString()}});
    }

    for (const auto &msg : messages) {
        json sanitized = msg;
        /* The internal `_usage` annotation is for our token estimator
         * only; OpenAI / DeepSeek reject unknown top-level fields on
         * messages, so strip it before the wire. */
        sanitized.erase("_usage");
        sanitized.erase("_img_tokens");
        messagesWithSystem.push_back(sanitized);
    }

    /* Critical reminder: re-injected as a system message at the tail
     * of every turn's wire payload so long-running children (e.g.
     * read-only `explore`) don't drift away from their hard rules.
     * Not persisted into `messages`. */
    if (!agentConfig.criticalReminder.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", agentConfig.criticalReminder.toStdString()}});
    }
    if (agentConfig.planMode) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", kPlanModeReminder}});
    }
    /* Focus-aware: when the user is not watching, steer away from
     * blocking ask_user prompts. Never persisted (tracks live focus). */
    if (userWatchingProbe_ && !userWatchingProbe_()) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", kNotWatchingReminder}});
    }
    /* Approved plan handoff: re-injected each turn (like the reminder,
     * never persisted) so the executing model keeps the plan across
     * pruning and compaction. Single, budget-capped copy. */
    if (!approvedPlan_.isEmpty()) {
        const QString planBlock
            = QStringLiteral(
                  "<approved_plan>\nThe user approved this implementation plan. "
                  "Follow it; deviate only with good reason and say so.\n\n%1\n"
                  "</approved_plan>")
                  .arg(approvedPlan_);
        messagesWithSystem.push_back({{"role", "system"}, {"content", planBlock.toStdString()}});
    }

    /* Selective memory recall: relevant memories for this turn, injected
     * as a non-persisted reminder so the system-prompt prefix (and its
     * cache) stays untouched. */
    if (!recallBlock_.isEmpty()) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", recallBlock_.toStdString()}});
    }

    /* Get tool definitions, filtered by sub-agent allowlist when set. */
    json tools = filterAllowedTools(toolRegistry->getToolDefinitions());

    /* Estimate input tokens for this request (includes system prompt + tool defs) */
    int inputTokens = estimateTotalTokens();
    totalInputTokens.fetch_add(inputTokens);

    /* Determine model override for reasoning */
    QString modelOverride;
    if (!agentConfig.effortLevel.isEmpty() && !agentConfig.reasoningModel.isEmpty()) {
        modelOverride = agentConfig.reasoningModel;
    }

    /* Send streaming request */
    llmService->sendChatCompletionStream(
        messagesWithSystem, tools, agentConfig.temperature, agentConfig.effortLevel, modelOverride);
}

void QSocAgent::handleStreamComplete(const json &response)
{
    if (!isStreaming) {
        return;
    }

    /* Check for errors */
    if (response.contains("error")) {
        QString errorMsg = QString::fromStdString(response["error"].get<std::string>());
        isStreaming      = false;
        heartbeatTimer->stop();
        teardownStreamMonitor(QString());
        emit runError(errorMsg);
        return;
    }

    /* Extract assistant message */
    if (!response.contains("choices") || response["choices"].empty()) {
        isStreaming = false;
        heartbeatTimer->stop();
        teardownStreamMonitor(QString());
        emit runError("Invalid response from LLM");
        return;
    }

    auto message = response["choices"][0]["message"];

    /* A valid streamed response means the connection is healthy: reset
     * the transient/rate-limit and context-overflow retry budgets so a
     * later independent stall on a long multi-turn run gets the full
     * allowance instead of a leaked count. */
    currentRetryCount         = 0;
    contextOverflowRetryCount = 0;

    /* Embed the server-reported usage on the assistant message itself
     * (private `_usage` field). The token estimator scans backwards
     * for the most recent record carrying this and treats it as
     * ground truth, then only estimates the tail added since. The
     * field is stripped before each outgoing request so OpenAI-style
     * APIs never see it. */
    if (response.contains("usage") && response["usage"].is_object()) {
        message["_usage"] = response["usage"];
    }

    /* Check for tool calls */
    if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
        /* Add assistant message with tool calls to history */
        messages.push_back(message);

        if (agentConfig.verbose) {
            emit verboseOutput("[Assistant requesting tool calls]");
        }

        /* Handle tool calls synchronously */
        handleToolCalls(message["tool_calls"]);

        /* Continue with next iteration */
        processStreamIteration();
        return;
    }

    /* Regular response without tool calls. Treat a missing content
     * field and a present-but-empty content string identically so the
     * empty-response handler only lives in one place. */
    QString content;
    if (message.contains("content") && message["content"].is_string()) {
        content = QString::fromStdString(message["content"].get<std::string>());
    }

    if (agentConfig.verbose) {
        emit verboseOutput(QString("[Assistant]: %1").arg(content));
    }

    /* Push full message to preserve reasoning_content for the next
     * iteration; some thinking-mode providers require it. */
    messages.push_back(message);

    accountGoalUsageForIteration(streamPrevTokensEstimate, streamIterationTimer);

    /* Continue if there are queued requests */
    if (hasPendingRequests()) {
        processStreamIteration();
        return;
    }

    if (maybeQueueGoalContinuation()) {
        /* A continuation (or budget-limit) prompt was just appended;
         * feed it back into the stream loop so the model answers it
         * without exiting to the REPL idle state. */
        processStreamIteration();
        return;
    }

    /* Plan-mode protocol: a prose ending leaves the user with no
     * approval dialog. Give the model one pointed chance to route the
     * text through exit_plan_mode or ask_user before the turn ends. */
    if (!content.trimmed().isEmpty() && maybeQueuePlanModeNudge()) {
        processStreamIteration();
        return;
    }

    if (!content.isEmpty()) {
        isStreaming = false;
        heartbeatTimer->stop();
        teardownStreamMonitor(QString());
        fireStopHook(content);
        emit runComplete(content);
        return;
    }

    /* Empty content path. Classify the cause: length-truncated runs
     * are deterministic and not worth retrying (same prompt would
     * exhaust the same budget); other empty responses get up to two
     * retries before surfacing a diagnostic. */
    QString finishReason;
    if (response["choices"][0].contains("finish_reason")
        && response["choices"][0]["finish_reason"].is_string()) {
        finishReason = QString::fromStdString(
            response["choices"][0]["finish_reason"].get<std::string>());
    }
    const bool hasReasoning = message.contains("reasoning_content")
                              && message["reasoning_content"].is_string()
                              && !message["reasoning_content"].get<std::string>().empty();

    QString diag;
    bool    retryable = false;
    if (finishReason == "length" && hasReasoning) {
        diag = QStringLiteral(
            "Output truncated by max_output_tokens; the model spent the whole "
            "budget on reasoning before emitting any reply. Raise "
            "max_output_tokens or lower effort.");
    } else if (finishReason == "length") {
        diag = QStringLiteral(
            "Output truncated by max_output_tokens before any content was "
            "emitted. Raise max_output_tokens.");
    } else if (hasReasoning) {
        diag      = QStringLiteral("Model returned reasoning content but no reply text.");
        retryable = true;
    } else {
        diag      = QStringLiteral("Empty response from the model.");
        retryable = true;
    }

    static constexpr int maxEmptyRetries = 2;
    if (retryable && emptyResponseRetryCount < maxEmptyRetries) {
        emptyResponseRetryCount++;
        /* Drop the empty assistant turn so the retry isn't conditioned
         * on the model having just produced nothing. */
        if (!messages.empty()) {
            messages.erase(messages.end() - 1);
        }
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Empty response %1/%2: retrying]")
                                   .arg(emptyResponseRetryCount)
                                   .arg(maxEmptyRetries));
        }
        processStreamIteration();
        return;
    }

    isStreaming = false;
    heartbeatTimer->stop();
    teardownStreamMonitor(QString());
    emptyResponseRetryCount = 0;
    fireStopHook(QString());
    emit runError(diag);
}

bool QSocAgent::processIteration()
{
    if (!llmService || !toolRegistry) {
        QSocConsole::warn() << "LLM service or tool registry not configured";
        return true;
    }

    /* Build messages with system prompt (includes auto-injected memory) */
    json messagesWithSystem = json::array();

    QString fullSystemPrompt = buildSystemPromptWithMemory();
    if (!fullSystemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", fullSystemPrompt.toStdString()}});
    }

    /* Add conversation history (strip the internal `_usage` /
     * `_img_tokens` annotations so the wire stays standard chat
     * completion shape). */
    for (const auto &msg : messages) {
        json sanitized = msg;
        sanitized.erase("_usage");
        sanitized.erase("_img_tokens");
        messagesWithSystem.push_back(sanitized);
    }

    /* Critical reminder: same per-turn re-injection as the streaming
     * path; defends against drift on long sync runs. */
    if (!agentConfig.criticalReminder.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", agentConfig.criticalReminder.toStdString()}});
    }
    if (agentConfig.planMode) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", kPlanModeReminder}});
    }
    /* Focus-aware: when the user is not watching, steer away from
     * blocking ask_user prompts. Never persisted (tracks live focus). */
    if (userWatchingProbe_ && !userWatchingProbe_()) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", kNotWatchingReminder}});
    }
    /* Approved plan handoff: re-injected each turn (like the reminder,
     * never persisted) so the executing model keeps the plan across
     * pruning and compaction. Single, budget-capped copy. */
    if (!approvedPlan_.isEmpty()) {
        const QString planBlock
            = QStringLiteral(
                  "<approved_plan>\nThe user approved this implementation plan. "
                  "Follow it; deviate only with good reason and say so.\n\n%1\n"
                  "</approved_plan>")
                  .arg(approvedPlan_);
        messagesWithSystem.push_back({{"role", "system"}, {"content", planBlock.toStdString()}});
    }

    /* Selective memory recall: relevant memories for this turn, injected
     * as a non-persisted reminder so the system-prompt prefix (and its
     * cache) stays untouched. */
    if (!recallBlock_.isEmpty()) {
        messagesWithSystem.push_back({{"role", "system"}, {"content", recallBlock_.toStdString()}});
    }

    /* Get tool definitions, filtered by sub-agent allowlist when set. */
    json tools = filterAllowedTools(toolRegistry->getToolDefinitions());

    /* Call LLM */
    json response
        = llmService->sendChatCompletion(messagesWithSystem, tools, agentConfig.temperature);

    /* Check for errors */
    if (response.contains("error")) {
        QString errorMsg = QString::fromStdString(response["error"].get<std::string>());
        QSocConsole::warn() << "LLM error:" << errorMsg;
        addMessage("assistant", QString("Error: %1").arg(errorMsg));
        return true;
    }

    /* Extract assistant message */
    if (!response.contains("choices") || response["choices"].empty()) {
        QSocConsole::warn() << "Invalid LLM response: no choices";
        addMessage("assistant", "Error: Invalid response from LLM");
        return true;
    }

    auto message = response["choices"][0]["message"];
    if (response.contains("usage") && response["usage"].is_object()) {
        message["_usage"] = response["usage"];
    }

    /* Check for tool calls */
    if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
        /* Add assistant message with tool calls to history */
        messages.push_back(message);

        if (agentConfig.verbose) {
            emit verboseOutput("[Assistant requesting tool calls]");
        }

        /* Handle tool calls */
        handleToolCalls(message["tool_calls"]);

        return false; /* Not complete yet, need to continue */
    }

    /* Regular response without tool calls */
    if (message.contains("content") && message["content"].is_string()) {
        QString content = QString::fromStdString(message["content"].get<std::string>());

        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Assistant]: %1").arg(content));
        }

        /* Add to history */
        addMessage("assistant", content);

        return true; /* Complete */
    }

    /* Empty response */
    addMessage("assistant", "");
    return true;
}

void QSocAgent::handleToolCalls(const json &toolCalls)
{
    /* Tool calls count as progress */
    if (streamMonitor != nullptr) {
        streamMonitor->notifyProgress();
    }

    for (const auto &toolCall : toolCalls) {
        QString toolCallId   = QString::fromStdString(toolCall["id"].get<std::string>());
        QString functionName = QString::fromStdString(
            toolCall["function"]["name"].get<std::string>());
        QString argumentsStr = QString::fromStdString(
            toolCall["function"]["arguments"].get<std::string>());

        /* Check for abort - must still add tool message for API format compliance */
        if (abortRequested) {
            addToolMessage(toolCallId, "Aborted by user");
            emit toolResult(functionName, "Aborted by user");
            continue;
        }

        if (agentConfig.verbose) {
            emit verboseOutput(QString("  -> Calling tool: %1").arg(functionName));
            emit verboseOutput(QString("     Arguments: %1").arg(argumentsStr));
        }

        emit toolCalled(functionName, argumentsStr);

        /* Allowlist guard. Defends against an LLM that ignores the
         * filtered tool list or recalls a name from earlier history. */
        const QString denyReason = toolDenyReason(functionName);
        if (!denyReason.isEmpty()) {
            const QString denied = QStringLiteral("Error: tool \"%1\" is not available: %2")
                                       .arg(functionName, denyReason);
            addToolMessage(toolCallId, denied);
            emit toolResult(functionName, denied);
            continue;
        }

        /* Parse arguments */
        json arguments;
        try {
            arguments = json::parse(argumentsStr.toStdString());
        } catch (const json::parse_error &e) {
            QString errorResult = QString("Error: Invalid JSON arguments - %1").arg(e.what());
            addToolMessage(toolCallId, errorResult);
            emit toolResult(functionName, errorResult);
            continue;
        }

        /* Plan-mode shell safety: bash / remote_shell_bash are judged
         * per command by the injected LLM classifier (semantic, not a
         * hardcoded allowlist). Fail-closed when no judge is installed.
         * Read-only commands proceed; mutating ones are surfaced to the
         * model as a denial so it can adjust or exit plan mode. */
        if (agentConfig.planMode
            && (functionName == QStringLiteral("bash")
                || functionName == QStringLiteral("remote_shell_bash"))) {
            QString command;
            if (arguments.contains("command") && arguments["command"].is_string()) {
                command = QString::fromStdString(arguments["command"].get<std::string>());
            }
            QSocBashSafety verdict;
            if (bashSafetyJudge_) {
                verdict = bashSafetyJudge_(command);
            }
            if (!verdict.readOnly) {
                const QString reason = verdict.reason.isEmpty()
                                           ? QStringLiteral("not classified as read-only")
                                           : verdict.reason;
                const QString denied
                    = QStringLiteral(
                          "Plan mode: command blocked, it may modify state (%1). Use "
                          "read-only inspection, or call exit_plan_mode to get approval.")
                          .arg(reason);
                addToolMessage(toolCallId, denied);
                emit toolResult(functionName, denied);
                continue;
            }
        }

        /* Pre-tool hook: matched hooks may inspect or block the call.
         * On block we short-circuit executeTool and surface the reason
         * back to the model as the tool result. */
        if (hookManager != nullptr && hookManager->hasHooksFor(QSocHookEvent::PreToolUse)) {
            json payload          = buildHookEnvelope();
            payload["event"]      = "pre_tool_use";
            payload["tool_name"]  = functionName.toStdString();
            payload["tool_input"] = arguments;
            const auto outcome = hookManager->fire(QSocHookEvent::PreToolUse, functionName, payload);
            if (outcome.blocked) {
                const QString reason = outcome.blockReason.isEmpty()
                                           ? QStringLiteral("hook blocked execution")
                                           : outcome.blockReason;
                const QString blocked
                    = QStringLiteral("Tool blocked by pre_tool_use hook: %1").arg(reason);
                addToolMessage(toolCallId, blocked);
                emit toolResult(functionName, blocked);
                continue;
            }
            if (outcome.hasMergedResponse && outcome.mergedResponse.contains("updatedInput")
                && outcome.mergedResponse["updatedInput"].is_object()) {
                arguments = outcome.mergedResponse["updatedInput"];
            }
        }

        /* Execute tool */
        const QString rawResult = toolRegistry->executeTool(functionName, arguments);

        /* Strip image-attachment markers up front so every consumer
         * (verbose log, scrollview, hooks, message history) sees the
         * same clean text. Attachments are passed separately to
         * addToolMessage which lifts them into the OpenAI-compatible
         * content array on the conversation history. */
        QList<AttachmentSpec> attachments;
        const QString         result = extractImageAttachments(rawResult, &attachments);

        if (agentConfig.verbose) {
            QString truncatedResult = result.length() > 200 ? result.left(200) + "... (truncated)"
                                                            : result;
            emit    verboseOutput(QString("     Result: %1").arg(truncatedResult));
        }

        emit toolResult(functionName, result);

        /* Post-tool hook: audit, log, or notify. Fire-and-forget; the
         * outcome is not allowed to mutate the result the model sees. */
        if (hookManager != nullptr && hookManager->hasHooksFor(QSocHookEvent::PostToolUse)) {
            json payload          = buildHookEnvelope();
            payload["event"]      = "post_tool_use";
            payload["tool_name"]  = functionName.toStdString();
            payload["tool_input"] = arguments;
            payload["response"]   = result.toStdString();
            hookManager->fire(QSocHookEvent::PostToolUse, functionName, payload);
        }

        /* Add tool response to messages */
        addToolMessage(toolCallId, result, attachments);
    }
}

void QSocAgent::setMemoryManager(QSocMemoryManager *manager)
{
    memoryManager = manager;
}

void QSocAgent::setHookManager(QSocHookManager *manager)
{
    hookManager = manager;
}

void QSocAgent::setLoopScheduler(QSocLoopScheduler *scheduler)
{
    loopScheduler = scheduler;
}

namespace {

nlohmann::json buildRemoteSection(const QSocAgentConfig &cfg)
{
    return nlohmann::json{
        {"display", cfg.remoteDisplay.toStdString()},
        {"workspace", cfg.remoteWorkspace.toStdString()},
        {"cwd", cfg.remoteWorkingDir.toStdString()},
    };
}

} // namespace

nlohmann::json QSocAgent::buildHookEnvelope() const
{
    nlohmann::json env = nlohmann::json::object();
    env["cwd"]         = QDir::currentPath().toStdString();
    if (agentConfig.remoteMode) {
        env["remote"] = buildRemoteSection(agentConfig);
    }
    return env;
}

void QSocAgent::fireSessionStartHookOnce()
{
    if (sessionStartFired) {
        return;
    }
    sessionStartFired = true;
    /* Sub-agents inherit the parent's session_start by default; suppress
     * the duplicate. Definitions that declare their own hooks
     * (agentConfig.hooks non-empty in a sub-agent) get to fire. */
    if (agentConfig.isSubAgent && agentConfig.hooks.isEmpty()) {
        return;
    }
    if (hookManager == nullptr || !hookManager->hasHooksFor(QSocHookEvent::SessionStart)) {
        return;
    }
    json payload     = buildHookEnvelope();
    payload["event"] = "session_start";
    hookManager
        ->fire(QSocHookEvent::SessionStart, hookEventToYamlKey(QSocHookEvent::SessionStart), payload);
}

void QSocAgent::fireStopHook(const QString &finalContent)
{
    /* Sub-agents finish into a tool result, not a session stop —
     * suppress unless the def explicitly declared its own hooks. */
    if (agentConfig.isSubAgent && agentConfig.hooks.isEmpty()) {
        return;
    }
    if (hookManager == nullptr || !hookManager->hasHooksFor(QSocHookEvent::Stop)) {
        return;
    }
    json payload             = buildHookEnvelope();
    payload["event"]         = "stop";
    payload["final_content"] = finalContent.toStdString();
    hookManager->fire(QSocHookEvent::Stop, hookEventToYamlKey(QSocHookEvent::Stop), payload);
}

bool QSocAgent::firePromptSubmitHook(QString *userQuery, QString *blockReason)
{
    /* Sub-agent prompts come from the parent agent, not the user —
     * unless the def opts into its own user_prompt_submit hook. */
    if (agentConfig.isSubAgent && agentConfig.hooks.isEmpty()) {
        return true;
    }
    if (hookManager == nullptr || !hookManager->hasHooksFor(QSocHookEvent::UserPromptSubmit)) {
        return true;
    }
    if (userQuery == nullptr) {
        return true;
    }
    json payload       = buildHookEnvelope();
    payload["event"]   = "user_prompt_submit";
    payload["prompt"]  = userQuery->toStdString();
    const auto outcome = hookManager->fire(
        QSocHookEvent::UserPromptSubmit,
        hookEventToYamlKey(QSocHookEvent::UserPromptSubmit),
        payload);
    if (outcome.blocked) {
        if (blockReason != nullptr) {
            *blockReason = outcome.blockReason.isEmpty()
                               ? QStringLiteral("user_prompt_submit hook blocked the prompt")
                               : outcome.blockReason;
        }
        return false;
    }
    if (outcome.hasMergedResponse && outcome.mergedResponse.contains("context")
        && outcome.mergedResponse["context"].is_string()) {
        const QString extra = QString::fromStdString(
            outcome.mergedResponse["context"].get<std::string>());
        if (!extra.isEmpty()) {
            *userQuery = extra + QStringLiteral("\n\n") + *userQuery;
        }
    }
    return true;
}

QString QSocAgent::buildSystemPromptWithMemory() const
{
    /* Legacy override path (non-sub-agent): replace the entire prompt. */
    if (!agentConfig.systemPromptOverride.isEmpty() && !agentConfig.isSubAgent) {
        return agentConfig.systemPromptOverride;
    }

    /* Sub-agent path: override replaces only the static identity /
     * usage sections; environment, project instructions, optional
     * skill listing and optional memory are still appended below so
     * the child sees the same workspace, project rules, and (when
     * enabled) the same skill / memory context as the parent. */
    if (agentConfig.isSubAgent && !agentConfig.systemPromptOverride.isEmpty()) {
        QString subPrompt = agentConfig.systemPromptOverride;
        if (!subPrompt.endsWith(QLatin1Char('\n'))) {
            subPrompt += QLatin1Char('\n');
        }
        appendDynamicSystemSections(subPrompt);
        return subPrompt;
    }

    QString prompt;

    /* ── Static sections ─────────────────────────────────────────────── */

    /* Section 1: Identity */
    prompt += QStringLiteral(
        "You are QSoC Agent, an interactive AI assistant for System-on-Chip design "
        "automation. You help users with RTL generation, bus integration, module "
        "management, project automation, and general software engineering tasks.\n");

    /* Section 2: Doing tasks */
    prompt += QStringLiteral(
        "\n# Doing tasks\n"
        "- The user will primarily request SoC design and software engineering tasks.\n"
        "- Read and understand existing code before suggesting modifications.\n"
        "- For multi-step tasks (3+ steps), create a TODO list first, then execute step by step.\n"
        "- Prefer editing existing files over creating new ones.\n"
        "- Do not add features, refactor, or make improvements beyond what was asked.\n"
        "- Complete every step — do not stop until the full task is done.\n"
        "- After each tool call, briefly explain the result.\n"
        "- If a tool fails, explain the error and try to fix it.\n");

    /* Section 3: SoC infrastructure (domain-specific) */
    prompt += QStringLiteral(
        "\n# SoC infrastructure\n"
        "qsoc generates production RTL from .soc_net YAML files via generate_verilog:\n"
        "- **clock**: ICG gating, static/dynamic/auto dividers, glitch-free MUX, STA guide "
        "buffers, test enable\n"
        "- **reset**: ARSR synchronizers (async assert/sync release), multi-source matrices, reset "
        "reason recording\n"
        "- **power**: 8-state FSM per domain (OFF→WAIT_DEP→TURN_ON→CLK_ON→ON→RST_ASSERT→TURN_OFF), "
        "hard/soft dependencies, fault recovery\n"
        "- **fsm**: Table-mode (Moore/Mealy) and microcode-mode, binary/onehot/gray encoding\n"
        "\n"
        "For clock/reset/power/FSM tasks, ALWAYS:\n"
        "1. Call query_docs with the matching topic to get YAML format details\n"
        "2. Write a .soc_net YAML file using the documented format\n"
        "3. Call generate_verilog with that file to produce production-grade Verilog\n"
        "NEVER write clock/reset/power/FSM Verilog manually — qsoc generates it with "
        "ICG, glitch-free MUX, ARSR synchronizers, 8-state power FSM, and proper DFT "
        "support that hand-written code will lack.\n"
        "\n"
        "Keyword triggers:\n"
        "- clock/clk/divider/ICG/PLL/clock gate → query_docs topic:\"clock\"\n"
        "- reset/rst/ARSR/synchronizer/reset tree → query_docs topic:\"reset\"\n"
        "- power/domain/pgood/power sequence → query_docs topic:\"power\"\n"
        "- FSM/state machine/sequencer/microcode → query_docs topic:\"fsm\"\n");

    /* Section 4: Actions & safety */
    prompt += QStringLiteral(
        "\n# Executing actions with care\n"
        "Consider the reversibility and blast radius of every action.\n"
        "- Local, reversible actions (editing files, running tests) — proceed freely.\n"
        "- Hard-to-reverse or shared-state actions (deleting files, force-push, "
        "dropping data) — explain the risk and confirm with the user first.\n"
        "- Never skip safety hooks (e.g. --no-verify) unless the user explicitly asks.\n"
        "- When encountering unexpected state (unfamiliar files, branches), investigate "
        "before deleting or overwriting — it may be the user's in-progress work.\n"
        "\n"
        "Directory access:\n"
        "- Read: unrestricted (any path)\n"
        "- Write: allowed directories only (project, working, user-added, temp)\n"
        "- Use path_context to manage allowed directories\n"
        "- Always use absolute paths\n");

    /* Section 5: Using tools */
    prompt += QStringLiteral(
        "\n# Using your tools\n"
        "- Use dedicated tools (file_read, file_write, file_edit, grep, glob) instead "
        "of shell equivalents when available — they are more reliable and reviewable.\n"
        "- Break down complex work with todo_add / todo_update to track progress.\n"
        "- For SoC infrastructure: query_docs → file_write .soc_net → generate_verilog.\n"
        "- For web research: web_search for discovery, web_fetch for content retrieval.\n"
        "- Use monitor proactively when the user asks to watch, monitor, tail, poll, "
        "wait for a status change, react to log lines, or keep an eye on a long-running "
        "process while continuing the conversation. Write the watcher command yourself; "
        "do not ask the user to configure it.\n"
        "- Use background bash for long-running commands whose output only needs later "
        "manual inspection. Use monitor when output should wake you as events arrive.\n"
        "- Tool definitions are provided separately — you do not need to memorize their "
        "schemas.\n");

    /* Section 6: Tone and style */
    prompt += QStringLiteral(
        "\n# Tone and style\n"
        "- Do not use emojis unless the user explicitly asks for them.\n"
        "- Be concise. Lead with the answer or action, not the reasoning.\n"
        "- Use Github-flavored markdown for formatting.\n"
        "- When referencing code, include `file_path:line_number` for navigation.\n"
        "- Do not restate what the user said — just do it.\n"
        "- If you can say it in one sentence, do not use three.\n");

    /* Section 7: Output efficiency */
    prompt += QStringLiteral(
        "\n# Output efficiency\n"
        "Go straight to the point. Try the simplest approach first.\n"
        "Focus text output on:\n"
        "- Decisions that need the user's input\n"
        "- High-level status updates at natural milestones\n"
        "- Errors or blockers that change the plan\n"
        "Skip filler words, preamble, and unnecessary transitions.\n");

    /* Section 7.5: Background monitoring and scheduled prompts */
    prompt += QStringLiteral(
        "\n# Background monitoring and scheduled prompts\n"
        "Use monitor for live event streams inside this session: log tails, file watches, "
        "CI/status polling loops, test runners in watch mode, dev servers, and any script "
        "that should notify you line-by-line as something changes. Keep monitor output "
        "concise: print one useful event per line and avoid noisy heartbeats.\n"
        "Use schedule_create for time-based future prompts: reminders, periodic check-ins, "
        "or work that should run at a wall-clock cadence even when there is no live output.\n"
        "Do NOT use schedule_create for ordinary one-off immediate work.\n"
        "Time format is a 5-field cron string in local time.\n"
        "- Recurring: cron like \"*/5 * * * *\" with recurring=true.\n"
        "- One-shot: pin minute+hour+dom+month, e.g. \"30 14 27 2 *\" with recurring=false.\n"
        "Default durable=false (session-only). Set durable=true only when the user says "
        "\"permanently\", \"every day from now on\", or similar.\n"
        "Stop monitors with monitor_stop when the watched condition is resolved or the "
        "watch is no longer useful.\n"
        "Preserve /commands and !commands verbatim in prompt.\n"
        "After scheduling, tell the user the id, schedule, durability, and how to cancel "
        "(schedule_delete id=<id>).\n");

    appendDynamicSystemSections(prompt);
    return prompt;
}

void QSocAgent::appendDynamicSystemSections(QString &prompt) const
{
    /* Section 8: Environment */
    {
        QString envSection = QStringLiteral("\n# Environment\n");
        if (!agentConfig.modelId.isEmpty()) {
            envSection += QStringLiteral("- Model: ") + agentConfig.modelId + QStringLiteral("\n");
        }
        envSection += QStringLiteral("- Platform: ") + QSysInfo::productType() + QStringLiteral(" ")
                      + QSysInfo::productVersion() + QStringLiteral("\n");
        envSection += QStringLiteral("- Architecture: ") + QSysInfo::currentCpuArchitecture()
                      + QStringLiteral("\n");
        if (!agentConfig.projectPath.isEmpty()) {
            envSection += QStringLiteral("- Working directory: ") + agentConfig.projectPath
                          + QStringLiteral("\n");
            QDir gitDir(agentConfig.projectPath);
            if (gitDir.exists(QStringLiteral(".git"))) {
                envSection += QStringLiteral("- Git repository: yes\n");
            }
        }
        prompt += envSection;
    }

    /* Remote workspace environment, emitted only when an SSH remote is active. */
    if (agentConfig.remoteMode) {
        QString remoteSection = QStringLiteral("\n# Remote Workspace\n");
        remoteSection += QStringLiteral("- Mode: SSH remote workspace\n");
        if (!agentConfig.remoteDisplay.isEmpty()) {
            remoteSection += QStringLiteral("- Target: ") + agentConfig.remoteDisplay
                             + QStringLiteral("\n");
        }
        if (!agentConfig.remoteWorkspace.isEmpty()) {
            remoteSection += QStringLiteral("- Remote workspace: ") + agentConfig.remoteWorkspace
                             + QStringLiteral("\n");
        }
        if (!agentConfig.remoteWorkingDir.isEmpty()) {
            remoteSection += QStringLiteral("- Working directory: ") + agentConfig.remoteWorkingDir
                             + QStringLiteral("\n");
        }
        if (!agentConfig.remoteWritableDirs.isEmpty()) {
            remoteSection += QStringLiteral("- Writable directories:\n");
            for (const QString &dir : agentConfig.remoteWritableDirs) {
                remoteSection += QStringLiteral("  - ") + dir + QStringLiteral("\n");
            }
        }
        remoteSection += QStringLiteral(
            "\n"
            "All workspace tools operate on the remote host:\n"
            "- read_file, list_files, write_file, edit_file use remote SFTP.\n"
            "- bash and bash_manage execute on the remote host.\n"
            "- path_context reports and changes remote paths.\n"
            "- todo tools read/write the remote workspace .qsoc/todos.md.\n"
            "- project memory, project skills, and project instructions are loaded from the\n"
            "  remote project when available.\n"
            "\n"
            "The following QSoC business tools are intentionally unavailable in remote mode:\n"
            "project_*, module_*, bus_*, generate_*, lsp.\n"
            "If a task requires these tools, explain that remote mode currently supports file,\n"
            "shell, docs, web, project memory, project skills, skill creation in remote\n"
            "project scope, and todo operations only.\n"
            "\n"
            "Use absolute remote paths in tool calls. Do not refer to local paths unless the\n"
            "user explicitly asks for local-machine information.\n"
            "\n"
            "Local QSoC configuration remains authoritative for LLM endpoints, API keys,\n"
            "proxy, remote profiles, SSH policy, tool policy, model selection, and safety\n"
            "rules. Remote .qsoc.yml is project metadata only and must not override local\n"
            "control configuration.\n"
            "\n"
            "# SSH Secret Handling\n"
            "Never request, display, summarize, copy, or store SSH private key contents.\n"
            "QSoC authenticates through ssh-agent or by passing an IdentityFile path to\n"
            "libssh2; the private key file content is never exposed to tools, prompts, logs,\n"
            "or memory.\n");
        prompt += remoteSection;
    }

    /* Model capability: emitted only when the active model has no
     * vision support so the LLM stops suggesting screenshots, image
     * uploads, or tool calls that produce image content. Text-only
     * models silently drop image content blocks otherwise, which
     * surfaces as the agent appearing to "ignore" the user's image. */
    if (llmService != nullptr && !llmService->currentSupportsImage()) {
        prompt += QStringLiteral(
            "\n# Model Capability\n"
            "- The active model is text-only; it cannot see images.\n"
            "- Do NOT ask the user to attach screenshots, paste images, or\n"
            "  upload pictures; describe what you need in words instead.\n"
            "- Do NOT call web_image, image-extracting variants of web_fetch,\n"
            "  or any tool whose primary output is an image. Prefer the\n"
            "  text-rendering tool of the same family when available.\n"
            "- When a tool returns an image surrogate like\n"
            "  `[image: file mime=... dims=... bytes=... est_tokens=...]`,\n"
            "  treat the surrogate text as the only information you have\n"
            "  about that image; do not pretend to have seen its contents.\n");
    }

    /* Host catalog: emitted when the catalog has entries OR the
     * active binding is non-local, so the LLM knows what hosts it
     * can dispatch sub-agents to and where it is currently bound.
     * Capability text is fed verbatim from the user-curated YAML. */
    if (hostCatalog != nullptr) {
        const auto entries = hostCatalog->allList();
        const auto active  = hostCatalog->active();
        if (!entries.isEmpty() || !active.isLocal()) {
            QString section = QStringLiteral("\n# Host Catalog\n\n");
            section += QStringLiteral("## Current host binding\n\n");
            if (active.isAlias()) {
                section += QStringLiteral("Active: ") + active.alias + QStringLiteral("\n");
                for (const auto &entry : entries) {
                    if (entry.alias == active.alias) {
                        section += QStringLiteral("Workspace: ") + entry.workspace
                                   + QStringLiteral("\n");
                        break;
                    }
                }
            } else if (active.isAdHoc()) {
                section += QStringLiteral("Active: ") + active.adHocTarget
                           + QStringLiteral(" (ad-hoc)\n");
                section += QStringLiteral("Workspace: ") + active.adHocWorkspace
                           + QStringLiteral("\n");
            } else {
                section += QStringLiteral("Active: local\nWorkspace: (none)\n");
            }
            if (!entries.isEmpty()) {
                section += QStringLiteral("\n## Available execution host\n\n");
                section += QStringLiteral("- local: this machine\n");
                for (const auto &entry : entries) {
                    const QString cap = entry.capability.isEmpty()
                                            ? QStringLiteral("(no capability text)")
                                            : entry.capability;
                    section += QStringLiteral("- ") + entry.alias + QStringLiteral(": ") + cap
                               + QStringLiteral("\n");
                }
                section += QStringLiteral(
                    "\nWhen spawning an `agent` tool call, set `host` to a name above when "
                    "the capability matches the task. Omit `host` to use the active binding.\n");
            }
            section += QStringLiteral(
                "\n## Host catalog learning\n\n"
                "The catalog is live. Update via host_register / host_update / host_remove when "
                "you OBSERVE a material change: the user states or retracts a capability, a run "
                "reveals a tool not yet listed, or a failure contradicts a listed capability. "
                "Do not log every successful command. When in doubt, ask the user before "
                "recording. To save the currently-bound ad-hoc target, call host_register with "
                "the active target+workspace shown above.\n");
            prompt += section;
        }
    }

    /* Section 9: Project instructions (AGENTS.md / AGENTS.local.md). */
    if (agentConfig.injectProjectMd && !agentConfig.projectPath.isEmpty()) {
        QDir    projectDir(agentConfig.projectPath);
        QString instructions;
        for (const QString &name :
             {QStringLiteral("AGENTS.md"), QStringLiteral("AGENTS.local.md")}) {
            const QString path = projectDir.filePath(name);
            QFile         file(path);
            if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream   stream(&file);
                const QString content = stream.readAll().trimmed();
                file.close();
                if (!content.isEmpty()) {
                    if (!instructions.isEmpty()) {
                        instructions += QStringLiteral("\n\n");
                    }
                    instructions += content;
                }
            }
        }
        if (!instructions.isEmpty()) {
            prompt += QStringLiteral(
                          "\n# Project instructions\n"
                          "The following rules were loaded from AGENTS.md / AGENTS.local.md "
                          "in the project root. Follow them precisely.\n\n")
                      + instructions + QStringLiteral("\n");
        }
    }

    /* Section 10: Available skills */
    if (!agentConfig.skillListing.isEmpty()) {
        prompt += QStringLiteral("\n# Available skills\n\n") + agentConfig.skillListing;
    }

    /* Section 11.5: External MCP servers contributing tools */
    if (toolRegistry != nullptr) {
        QHash<QString, int> mcpToolCounts;
        const QStringList   allNames = toolRegistry->toolNames();
        for (const QString &name : allNames) {
            if (!name.startsWith(QStringLiteral("mcp__"))) {
                continue;
            }
            const QString rest = name.mid(5);
            const int     sep  = rest.indexOf(QStringLiteral("__"));
            if (sep <= 0) {
                continue;
            }
            mcpToolCounts[rest.left(sep)] += 1;
        }
        if (!mcpToolCounts.isEmpty()) {
            prompt += QStringLiteral(
                "\n# External MCP servers\n"
                "These remote Model Context Protocol servers contribute tools, "
                "namespaced as mcp__<server>__<tool>:\n");
            QStringList serverNames = mcpToolCounts.keys();
            std::sort(serverNames.begin(), serverNames.end());
            for (const QString &srv : serverNames) {
                const int     count = mcpToolCounts.value(srv);
                const QString unit  = count == 1 ? QStringLiteral("tool") : QStringLiteral("tools");
                prompt += QStringLiteral("- %1 (%2 %3)\n").arg(srv).arg(count).arg(unit);
            }
        }
    }

    /* Section 11: Memory. When selective recall is enabled, relevant
     * memories are injected per turn as a non-persisted reminder instead,
     * so the full index is kept out of the (cached) system prompt. Full
     * index injection remains the fallback when recall is disabled. */
    if (agentConfig.autoLoadMemory && !agentConfig.memoryRecallEnabled && memoryManager) {
        QString memoryContent = memoryManager->loadMemoryForPrompt(agentConfig.memoryMaxChars);
        if (!memoryContent.isEmpty()) {
            prompt += QStringLiteral(
                          "\n# Memory\n"
                          "Below is persistent memory from previous sessions. "
                          "Use this context but verify stale information against current code.\n\n")
                      + memoryContent;
        }
    }
}

void QSocAgent::addMessage(const QString &role, const QString &content)
{
    messages.push_back({{"role", role.toStdString()}, {"content", content.toStdString()}});
}

QString QSocAgent::extractImageAttachments(const QString &raw, QList<AttachmentSpec> *out)
{
    if (out != nullptr) {
        out->clear();
    }
    const QString openMarker  = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen());
    const QString closeMarker = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerClose());
    if (openMarker.isEmpty() || closeMarker.isEmpty() || !raw.contains(openMarker)) {
        return raw;
    }

    QString stripped;
    stripped.reserve(raw.size());
    int pos = 0;
    while (pos < raw.size()) {
        const int oi = raw.indexOf(openMarker, pos);
        if (oi < 0) {
            stripped.append(QStringView{raw}.mid(pos));
            break;
        }
        const int ci = raw.indexOf(closeMarker, oi + openMarker.size());
        if (ci < 0) {
            /* Unterminated marker: treat the rest as text so we never lose
             * data, and stop scanning. */
            stripped.append(QStringView{raw}.mid(pos));
            break;
        }
        stripped.append(QStringView{raw}.mid(pos, oi - pos));

        const QString jsonText
            = raw.mid(oi + openMarker.size(), ci - oi - static_cast<int>(openMarker.size()));
        if (out != nullptr) {
            try {
                const auto     payload = json::parse(jsonText.toStdString());
                AttachmentSpec spec;
                spec.mime      = QString::fromStdString(payload.value("mime", std::string()));
                spec.dataB64   = QString::fromStdString(payload.value("data", std::string()));
                spec.sourceUrl = QString::fromStdString(payload.value("source_url", std::string()));
                spec.width     = payload.value("width", 0);
                spec.height    = payload.value("height", 0);
                spec.byteSize  = payload.value("byte_size", 0);
                spec.estTokens = payload.value("est_tokens", 0);
                spec.resized   = payload.value("resized", false);
                if (!spec.mime.isEmpty() && !spec.dataB64.isEmpty()) {
                    out->append(spec);
                }
            } catch (const json::exception &) {
                /* Drop malformed marker silently: the textual summary
                 * line above the marker still tells the model what was
                 * supposed to be there. */
            }
        }
        pos = ci + closeMarker.size();
        /* Eat the trailing newline left over from the tool's separator
         * so the visible text reads naturally. */
        if (pos < raw.size() && raw[pos] == QLatin1Char('\n')) {
            pos++;
        }
    }
    return stripped;
}

void QSocAgent::addToolMessage(
    const QString &toolCallId, const QString &content, const QList<AttachmentSpec> &attachments)
{
    /* Tool messages always carry a plain string. OpenAI defines a
     * content-array form for tool responses, but several backends
     * reject it ("text is not set"); a string keeps every provider
     * on the same path. Binary attachments are delivered on a
     * follow-up user message instead. */
    json toolMsg
        = {{"role", "tool"},
           {"tool_call_id", toolCallId.toStdString()},
           {"content", content.toStdString()}};
    messages.push_back(toolMsg);

    if (attachments.isEmpty()) {
        return;
    }

    /* Synthetic user message: one text part (required by strict
     * providers) plus one image_url part per attachment, encoded
     * as a data URL so the server never has to fetch the original
     * host. */
    json contentArr  = json::array();
    int  imageTokens = 0;
    contentArr.push_back({{"type", "text"}, {"text", std::string("Tool attachment payload:")}});
    for (const auto &att : attachments) {
        const QString dataUrl = QStringLiteral("data:%1;base64,%2").arg(att.mime, att.dataB64);
        contentArr.push_back(
            {{"type", "image_url"}, {"image_url", {{"url", dataUrl.toStdString()}}}});
        imageTokens += att.estTokens;
    }
    /* Record the image token cost as an internal annotation (stripped on
     * the wire like `_usage`) so the token estimator counts array-content
     * image messages, which it otherwise skips. */
    json imageMsg = {{"role", "user"}, {"content", contentArr}};
    if (imageTokens > 0) {
        imageMsg["_img_tokens"] = imageTokens;
    }
    messages.push_back(imageMsg);
}

void QSocAgent::clearHistory()
{
    messages = json::array();
}

void QSocAgent::accountGoalUsageForIteration(int &prevTokensEstimate, QElapsedTimer &iterationTimer)
{
    if (goalCatalog == nullptr) {
        return;
    }
    const int newEstimate = estimateMessagesTokens();
    const int deltaTokens = newEstimate > prevTokensEstimate ? newEstimate - prevTokensEstimate : 0;
    const qint64 deltaSec = iterationTimer.elapsed() / 1000;
    prevTokensEstimate    = newEstimate;
    iterationTimer.restart();
    if (deltaTokens > 0 || deltaSec > 0) {
        QString innerErr;
        goalCatalog->accountUsage(deltaTokens, deltaSec, &innerErr);
    }
}

bool QSocAgent::maybeQueueGoalContinuation()
{
    if (goalCatalog == nullptr) {
        return false;
    }
    if (hasPendingRequests()) {
        return false;
    }
    if (goalContinuationInFlight.exchange(true)) {
        return false;
    }

    bool queued      = false;
    auto currentGoal = goalCatalog->current();
    if (currentGoal.has_value() && currentGoal->status == QSocGoalStatus::Active) {
        if (currentGoal->tokenBudget > 0 && currentGoal->tokensUsed >= currentGoal->tokenBudget) {
            /* Flip to BudgetLimited and inject the wrap-up prompt
             * exactly once; the next turn must not auto-continue
             * because Active is no longer the status. */
            QString innerErr;
            goalCatalog->setStatus(QSocGoalStatus::BudgetLimited, &innerErr);
            const auto refreshed = goalCatalog->current();
            if (refreshed.has_value()) {
                addMessage("user", QSocGoalPrompt::budgetLimit(*refreshed));
                goalCatalog->noteContinuation(QStringLiteral("budget_limited"));
                queued = true;
            }
        } else {
            addMessage("user", QSocGoalPrompt::continuation(*currentGoal));
            goalCatalog->noteContinuation(QStringLiteral("auto"));
            queued = true;
        }
    }
    goalContinuationInFlight.store(false);
    return queued;
}

bool QSocAgent::maybeQueuePlanModeNudge()
{
    if (!agentConfig.planMode || agentConfig.isSubAgent || planNudgeUsed) {
        return false;
    }
    if (hasPendingRequests()) {
        return false;
    }
    planNudgeUsed = true;
    addMessage(
        "user",
        QStringLiteral(
            "You are in plan mode and ended your turn without calling a tool. "
            "Do not reply in prose: if the plan is ready, call exit_plan_mode "
            "with the complete plan; if you need anything from the user, call "
            "ask_user with your question."));
    if (agentConfig.verbose) {
        emit verboseOutput("[Plan mode: nudging model to call exit_plan_mode or ask_user]");
    }
    return true;
}

void QSocAgent::queueRequest(const QString &request)
{
    QMutexLocker locker(&queueMutex);
    requestQueue.append({request, false});
}

void QSocAgent::queueTaskNotification(const QString &notification)
{
    QMutexLocker locker(&queueMutex);
    requestQueue.append({notification, true});
}

bool QSocAgent::hasPendingRequests() const
{
    QMutexLocker locker(&queueMutex);
    return !requestQueue.isEmpty();
}

int QSocAgent::pendingRequestCount() const
{
    QMutexLocker locker(&queueMutex);
    return requestQueue.size();
}

void QSocAgent::clearPendingRequests()
{
    QMutexLocker locker(&queueMutex);
    requestQueue.clear();
}

void QSocAgent::addExternalTokenUsage(qint64 inputTokens, qint64 outputTokens)
{
    if (inputTokens <= 0 && outputTokens <= 0) {
        return;
    }
    if (inputTokens > 0) {
        totalInputTokens.fetch_add(inputTokens);
    }
    if (outputTokens > 0) {
        totalOutputTokens.fetch_add(outputTokens);
    }
    emit tokenUsage(totalInputTokens.load(), totalOutputTokens.load());
}

void QSocAgent::abort()
{
    abortRequested = true;

    /* Latch the watchdog first so its tick handler sees the cancel
     * before the network reply tears down. */
    if (streamMonitor != nullptr) {
        streamMonitor->cancel(QStringLiteral("user abort"));
    }

    /* Cascade to LLM stream */
    if (llmService) {
        llmService->abortStream();
    }

    /* Cascade to running tools */
    if (toolRegistry) {
        toolRegistry->abortAll();
    }
}

void QSocAgent::armStreamMonitor()
{
    const QLongTaskMonitor::Config cfg{
        1000 /*tickIntervalMs*/,
        agentConfig.stuckThresholdSeconds > 0 ? agentConfig.stuckThresholdSeconds * 1000 : 30000,
        300000 /*wallClockMs (5 min)*/,
        3 /*consecutiveIdleTicks*/,
    };
    streamMonitor = new QLongTaskMonitor(this, cfg);
    connect(streamMonitor, &QLongTaskMonitor::stalled, this, [this](int silentMs, int /*streak*/) {
        const int silentSec = silentMs / 1000;
        emit      stuckDetected(streamIteration, silentSec);
        if (agentConfig.autoStatusCheck) {
            queueRequest(
                "[System: No progress detected. Please briefly report: "
                "1) What are you doing? 2) Any issues? 3) Estimated time remaining?]");
        }
    });
    connect(streamMonitor, &QLongTaskMonitor::wallClockExceeded, this, [this](int elapsedMs) {
        emit stuckDetected(streamIteration, elapsedMs / 1000);
    });
    streamMonitor->start();
}

void QSocAgent::teardownStreamMonitor(const QString &cancelReason)
{
    if (streamMonitor == nullptr) {
        return;
    }
    if (cancelReason.isEmpty()) {
        streamMonitor->finish();
    } else {
        streamMonitor->cancel(cancelReason);
    }
    streamMonitor->deleteLater();
    streamMonitor = nullptr;
}

bool QSocAgent::isRunning() const
{
    return isStreaming;
}

void QSocAgent::setLLMService(QLLMService *llmService)
{
    this->llmService = llmService;
}

void QSocAgent::setToolRegistry(QSocToolRegistry *toolRegistry)
{
    this->toolRegistry = toolRegistry;
}

void QSocAgent::setApprovedPlan(const QString &plan)
{
    /* Single slot: the plan is not stored in `messages` (which would
     * break tool_call/tool ordering if set mid-dispatch and would need
     * prune/compact protection). It is re-injected as a system message
     * each turn from this string, so a new plan simply overwrites the
     * old and it survives compaction for free. The caller budget-caps
     * the text and keeps the full copy on disk. */
    approvedPlan_ = plan;
}

void QSocAgent::setEffortLevel(const QString &level)
{
    agentConfig.effortLevel = level;
}

void QSocAgent::setReasoningModel(const QString &model)
{
    agentConfig.reasoningModel = model;
}

void QSocAgent::setConfig(const QSocAgentConfig &config)
{
    agentConfig = config;
}

QSocAgentConfig QSocAgent::getConfig() const
{
    return agentConfig;
}

json QSocAgent::getMessages() const
{
    return messages;
}

void QSocAgent::setMessages(const json &msgs)
{
    if (msgs.is_array()) {
        messages = msgs;
    }
}

int QSocAgent::estimateTokens(const QString &text) const
{
    /* Character-class weighted heuristic. Flat chars/4 severely
     * under-counts CJK-heavy conversations (one CJK char ≈ 1 token in
     * cl100k/o200k BPEs, not 0.25), which let auto-compact overshoot
     * the model window and then crash on its own over-budget LLM call.
     * Weights below track empirical BPE output within ~20% for
     * English/code, Chinese, Japanese/Korean, and mixed content, which
     * is tight enough for threshold-based compaction decisions. */
    double tokens = 0.0;
    for (const QChar qch : text) {
        const ushort code = qch.unicode();
        if (code < 0x80) {
            tokens += 0.25; /* ASCII text/code ≈ 4 chars per token */
        } else if (code >= 0x4E00 && code <= 0x9FFF) {
            tokens += 1.0; /* CJK Unified Ideographs */
        } else if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0xAC00 && code <= 0xD7AF)) {
            tokens += 0.8; /* Kana and Hangul syllables */
        } else {
            tokens += 0.5; /* Latin-ext, punctuation, symbols, emoji lead */
        }
    }
    return static_cast<int>(tokens) + 1;
}

int QSocAgent::effectiveContextTokens() const
{
    /* Model windows cover input + output combined; the assistant reply
     * carves a slice off the same pool. Threshold math must run against
     * the input budget alone, otherwise the request can clear the
     * compact threshold and still get rejected when the reply lands. */
    const int reserved
        = qBound(0, agentConfig.reservedOutputTokens, agentConfig.maxContextTokens / 2);
    return qMax(1024, agentConfig.maxContextTokens - reserved);
}

int QSocAgent::estimateMessagesTokens() const
{
    /* Anchor on the most recent assistant message that carries server
     * usage. Everything up to and including that message is already
     * accounted for by `prompt_tokens`; only the tail added since needs
     * a heuristic estimate. Fall back to full-walk estimation when no
     * usage anchor exists yet (first turn, or non-streaming endpoints
     * that strip the usage field). */
    const int msgCount           = static_cast<int>(messages.size());
    int       anchorIdx          = -1;
    int       anchorPromptTokens = 0;
    for (int i = msgCount - 1; i >= 0; --i) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (!msg.contains("_usage") || !msg["_usage"].is_object()) {
            continue;
        }
        const auto &usage = msg["_usage"];
        /* Use prompt_tokens (input only) since the agent is asking
         * "what would the next request weigh"; output_tokens belong
         * to the reply slice, not the input window. The assistant's
         * own reply contributes to the next request's input — count
         * it via the per-message tail walk below. */
        int promptTok = 0;
        if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number_integer()) {
            promptTok = usage["prompt_tokens"].get<int>();
        } else if (usage.contains("input_tokens") && usage["input_tokens"].is_number_integer()) {
            promptTok = usage["input_tokens"].get<int>();
        }
        if (promptTok > 0) {
            anchorIdx          = i;
            anchorPromptTokens = promptTok;
            break;
        }
    }

    int total = 0;
    int start = 0;
    if (anchorIdx >= 0) {
        total = anchorPromptTokens;
        /* Anchor's prompt covers messages [0, anchorIdx); the assistant
         * message at anchorIdx is the reply to that prompt, so it is
         * NOT in prompt_tokens but WILL be in the next request's
         * prompt. Estimate from anchorIdx forward. */
        start = anchorIdx;
    }

    for (int i = start; i < msgCount; ++i) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (msg.contains("content") && msg["content"].is_string()) {
            total += estimateTokens(QString::fromStdString(msg["content"].get<std::string>()));
        }
        if (msg.contains("tool_calls")) {
            total += estimateTokens(QString::fromStdString(msg["tool_calls"].dump()));
        }
        /* Image messages carry array content the string branch skips; add
         * the precomputed per-image cost recorded in `_img_tokens`. */
        if (msg.contains("_img_tokens") && msg["_img_tokens"].is_number_integer()) {
            total += msg["_img_tokens"].get<int>();
        }
        total += 10; /* per-message structural overhead */
    }
    return total;
}

int QSocAgent::estimateTotalTokens() const
{
    int tokens = estimateMessagesTokens();

    /* When estimateMessagesTokens fell back to anchorless walking (no
     * server usage seen yet), system prompt + tool defs aren't covered;
     * add them. When anchored, prompt_tokens already includes them and
     * we'd be double-counting. Detect anchor by scanning for `_usage`. */
    bool anchored = false;
    for (const auto &msg : messages) {
        if (msg.contains("_usage") && msg["_usage"].is_object()) {
            anchored = true;
            break;
        }
    }
    if (!anchored) {
        tokens += estimateTokens(buildSystemPromptWithMemory());
        if (toolRegistry) {
            json tools = filterAllowedTools(toolRegistry->getToolDefinitions());
            if (!tools.empty()) {
                tokens += estimateTokens(QString::fromStdString(tools.dump()));
            }
        }
        /* Reminders are appended to messagesWithSystem every turn but never
         * stored in `messages`, so the walk misses them. Count them only in
         * the anchorless case; once anchored, prompt_tokens already includes
         * the reminders sent that turn, so adding them again double-counts. */
        tokens += estimateTokens(recallBlock_);
        tokens += estimateTokens(approvedPlan_);
        tokens += estimateTokens(agentConfig.criticalReminder);
    }

    return tokens;
}

int QSocAgent::compact()
{
    int originalTokens = estimateMessagesTokens();

    /* Layer 1: Force prune (skip threshold check) */
    if (pruneToolOutputs(true)) {
        int  afterPrune = estimateMessagesTokens();
        emit compacting(1, originalTokens, afterPrune);
    }

    /* Layer 2: Force LLM compact (skip threshold check) */
    int beforeCompact = estimateMessagesTokens();
    if (compactWithLLM(true)) {
        int  afterCompact = estimateMessagesTokens();
        emit compacting(2, beforeCompact, afterCompact);
    }

    int afterTokens = estimateMessagesTokens();
    return originalTokens - afterTokens;
}

bool QSocAgent::pruneToolOutputs(bool force)
{
    if (!force) {
        int currentTokens = estimateTotalTokens();
        int pruneTokens   = static_cast<int>(effectiveContextTokens() * agentConfig.pruneThreshold);
        if (currentTokens <= pruneTokens) {
            return false;
        }
    }

    int msgCount = static_cast<int>(messages.size());
    if (msgCount == 0) {
        return false;
    }

    /* Scan from end to find protection boundary.
     * Everything at or after protectBoundary is protected (recent).
     * Everything before protectBoundary is eligible for pruning. */
    int toolTokensFromEnd = 0;
    int protectBoundary   = 0; /* Default: protect everything (nothing to prune) */

    for (int i = msgCount - 1; i >= 0; i--) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (msg.contains("role") && msg["role"] == "tool" && msg.contains("content")
            && msg["content"].is_string()) {
            int contentTokens = estimateTokens(
                QString::fromStdString(msg["content"].get<std::string>()));
            toolTokensFromEnd += contentTokens;
            if (toolTokensFromEnd >= agentConfig.pruneProtectTokens) {
                protectBoundary = i;
                break;
            }
        }
    }

    /* Calculate potential savings before modifying messages */
    int              potentialSavings = 0;
    std::vector<int> pruneIndices;

    for (int i = 0; i < protectBoundary; i++) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (msg.contains("role") && msg["role"] == "tool" && msg.contains("content")
            && msg["content"].is_string()) {
            int contentTokens = estimateTokens(
                QString::fromStdString(msg["content"].get<std::string>()));
            if (contentTokens > 100) {
                int prunedTokens = estimateTokens(QString("[output pruned]"));
                potentialSavings += contentTokens - prunedTokens;
                pruneIndices.push_back(i);
            }
        }
    }

    if (potentialSavings < agentConfig.pruneMinimumSavings) {
        return false;
    }

    /* Apply pruning */
    for (int idx : pruneIndices) {
        messages[static_cast<size_t>(idx)]["content"] = "[output pruned]";
    }

    if (agentConfig.verbose) {
        emit verboseOutput(QString("[Layer 1 Prune: saved ~%1 tokens, boundary at message %2/%3]")
                               .arg(potentialSavings)
                               .arg(protectBoundary)
                               .arg(msgCount));
    }

    return true;
}

int QSocAgent::findSafeBoundary(int proposedIndex) const
{
    int msgCount = static_cast<int>(messages.size());

    if (proposedIndex <= 0) {
        return 0;
    }
    if (proposedIndex >= msgCount) {
        return msgCount;
    }

    /* If proposed boundary lands on a tool message, walk backwards to include
     * the entire assistant(tool_calls) + tool group */
    int boundary = proposedIndex;

    while (boundary > 0) {
        const auto &msg = messages[static_cast<size_t>(boundary)];
        if (msg.contains("role") && msg["role"] == "tool") {
            /* This is a tool response - the assistant(tool_calls) must be before it */
            boundary--;
        } else {
            break;
        }
    }

    /* If we landed on an assistant message with tool_calls, include it too */
    if (boundary > 0) {
        const auto &msg = messages[static_cast<size_t>(boundary)];
        if (msg.contains("role") && msg["role"] == "assistant" && msg.contains("tool_calls")) {
            /* Don't split: move boundary before this assistant message */
            /* But we need to keep the whole group, so move boundary after the group */
            /* Actually, we want to include this group in the "old" section to be summarized,
             * so we find where the tool responses end */
            int groupEnd = boundary + 1;
            while (groupEnd < msgCount) {
                const auto &nextMsg = messages[static_cast<size_t>(groupEnd)];
                if (nextMsg.contains("role") && nextMsg["role"] == "tool") {
                    groupEnd++;
                } else {
                    break;
                }
            }
            boundary = groupEnd;
        }
    }

    return boundary;
}

QString QSocAgent::formatMessagesForSummary(int start, int end) const
{
    QString result;
    int     msgCount = static_cast<int>(messages.size());

    if (start < 0) {
        start = 0;
    }
    if (end > msgCount) {
        end = msgCount;
    }

    for (int i = start; i < end; i++) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (!msg.contains("role")) {
            continue;
        }

        QString role = QString::fromStdString(msg["role"].get<std::string>());

        if (role == "assistant" && msg.contains("tool_calls")) {
            result += QString("[Assistant called tools: ");
            for (const auto &tc : msg["tool_calls"]) {
                if (tc.contains("function") && tc["function"].contains("name")) {
                    result += QString::fromStdString(tc["function"]["name"].get<std::string>());
                    result += " ";
                }
            }
            result += "]\n";
        } else if (role == "tool") {
            QString content = msg.contains("content") && msg["content"].is_string()
                                  ? QString::fromStdString(msg["content"].get<std::string>())
                                  : "";
            /* Cap tool outputs in the summary input. Big enough to keep
             * file paths, error tails, and command output context; small
             * enough that a handful of multi-MB outputs cannot blow the
             * summarizer's own input budget. */
            if (content.length() > 2000) {
                content = content.left(1600) + "\n... (truncated, kept tail) ...\n"
                          + content.right(400);
            }
            result += QString("[Tool result: %1]\n").arg(content);
        } else if (msg.contains("content") && msg["content"].is_string()) {
            QString content = QString::fromStdString(msg["content"].get<std::string>());
            result += QString("[%1]: %2\n").arg(role, content);
        }
    }

    return result;
}

bool QSocAgent::compactWithLLM(bool force)
{
    if (!force) {
        int currentTokens = estimateTotalTokens();
        int compactTokens = static_cast<int>(
            effectiveContextTokens() * agentConfig.compactThreshold);
        if (currentTokens <= compactTokens) {
            return false;
        }
    }

    int msgCount = static_cast<int>(messages.size());

    /* Anchored / rolling summary: if messages[0] is a prior compaction
     * summary we generated, lift it out and feed it back as a
     * <previous-summary> anchor instead of re-summarizing it. Without
     * this, every successive /compact re-feeds an already-lossy
     * summary into the summarizer and information drifts away. */
    QString       previousSummary;
    int           summarizeStart = 0;
    const QString summaryMarker  = QStringLiteral("[Conversation Summary]\n");
    if (msgCount > 0) {
        const auto &first = messages[0];
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
        }
        return false;
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
        const auto &msg = messages[static_cast<size_t>(i)];
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
    int boundary         = findSafeBoundary(proposedBoundary);

    if (boundary <= summarizeStart) {
        return false;
    }

    /* Format old messages for summarization (skip the anchor itself) */
    QString oldContent = formatMessagesForSummary(summarizeStart, boundary);

    /* Try LLM summarization if service is available and not circuit-broken */
    QString              summary;
    bool                 llmSuccess         = false;
    static constexpr int maxCompactFailures = 3;

    /* If the conversation we want summarized is itself larger than the
     * model can accept in a single request, the LLM call would be
     * rejected outright. Skip straight to the mechanical fallback so
     * compaction still makes forward progress instead of crashing with
     * "Request Entity Too Large". Leaves 20% headroom for the summary
     * system prompt, completion tokens, and protocol overhead. */
    const int summaryInputTokens = estimateTokens(oldContent);
    const int compactBudget      = static_cast<int>(
        static_cast<double>(agentConfig.maxContextTokens) * 0.8);
    const bool llmCallable = compactFailureCount < maxCompactFailures && llmService
                             && llmService->hasEndpoint() && summaryInputTokens < compactBudget;

    if (llmCallable) {
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

        /* Honor an explicit compaction_model for the summary call only; an
         * empty value keeps the user's primary model. Captured before the
         * switch and restored right after, so normal turns are unaffected. */
        const QString priorModelId  = agentConfig.compactionModel.isEmpty()
                                          ? QString()
                                          : llmService->getCurrentModelId();
        const bool    modelSwitched = !agentConfig.compactionModel.isEmpty()
                                      && llmService->setCurrentModel(agentConfig.compactionModel);

        /* Use synchronous call - safe because we're at the start of processStreamIteration */
        json response = llmService->sendChatCompletion(summaryMessages, json::array(), 0.1);

        if (modelSwitched) {
            llmService->setCurrentModel(priorModelId);
        }

        if (response.contains("choices") && !response["choices"].empty()) {
            auto msg = response["choices"][0]["message"];
            if (msg.contains("content") && msg["content"].is_string()) {
                summary             = QString::fromStdString(msg["content"].get<std::string>());
                llmSuccess          = true;
                compactFailureCount = 0;
            }
        }

        if (!llmSuccess) {
            compactFailureCount++;
        }
    }

    /* Fallback: mechanical truncation if LLM failed, circuit-broken, or
     * the conversation was too large to even submit. Cap the summary at
     * a fraction of the context budget so the post-compact messages
     * still fit inside the model's window. */
    if (!llmSuccess) {
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Layer 2: Using mechanical summary (failures: %1/%2)]")
                                   .arg(compactFailureCount)
                                   .arg(maxCompactFailures));
        }
        const int summaryBudgetTokens
            = qMax(2048, static_cast<int>(agentConfig.maxContextTokens / 4));
        const int summaryBudgetChars = summaryBudgetTokens * 4; /* coarse token->char */
        QString   carryAnchor;
        if (!previousSummary.isEmpty()) {
            /* Carry the prior anchor so rolling state survives even
             * when the LLM call fails. */
            carryAnchor = QStringLiteral("[carried anchor]\n") + previousSummary
                          + QStringLiteral("\n[/carried anchor]\n");
        }
        summary = "[Previous conversation summary: " + carryAnchor;
        for (int i = summarizeStart; i < boundary; i++) {
            if (summary.size() >= summaryBudgetChars) {
                summary += "...(truncated)";
                break;
            }
            const auto &msg = messages[static_cast<size_t>(i)];
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

    for (int i = boundary; i < msgCount; i++) {
        newMessages.push_back(messages[static_cast<size_t>(i)]);
    }

    /* Compute savings before swapping so we can warn the user when the
     * compact accomplished nothing meaningful. The "recent kept zone
     * dominates" pattern shows up as `before == after`: the LLM call
     * burned latency for no reason. Surfacing it lets users decide to
     * /clear or shrink keepRecentMessages instead of looping. */
    const int beforeTokens = estimateMessagesTokens();
    messages               = newMessages;
    const int afterTokens  = estimateMessagesTokens();

    if (agentConfig.verbose) {
        const int    saved   = beforeTokens - afterTokens;
        const double savedPc = beforeTokens > 0 ? (100.0 * saved / beforeTokens) : 0.0;
        QString      tag;
        if (saved <= 0) {
            tag = " — no-op, recent zone already dominates; consider /clear";
        } else if (savedPc < 10.0) {
            tag = QString(" — only %1% saved, recent zone dominates").arg(savedPc, 0, 'f', 0);
        }
        emit verboseOutput(QString("[Layer 2 Compact: %1 -> %2 messages, ~%3 tokens%4%5]")
                               .arg(msgCount)
                               .arg(messages.size())
                               .arg(afterTokens)
                               .arg(llmSuccess ? "" : " (fallback)")
                               .arg(tag));
        /* Surface the head of the produced summary so users can verify
         * the template was followed (anchored vs fresh, section names
         * present, no leaked tool calls). Capped so the verbose stream
         * stays readable on big sessions. */
        const int     dumpChars = 600;
        const QString head      = summary.left(dumpChars);
        emit verboseOutput(QString("[Layer 2 Summary head%1]\n%2%3")
                               .arg(previousSummary.isEmpty() ? " (fresh)" : " (anchored)")
                               .arg(head)
                               .arg(summary.size() > dumpChars ? "\n... (truncated)" : ""));
    }

    return true;
}

void QSocAgent::compressHistoryIfNeeded()
{
    int originalTokens = estimateTotalTokens();
    int tokens         = originalTokens;

    /* Layer 1: Prune tool outputs */
    int pruneTokens = static_cast<int>(effectiveContextTokens() * agentConfig.pruneThreshold);
    if (tokens > pruneTokens) {
        if (pruneToolOutputs()) {
            tokens = estimateTotalTokens();
            emit compacting(1, originalTokens, tokens);
        }
    }

    /* Layer 2: LLM compaction */
    int compactTokens = static_cast<int>(effectiveContextTokens() * agentConfig.compactThreshold);
    if (tokens > compactTokens) {
        int beforeCompact = tokens;
        if (compactWithLLM()) {
            tokens = estimateTotalTokens();
            emit compacting(2, beforeCompact, tokens);
        }
    }

    /* Layer 3: Auto-continue after compaction during streaming */
    if (tokens < originalTokens && isStreaming) {
        addMessage(
            "user",
            "[System: Context compacted. Your persistent memory is still available "
            "in the system prompt. Continue your current task.]");
    }
}
