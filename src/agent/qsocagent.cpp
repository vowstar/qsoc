// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"

#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>

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

QString QSocAgent::run(const QString &userQuery)
{
    /* Add user message to history */
    addMessage("user", userQuery);

    /* Agent loop */
    int iteration = 0;

    while (iteration < agentConfig.maxIterations) {
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
            emit verboseOutput(info);
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

    return QString("[Agent safety limit reached (%1 iterations)]").arg(agentConfig.maxIterations);
}

void QSocAgent::runStream(const QString &userQuery)
{
    if (!llmService || !toolRegistry) {
        emit runError("LLM service or tool registry not configured");
        return;
    }

    /* Add user message to history */
    addMessage("user", userQuery);

    /* Setup streaming */
    isStreaming       = true;
    streamIteration   = 0;
    currentRetryCount = 0;
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
    currentRetryCount = 0;
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
            QString newRequest = requestQueue.takeFirst();
            int     remaining  = requestQueue.size();
            locker.unlock();

            emit processingQueuedRequest(newRequest, remaining);

            /* Add new user message - this will cause LLM to reconsider */
            addMessage("user", newRequest);

            locker.relock();
        }
    }

    streamIteration++;

    if (streamIteration > agentConfig.maxIterations) {
        isStreaming = false;
        heartbeatTimer->stop();
        emit runError(
            QString("[Agent safety limit reached (%1 iterations)]").arg(agentConfig.maxIterations));
        return;
    }

    /* Check and compress history if needed */
    compressHistoryIfNeeded();

    if (agentConfig.verbose) {
        int     currentTokens = estimateMessagesTokens();
        QString info          = QString("[Iteration %1 | Tokens: %2/%3 (%4%) | Messages: %5]")
                           .arg(streamIteration)
                           .arg(currentTokens)
                           .arg(agentConfig.maxContextTokens)
                           .arg(100.0 * currentTokens / agentConfig.maxContextTokens, 0, 'f', 1)
                           .arg(messages.size());
        emit verboseOutput(info);
    }

    /* Build messages with system prompt */
    json messagesWithSystem = json::array();

    if (!agentConfig.systemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", agentConfig.systemPrompt.toStdString()}});
    }

    for (const auto &msg : messages) {
        messagesWithSystem.push_back(msg);
    }

    /* Get tool definitions */
    json tools = toolRegistry->getToolDefinitions();

    /* Estimate input tokens for this request */
    int inputTokens = estimateMessagesTokens();
    totalInputTokens.fetch_add(inputTokens);

    /* Determine model override for reasoning */
    QString modelOverride;
    if (!agentConfig.thinkingLevel.isEmpty() && !agentConfig.reasoningModel.isEmpty()) {
        modelOverride = agentConfig.reasoningModel;
    }

    /* Send streaming request */
    llmService->sendChatCompletionStream(
        messagesWithSystem, tools, agentConfig.temperature, agentConfig.thinkingLevel, modelOverride);
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
        emit runComplete("");
    }
}

bool QSocAgent::processIteration()
{
    if (!llmService || !toolRegistry) {
        qWarning() << "LLM service or tool registry not configured";
        return true;
    }

    /* Build messages with system prompt */
    json messagesWithSystem = json::array();

    /* Add system prompt as first message */
    if (!agentConfig.systemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", agentConfig.systemPrompt.toStdString()}});
    }

    /* Add conversation history */
    for (const auto &msg : messages) {
        messagesWithSystem.push_back(msg);
    }

    /* Get tool definitions */
    json tools = toolRegistry->getToolDefinitions();

    /* Call LLM */
    json response
        = llmService->sendChatCompletion(messagesWithSystem, tools, agentConfig.temperature);

    /* Check for errors */
    if (response.contains("error")) {
        QString errorMsg = QString::fromStdString(response["error"].get<std::string>());
        qWarning() << "LLM error:" << errorMsg;
        addMessage("assistant", QString("Error: %1").arg(errorMsg));
        return true;
    }

    /* Extract assistant message */
    if (!response.contains("choices") || response["choices"].empty()) {
        qWarning() << "Invalid LLM response: no choices";
        addMessage("assistant", "Error: Invalid response from LLM");
        return true;
    }

    auto message = response["choices"][0]["message"];

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

        /* Execute tool */
        QString result = toolRegistry->executeTool(functionName, arguments);

        if (agentConfig.verbose) {
            QString truncatedResult = result.length() > 200 ? result.left(200) + "... (truncated)"
                                                            : result;
            emit    verboseOutput(QString("     Result: %1").arg(truncatedResult));
        }

        emit toolResult(functionName, result);

        /* Add tool response to messages */
        addToolMessage(toolCallId, result);
    }
}

void QSocAgent::addMessage(const QString &role, const QString &content)
{
    messages.push_back({{"role", role.toStdString()}, {"content", content.toStdString()}});
}

void QSocAgent::addToolMessage(const QString &toolCallId, const QString &content)
{
    messages.push_back(
        {{"role", "tool"},
         {"tool_call_id", toolCallId.toStdString()},
         {"content", content.toStdString()}});
}

void QSocAgent::clearHistory()
{
    messages = json::array();
}

