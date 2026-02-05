// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"

#include <QDebug>

QSocAgent::QSocAgent(
    QObject *parent, QLLMService *llmService, QSocToolRegistry *toolRegistry, QSocAgentConfig config)
    : QObject(parent)
    , llmService_(llmService)
    , toolRegistry_(toolRegistry)
    , config_(std::move(config))
    , messages_(json::array())
{}

QSocAgent::~QSocAgent()
{
    /* Disconnect streaming signals */
    if (llmService_) {
        disconnect(llmService_, nullptr, this, nullptr);
    }
}

QString QSocAgent::run(const QString &userQuery)
{
    /* Add user message to history */
    addMessage("user", userQuery);

    /* Agent loop */
    int iteration = 0;

    while (iteration < config_.maxIterations) {
        iteration++;

        /* Check and compress history if needed */
        compressHistoryIfNeeded();

        int currentTokens = estimateMessagesTokens();

        if (config_.verbose) {
            QString info = QString("[Iteration %1 | Tokens: %2/%3 (%4%) | Messages: %5]")
                               .arg(iteration)
                               .arg(currentTokens)
                               .arg(config_.maxContextTokens)
                               .arg(100.0 * currentTokens / config_.maxContextTokens, 0, 'f', 1)
                               .arg(messages_.size());
            emit verboseOutput(info);
        }

        /* Process one iteration */
        bool isComplete = processIteration();

        if (isComplete) {
            /* Get the final assistant message */
            if (!messages_.empty()) {
                auto lastMessage = messages_.back();
                if (lastMessage["role"] == "assistant" && lastMessage.contains("content")
                    && lastMessage["content"].is_string()) {
                    return QString::fromStdString(lastMessage["content"].get<std::string>());
                }
            }
            return "[Agent completed without final message]";
        }
    }

    return QString("[Agent safety limit reached (%1 iterations)]").arg(config_.maxIterations);
}

void QSocAgent::runStream(const QString &userQuery)
{
    if (!llmService_ || !toolRegistry_) {
        emit runError("LLM service or tool registry not configured");
        return;
    }

    /* Add user message to history */
    addMessage("user", userQuery);

    /* Setup streaming */
    isStreaming_     = true;
    streamIteration_ = 0;
    streamFinalContent_.clear();

    /* Connect to LLM streaming signals */
    connect(
        llmService_,
        &QLLMService::streamChunk,
        this,
        [this](const QString &chunk) { emit contentChunk(chunk); },
        Qt::UniqueConnection);

    connect(
        llmService_,
        &QLLMService::streamComplete,
        this,
        &QSocAgent::handleStreamComplete,
        Qt::UniqueConnection);

    connect(
        llmService_,
        &QLLMService::streamError,
        this,
        [this](const QString &error) {
            isStreaming_ = false;
            emit runError(error);
        },
        Qt::UniqueConnection);

    /* Start first iteration */
    processStreamIteration();
}

void QSocAgent::processStreamIteration()
{
    if (!isStreaming_) {
        return;
    }

    streamIteration_++;

    if (streamIteration_ > config_.maxIterations) {
        isStreaming_ = false;
        emit runError(
            QString("[Agent safety limit reached (%1 iterations)]").arg(config_.maxIterations));
        return;
    }

    /* Check and compress history if needed */
    compressHistoryIfNeeded();

    if (config_.verbose) {
        int     currentTokens = estimateMessagesTokens();
        QString info          = QString("[Iteration %1 | Tokens: %2/%3 (%4%) | Messages: %5]")
                           .arg(streamIteration_)
                           .arg(currentTokens)
                           .arg(config_.maxContextTokens)
                           .arg(100.0 * currentTokens / config_.maxContextTokens, 0, 'f', 1)
                           .arg(messages_.size());
        emit verboseOutput(info);
    }

    /* Build messages with system prompt */
    json messagesWithSystem = json::array();

    if (!config_.systemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", config_.systemPrompt.toStdString()}});
    }

    for (const auto &msg : messages_) {
        messagesWithSystem.push_back(msg);
    }

    /* Get tool definitions */
    json tools = toolRegistry_->getToolDefinitions();

    /* Send streaming request */
    llmService_->sendChatCompletionStream(messagesWithSystem, tools, config_.temperature);
}

