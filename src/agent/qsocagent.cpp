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

void QSocAgent::handleStreamError(const QString &error)
{
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

    /* Send streaming request */
    llmService->sendChatCompletionStream(messagesWithSystem, tools, agentConfig.temperature);
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

    /* Regular response without tool calls - we're done */
    isStreaming = false;
    heartbeatTimer->stop();

    if (message.contains("content") && message["content"].is_string()) {
        QString content = QString::fromStdString(message["content"].get<std::string>());

        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Assistant]: %1").arg(content));
        }

        /* Add to history */
        addMessage("assistant", content);
        emit runComplete(content);
    } else {
        addMessage("assistant", "");
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

void QSocAgent::compressHistoryIfNeeded()
{
    int currentTokens   = estimateMessagesTokens();
    int thresholdTokens = static_cast<int>(
        agentConfig.maxContextTokens * agentConfig.compressionThreshold);

    /* Only compress if we exceed threshold */
    if (currentTokens <= thresholdTokens) {
        return;
    }

    if (agentConfig.verbose) {
        emit verboseOutput(QString("[Compressing history: %1 tokens > %2 threshold]")
                               .arg(currentTokens)
                               .arg(thresholdTokens));
    }

    int messagesCount = static_cast<int>(messages.size());

    /* Keep at least keepRecentMessages */
    if (messagesCount <= agentConfig.keepRecentMessages) {
        if (agentConfig.verbose) {
            emit verboseOutput(QString("[Cannot compress: only %1 messages]").arg(messagesCount));
        }
        return;
    }

    /* Create summary of old messages */
    QString summary  = "[Previous conversation summary: ";
    int     oldCount = messagesCount - agentConfig.keepRecentMessages;

    for (int i = 0; i < oldCount; i++) {
        const auto &msg = messages[static_cast<size_t>(i)];
        if (msg.contains("role") && msg.contains("content") && msg["content"].is_string()) {
            QString role    = QString::fromStdString(msg["role"].get<std::string>());
            QString content = QString::fromStdString(msg["content"].get<std::string>());

            /* Truncate long content */
            if (content.length() > 100) {
                content = content.left(100) + "...";
            }

            summary += role + ": " + content + "; ";
        }
    }
    summary += "]";

    /* Keep recent messages */
    json newMessages = json::array();

    /* Add summary as first message */
    newMessages.push_back({{"role", "system"}, {"content", summary.toStdString()}});

    /* Keep recent messages */
    for (int i = messagesCount - agentConfig.keepRecentMessages; i < messagesCount; i++) {
        newMessages.push_back(messages[static_cast<size_t>(i)]);
    }

    messages = newMessages;

    if (agentConfig.verbose) {
        emit verboseOutput(QString("[Compressed from %1 to %2 messages. New token estimate: %3]")
                               .arg(messagesCount)
                               .arg(messages.size())
                               .arg(estimateMessagesTokens()));
    }
}