void QSocAgent::queueRequest(const QString &request)
{
    QMutexLocker locker(&queueMutex);
    requestQueue.append(request);
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

void QSocAgent::setThinkingLevel(const QString &level)
{
    agentConfig.thinkingLevel = level;
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
    /* Simple estimation: approximately 4 characters per token */
    return static_cast<int>(text.length() / 4);
}

int QSocAgent::estimateMessagesTokens() const
{
    int total = 0;
    for (const auto &msg : messages) {
        /* Estimate content */
        if (msg.contains("content") && msg["content"].is_string()) {
            total += estimateTokens(QString::fromStdString(msg["content"].get<std::string>()));
        }
        /* Estimate tool calls (rough) */
        if (msg.contains("tool_calls")) {
            total += estimateTokens(QString::fromStdString(msg["tool_calls"].dump()));
        }
        /* Add overhead for message structure (~10 tokens per message) */
        total += 10;
    }
    return total;
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
        int currentTokens = estimateMessagesTokens();
        int pruneTokens   = static_cast<int>(
            agentConfig.maxContextTokens * agentConfig.pruneThreshold);
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
            /* Truncate large tool outputs for the summary prompt */
            if (content.length() > 500) {
                content = content.left(500) + "... (truncated)";
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
        int currentTokens = estimateMessagesTokens();
        int compactTokens = static_cast<int>(
            agentConfig.maxContextTokens * agentConfig.compactThreshold);
        if (currentTokens <= compactTokens) {
            return false;
        }
    }

    int msgCount = static_cast<int>(messages.size());

    if (msgCount <= agentConfig.keepRecentMessages) {
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Layer 2: Cannot compact, only %1 messages]").arg(msgCount));
        }
        return false;
    }

    /* Determine boundary: keep recent messages */
    int proposedBoundary = msgCount - agentConfig.keepRecentMessages;
    int boundary         = findSafeBoundary(proposedBoundary);

    if (boundary <= 0) {
        return false;
    }

    /* Format old messages for summarization */
    QString oldContent = formatMessagesForSummary(0, boundary);

    /* Try LLM summarization if service is available */
    QString summary;
    bool    llmSuccess = false;

    if (llmService && llmService->hasEndpoint()) {
        QString summaryPrompt
            = QString(
                  "You are a conversation summarizer. Produce a structured summary of the "
                  "following "
                  "conversation.\n\n"
                  "## Instructions\n"
                  "- Preserve ALL technical details: file paths, command outputs, error messages\n"
                  "- Preserve ALL decisions and their reasoning\n"
                  "- Preserve current task state and next steps\n"
                  "- Be concise but never lose actionable information\n\n"
                  "## Required Sections\n"
                  "### Task Overview\n"
                  "### Current State\n"
                  "### Key Files and Paths\n"
                  "### Decisions Made\n"
                  "### Important Context\n"
                  "### Next Steps\n\n"
                  "## Conversation to summarize:\n%1")
                  .arg(oldContent);

        /* Build messages for the summarization request */
        json summaryMessages = json::array();
        summaryMessages.push_back(
            {{"role", "system"},
             {"content", "You are a precise conversation summarizer. Output only the summary."}});
        summaryMessages.push_back({{"role", "user"}, {"content", summaryPrompt.toStdString()}});

        /* Use synchronous call - safe because we're at the start of processStreamIteration */
        json response = llmService->sendChatCompletion(summaryMessages, json::array(), 0.1);

        if (response.contains("choices") && !response["choices"].empty()) {
            auto msg = response["choices"][0]["message"];
            if (msg.contains("content") && msg["content"].is_string()) {
                summary    = QString::fromStdString(msg["content"].get<std::string>());
                llmSuccess = true;
            }
        }
    }

    /* Fallback: mechanical truncation if LLM failed */
    if (!llmSuccess) {
        if (agentConfig.verbose) {
            emit verboseOutput("[Layer 2: LLM unavailable, using mechanical summary]");
        }
        summary = "[Previous conversation summary: ";
        for (int i = 0; i < boundary; i++) {
            const auto &msg = messages[static_cast<size_t>(i)];
            if (msg.contains("role") && msg.contains("content") && msg["content"].is_string()) {
                QString role    = QString::fromStdString(msg["role"].get<std::string>());
                QString content = QString::fromStdString(msg["content"].get<std::string>());
                if (content.length() > 100) {
                    content = content.left(100) + "...";
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

    messages = newMessages;

    if (agentConfig.verbose) {
        emit verboseOutput(QString("[Layer 2 Compact: %1 -> %2 messages, ~%3 tokens%4]")
                               .arg(msgCount)
                               .arg(messages.size())
                               .arg(estimateMessagesTokens())
                               .arg(llmSuccess ? "" : " (fallback)"));
    }

    return true;
}

void QSocAgent::compressHistoryIfNeeded()
{
    int originalTokens = estimateMessagesTokens();
    int tokens         = originalTokens;

    /* Layer 1: Prune tool outputs (60% threshold) */
    int pruneTokens = static_cast<int>(agentConfig.maxContextTokens * agentConfig.pruneThreshold);
    if (tokens > pruneTokens) {
        if (pruneToolOutputs()) {
            tokens = estimateMessagesTokens();
            emit compacting(1, originalTokens, tokens);
        }
    }

    /* Layer 2: LLM compaction (80% threshold) */
    int compactTokens = static_cast<int>(
        agentConfig.maxContextTokens * agentConfig.compactThreshold);
    if (tokens > compactTokens) {
        int beforeCompact = tokens;
        if (compactWithLLM()) {
            tokens = estimateMessagesTokens();
            emit compacting(2, beforeCompact, tokens);
        }
    }

    /* Layer 3: Auto-continue after compaction during streaming */
    if (tokens < originalTokens && isStreaming) {
        addMessage("user", "[System: Context compacted. Continue your current task.]");
    }
}