void QSocAgent::handleStreamComplete(const json &response)
{
    if (!isStreaming_) {
        return;
    }

    /* Check for errors */
    if (response.contains("error")) {
        QString errorMsg = QString::fromStdString(response["error"].get<std::string>());
        isStreaming_     = false;
        emit runError(errorMsg);
        return;
    }

    /* Extract assistant message */
    if (!response.contains("choices") || response["choices"].empty()) {
        isStreaming_ = false;
        emit runError("Invalid response from LLM");
        return;
    }

    auto message = response["choices"][0]["message"];

    /* Check for tool calls */
    if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
        /* Add assistant message with tool calls to history */
        messages_.push_back(message);

        if (config_.verbose) {
            emit verboseOutput("[Assistant requesting tool calls]");
        }

        /* Handle tool calls synchronously */
        handleToolCalls(message["tool_calls"]);

        /* Continue with next iteration */
        processStreamIteration();
        return;
    }

    /* Regular response without tool calls - we're done */
    isStreaming_ = false;

    if (message.contains("content") && message["content"].is_string()) {
        QString content = QString::fromStdString(message["content"].get<std::string>());

        if (config_.verbose) {
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
    if (!llmService_ || !toolRegistry_) {
        qWarning() << "LLM service or tool registry not configured";
        return true;
    }

    /* Build messages with system prompt */
    json messagesWithSystem = json::array();

    /* Add system prompt as first message */
    if (!config_.systemPrompt.isEmpty()) {
        messagesWithSystem.push_back(
            {{"role", "system"}, {"content", config_.systemPrompt.toStdString()}});
    }

    /* Add conversation history */
    for (const auto &msg : messages_) {
        messagesWithSystem.push_back(msg);
    }

    /* Get tool definitions */
    json tools = toolRegistry_->getToolDefinitions();

    /* Call LLM */
    json response = llmService_->sendChatCompletion(messagesWithSystem, tools, config_.temperature);

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
        messages_.push_back(message);

        if (config_.verbose) {
            emit verboseOutput("[Assistant requesting tool calls]");
        }

        /* Handle tool calls */
        handleToolCalls(message["tool_calls"]);

        return false; /* Not complete yet, need to continue */
    }

    /* Regular response without tool calls */
    if (message.contains("content") && message["content"].is_string()) {
        QString content = QString::fromStdString(message["content"].get<std::string>());

        if (config_.verbose) {
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
    for (const auto &toolCall : toolCalls) {
        QString toolCallId   = QString::fromStdString(toolCall["id"].get<std::string>());
        QString functionName = QString::fromStdString(
            toolCall["function"]["name"].get<std::string>());
        QString argumentsStr = QString::fromStdString(
            toolCall["function"]["arguments"].get<std::string>());

        if (config_.verbose) {
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
        QString result = toolRegistry_->executeTool(functionName, arguments);

        if (config_.verbose) {
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
    messages_.push_back({{"role", role.toStdString()}, {"content", content.toStdString()}});
}

void QSocAgent::addToolMessage(const QString &toolCallId, const QString &content)
{
    messages_.push_back(
        {{"role", "tool"},
         {"tool_call_id", toolCallId.toStdString()},
         {"content", content.toStdString()}});
}

void QSocAgent::clearHistory()
{
    messages_ = json::array();
}

void QSocAgent::setLLMService(QLLMService *llmService)
{
    llmService_ = llmService;
}

void QSocAgent::setToolRegistry(QSocToolRegistry *toolRegistry)
{
    toolRegistry_ = toolRegistry;
}

void QSocAgent::setConfig(const QSocAgentConfig &config)
{
    config_ = config;
}

QSocAgentConfig QSocAgent::getConfig() const
{
    return config_;
}

json QSocAgent::getMessages() const
{
    return messages_;
}

int QSocAgent::estimateTokens(const QString &text) const
{
    /* Simple estimation: approximately 4 characters per token */
    return static_cast<int>(text.length() / 4);
}

int QSocAgent::estimateMessagesTokens() const
{
    int total = 0;
    for (const auto &msg : messages_) {
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
    int thresholdTokens = static_cast<int>(config_.maxContextTokens * config_.compressionThreshold);

    /* Only compress if we exceed threshold */
    if (currentTokens <= thresholdTokens) {
        return;
    }

    if (config_.verbose) {
        emit verboseOutput(QString("[Compressing history: %1 tokens > %2 threshold]")
                               .arg(currentTokens)
                               .arg(thresholdTokens));
    }

    int messagesCount = static_cast<int>(messages_.size());

    /* Keep at least keepRecentMessages */
    if (messagesCount <= config_.keepRecentMessages) {
        if (config_.verbose) {
            emit verboseOutput(QString("[Cannot compress: only %1 messages]").arg(messagesCount));
        }
        return;
    }

    /* Create summary of old messages */
    QString summary  = "[Previous conversation summary: ";
    int     oldCount = messagesCount - config_.keepRecentMessages;

    for (int i = 0; i < oldCount; i++) {
        const auto &msg = messages_[static_cast<size_t>(i)];
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
    for (int i = messagesCount - config_.keepRecentMessages; i < messagesCount; i++) {
        newMessages.push_back(messages_[static_cast<size_t>(i)]);
    }

    messages_ = newMessages;

    if (config_.verbose) {
        emit verboseOutput(QString("[Compressed from %1 to %2 messages. New token estimate: %3]")
                               .arg(messagesCount)
                               .arg(messages_.size())
                               .arg(estimateMessagesTokens()));
    }
}
