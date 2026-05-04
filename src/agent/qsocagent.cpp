// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"

#include "agent/qsochookmanager.h"
#include "agent/qsochooktypes.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/qsocconsole.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QSysInfo>
#include <QTextStream>

QSocAgent::QSocAgent(
    QObject *parent, QLLMService *llmService, QSocToolRegistry *toolRegistry, QSocAgentConfig config)
    : QObject(parent)
    , llmService(llmService)
    , toolRegistry(toolRegistry)
    , agentConfig(std::move(config))
    , messages(json::array())
    , heartbeatTimer(new QTimer(this))
{
    /* Setup heartbeat timer - fires every 5 seconds during operation */
    heartbeatTimer->setInterval(5000);
    connect(heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (isStreaming) {
            int  elapsed = static_cast<int>(runElapsedTimer.elapsed() / 1000);
            emit heartbeat(streamIteration, elapsed);

            /* Emit token usage update */
            emit tokenUsage(totalInputTokens.load(), totalOutputTokens.load());

            /* Stuck detection: check if no progress for configured threshold */
            if (agentConfig.enableStuckDetection) {
                qint64 now      = QDateTime::currentMSecsSinceEpoch();
                qint64 lastProg = lastProgressTime.load();
                if (lastProg > 0) {
                    int silentSeconds = static_cast<int>((now - lastProg) / 1000);
                    if (silentSeconds >= agentConfig.stuckThresholdSeconds) {
                        /* Reset to avoid repeated triggers */
                        lastProgressTime = now;
                        emit stuckDetected(streamIteration, silentSeconds);

                        /* Auto status check: inject status query */
                        if (agentConfig.autoStatusCheck) {
                            queueRequest(
                                "[System: No progress detected. Please briefly report: "
                                "1) What are you doing? 2) Any issues? 3) Estimated time "
                                "remaining?]");
                        }
                    }
                }
            }
        }
    });
}

QSocAgent::~QSocAgent()
{
    /* Qt automatically disconnects signals when either sender or receiver is destroyed */
    /* No manual disconnect needed - doing so can cause crashes if llmService is already destroyed */
}

bool QSocAgent::isToolAllowed(const QString &name) const
{
    /* Sub-agents must never spawn further sub-agents: closes the
     * recursion door regardless of allowlist. */
    if (agentConfig.isSubAgent && name == QStringLiteral("agent")) {
        return false;
    }
    /* Allowlist gate: empty list inherits everything. */
    if (!agentConfig.toolsAllow.isEmpty() && !agentConfig.toolsAllow.contains(name)) {
        return false;
    }
    /* Denylist gate: applied AFTER allowlist. Used to subtract a
     * couple of tools from "inherit everything" or from a broad
     * allowlist. Mirrors claude-code's `disallowedTools`. */
    if (agentConfig.toolsDeny.contains(name)) {
        return false;
    }
    return true;
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
    if (agentConfig.toolsAllow.isEmpty() && !agentConfig.isSubAgent) {
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

    /* Agent loop */
    int       iteration = 0;
    const int turnCap   = agentConfig.maxTurnsOverride > 0 ? agentConfig.maxTurnsOverride
                                                           : agentConfig.maxIterations;

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

        if (isComplete) {
            /* Get the final assistant message */
            if (!messages.empty()) {
                auto lastMessage = messages.back();
                if (lastMessage["role"] == "assistant" && lastMessage.contains("content")
                    && lastMessage["content"].is_string()) {
                    return QString::fromStdString(lastMessage["content"].get<std::string>());
                }
            }
            return "[Agent completed without final message]";
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
    isStreaming               = true;
    streamIteration           = 0;
    currentRetryCount         = 0;
    contextOverflowRetryCount = 0;
    streamFinalContent.clear();
    abortRequested   = false;
    lastProgressTime = QDateTime::currentMSecsSinceEpoch();

    /* Reset token counters for this run */
    totalInputTokens  = 0;
    totalOutputTokens = 0;

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
    lastProgressTime = QDateTime::currentMSecsSinceEpoch();

    /* Estimate output tokens from this chunk */
    int chunkTokens = estimateTokens(chunk);
    totalOutputTokens.fetch_add(chunkTokens);

    emit contentChunk(chunk);
}

void QSocAgent::handleReasoningChunk(const QString &chunk)
{
    lastProgressTime = QDateTime::currentMSecsSinceEpoch();
    int chunkTokens  = estimateTokens(chunk);
    totalOutputTokens.fetch_add(chunkTokens);
    emit reasoningChunk(chunk);
}

void QSocAgent::handleStreamError(const QString &error)
{
    /* Check if this error was caused by user abort */
    if (abortRequested) {
        isStreaming = false;
        heartbeatTimer->stop();
        abortRequested = false;
        emit runAborted(streamFinalContent);
        return;
    }

    /* Reactive compaction: when the server rejects the request because
     * the prompt overshoots the context window, force a compact and
     * retry once. Beats letting the user re-trigger manually because
     * our estimator was off (CJK + base64 tool results are the usual
     * culprit). Capped at maxCompactRetries to avoid infinite loops
     * when even the post-compact prompt still does not fit. */
    static constexpr int maxCompactRetries = 2;
    const bool isContextOverflow = error.contains("Entity Too Large", Qt::CaseInsensitive)
                                   || error.contains("context_length_exceeded", Qt::CaseInsensitive)
                                   || error.contains("maximum context length", Qt::CaseInsensitive)
                                   || error.contains("prompt is too long", Qt::CaseInsensitive)
                                   || error.contains("400", Qt::CaseSensitive); /* DeepSeek's 400 */
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
        lastProgressTime = QDateTime::currentMSecsSinceEpoch();
        processStreamIteration();
        return;
    }

    /* Check if error is retryable (timeout or network error) */
    bool isRetryable = error.contains("timeout", Qt::CaseInsensitive)
                       || error.contains("network", Qt::CaseInsensitive)
                       || error.contains("connection", Qt::CaseInsensitive);

    if (isRetryable && currentRetryCount < agentConfig.maxRetries) {
        currentRetryCount++;

        /* Always emit retrying signal for UI feedback */
        emit retrying(currentRetryCount, agentConfig.maxRetries, error);

        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Retry %1/%2: %3]")
                                   .arg(currentRetryCount)
                                   .arg(agentConfig.maxRetries)
                                   .arg(error));
        }

        /* Reset progress timer for retry */
        lastProgressTime = QDateTime::currentMSecsSinceEpoch();

        /* Retry the current iteration */
        processStreamIteration();
        return;
    }

    /* No more retries or non-retryable error */
    isStreaming = false;
    heartbeatTimer->stop();
    currentRetryCount         = 0;
    contextOverflowRetryCount = 0;
    emit runError(error);
}

void QSocAgent::processStreamIteration()
{
    if (!isStreaming) {
        return;
    }

    /* Check for abort request */
    if (abortRequested) {
        isStreaming = false;
        heartbeatTimer->stop();
        abortRequested = false;
        emit runAborted(streamFinalContent);
        return;
    }

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
        emit runError(errorMsg);
        return;
    }

    /* Extract assistant message */
    if (!response.contains("choices") || response["choices"].empty()) {
        isStreaming = false;
        heartbeatTimer->stop();
        emit runError("Invalid response from LLM");
        return;
    }

    auto message = response["choices"][0]["message"];

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

    /* Regular response without tool calls */
    if (message.contains("content") && message["content"].is_string()) {
        QString content = QString::fromStdString(message["content"].get<std::string>());

        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Assistant]: %1").arg(content));
        }

        /* Push full message to preserve reasoning_content for DeepSeek R1 */
        messages.push_back(message);

        /* Continue if there are queued requests */
        if (hasPendingRequests()) {
            processStreamIteration();
            return;
        }

        isStreaming = false;
        heartbeatTimer->stop();
        fireStopHook(content);
        emit runComplete(content);
    } else {
        /* Push full message to preserve reasoning_content for DeepSeek R1 */
        messages.push_back(message);

        /* Continue if there are queued requests */
        if (hasPendingRequests()) {
            processStreamIteration();
            return;
        }

        isStreaming = false;
        heartbeatTimer->stop();
        fireStopHook(QString());
        emit runComplete("");
    }
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

    /* Add conversation history (strip the internal `_usage` annotation
     * so the wire stays standard chat completion shape). */
    for (const auto &msg : messages) {
        json sanitized = msg;
        sanitized.erase("_usage");
        messagesWithSystem.push_back(sanitized);
    }

    /* Critical reminder: same per-turn re-injection as the streaming
     * path; defends against drift on long sync runs. */
    if (!agentConfig.criticalReminder.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", agentConfig.criticalReminder.toStdString()}});
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
    lastProgressTime = QDateTime::currentMSecsSinceEpoch();

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
        if (!isToolAllowed(functionName)) {
            const QString denied = QStringLiteral(
                                       "Error: tool \"%1\" is not allowed in this sub-agent")
                                       .arg(functionName);
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

    /* Section 11: Memory */
    if (agentConfig.autoLoadMemory && memoryManager) {
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
    json msg = {{"role", "tool"}, {"tool_call_id", toolCallId.toStdString()}};
    if (attachments.isEmpty()) {
        msg["content"] = content.toStdString();
        messages.push_back(msg);
        return;
    }

    /* OpenAI-compatible content array: one text part for the model's
     * narrative summary, then one image_url part per attachment using
     * a data URL so providers that cannot egress to the original host
     * (self-hosted vLLM/SGLang/Ollama) still see the bytes. */
    json contentArr = json::array();
    if (!content.isEmpty()) {
        contentArr.push_back({{"type", "text"}, {"text", content.toStdString()}});
    }
    for (const auto &att : attachments) {
        const QString dataUrl = QStringLiteral("data:%1;base64,%2").arg(att.mime, att.dataB64);
        contentArr.push_back(
            {{"type", "image_url"}, {"image_url", {{"url", dataUrl.toStdString()}}}});
    }
    msg["content"] = contentArr;
    messages.push_back(msg);
}

void QSocAgent::clearHistory()
{
    messages = json::array();
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

    /* Cascade to LLM stream */
    if (llmService) {
        llmService->abortStream();
    }

    /* Cascade to running tools */
    if (toolRegistry) {
        toolRegistry->abortAll();
    }
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

        /* Use synchronous call - safe because we're at the start of processStreamIteration */
        json response = llmService->sendChatCompletion(summaryMessages, json::array(), 0.1);

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
