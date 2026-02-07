// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENT_H
#define QSOCAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"

#include <atomic>
#include <nlohmann/json.hpp>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

using json = nlohmann::json;

/**
 * @brief The QSocAgent class provides an AI agent for SoC design automation
 * @details Implements an agent loop that interacts with an LLM using tool calling
 *          to perform various SoC design tasks. The agent maintains conversation
 *          history and handles tool execution automatically.
 */
class QSocAgent : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent QObject
     * @param llmService LLM service for API calls
     * @param toolRegistry Registry of available tools
     * @param config Agent configuration
     */
    explicit QSocAgent(
        QObject          *parent       = nullptr,
        QLLMService      *llmService   = nullptr,
        QSocToolRegistry *toolRegistry = nullptr,
        QSocAgentConfig   config       = QSocAgentConfig());

    /**
     * @brief Destructor
     */
    ~QSocAgent() override;

    /**
     * @brief Run the agent with a user query
     * @param userQuery The user's input query
     * @return The agent's final response
     */
    QString run(const QString &userQuery);

    /**
     * @brief Run the agent with streaming output
     * @details Connects to LLM streaming and emits contentChunk for real-time output.
     *          Returns after the agent completes (may involve multiple tool calls).
     * @param userQuery The user's input query
     */
    void runStream(const QString &userQuery);

    /**
     * @brief Clear the conversation history
     */
    void clearHistory();

    /**
     * @brief Queue a new user request to be processed at the next opportunity
     * @details If agent is running, the request will be injected at the next
     *          iteration checkpoint. If not running, it will be processed
     *          when run() or runStream() is called.
     * @param request The user request to queue
     */
    void queueRequest(const QString &request);

    /**
     * @brief Check if there are pending requests in the queue
     * @return True if there are pending requests
     */
    bool hasPendingRequests() const;

    /**
     * @brief Get the number of pending requests
     * @return Number of requests in the queue
     */
    int pendingRequestCount() const;

    /**
     * @brief Clear all pending requests
     */
    void clearPendingRequests();

    /**
     * @brief Abort the current operation
     * @details Stops the agent at the next checkpoint and emits runAborted signal
     */
    void abort();

    /**
     * @brief Check if agent is currently running
     * @return True if agent is processing a request
     */
    bool isRunning() const;

    /**
     * @brief Set the LLM service
     * @param llmService Pointer to the LLM service
     */
    void setLLMService(QLLMService *llmService);

    /**
     * @brief Set the tool registry
     * @param toolRegistry Pointer to the tool registry
     */
    void setToolRegistry(QSocToolRegistry *toolRegistry);

    /**
     * @brief Set the agent configuration
     * @param config Agent configuration
     */
    void setConfig(const QSocAgentConfig &config);

    /**
     * @brief Get the current configuration
     * @return Current agent configuration
     */
    QSocAgentConfig getConfig() const;

    /**
     * @brief Get the conversation history
     * @return JSON array of messages
     */
    json getMessages() const;

    /**
     * @brief Set the conversation history
     * @param msgs JSON array of messages to restore
     */
    void setMessages(const json &msgs);

signals:
    /**
     * @brief Signal emitted when a tool is called
     * @param toolName Name of the tool being called
     * @param arguments Arguments passed to the tool
     */
    void toolCalled(const QString &toolName, const QString &arguments);

    /**
     * @brief Signal emitted when a tool returns a result
     * @param toolName Name of the tool
     * @param result Result from the tool
     */
    void toolResult(const QString &toolName, const QString &result);

    /**
     * @brief Signal emitted for verbose output
     * @param message The verbose message
     */
    void verboseOutput(const QString &message);

    /**
     * @brief Signal emitted for each content chunk during streaming
     * @param chunk The content chunk
     */
    void contentChunk(const QString &chunk);

    /**
     * @brief Signal emitted when streaming run is complete
     * @param response The complete response
     */
    void runComplete(const QString &response);

    /**
     * @brief Signal emitted when an error occurs during streaming
     * @param error Error message
     */
    void runError(const QString &error);

    /**
     * @brief Signal emitted periodically during long operations
     * @param iteration Current iteration number
     * @param elapsedSeconds Total elapsed time in seconds
     */
    void heartbeat(int iteration, int elapsedSeconds);

    /**
     * @brief Signal emitted when a queued request is being processed
     * @param request The request being processed
     * @param queueSize Remaining requests in queue
     */
    void processingQueuedRequest(const QString &request, int queueSize);

    /**
     * @brief Signal emitted when operation is aborted by user
     * @param partialResult Any partial result accumulated so far
     */
    void runAborted(const QString &partialResult);

    /**
     * @brief Signal emitted when stuck is detected (no progress for configured threshold)
     * @param iteration Current iteration number
     * @param silentSeconds Number of seconds without progress
     */
    void stuckDetected(int iteration, int silentSeconds);

    /**
     * @brief Signal emitted when retrying after a timeout or network error
     * @param attempt Current retry attempt (1-based)
     * @param maxAttempts Maximum retry attempts
     * @param error The error that triggered the retry
     */
    void retrying(int attempt, int maxAttempts, const QString &error);

    /**
     * @brief Signal emitted to report token usage statistics
     * @param inputTokens Estimated input (prompt) tokens
     * @param outputTokens Estimated output (completion) tokens
     */
    void tokenUsage(qint64 inputTokens, qint64 outputTokens);

private:
    QLLMService      *llmService   = nullptr;
    QSocToolRegistry *toolRegistry = nullptr;
    QSocAgentConfig   agentConfig;
    json              messages;

    /* Streaming state */
    bool    isStreaming     = false;
    int     streamIteration = 0;
    QString streamFinalContent;

    /* Timing state */
    QTimer       *heartbeatTimer = nullptr;
    QElapsedTimer runElapsedTimer;

    /* Request queue for dynamic input during execution */
    QStringList       requestQueue;
    mutable QMutex    queueMutex;
    std::atomic<bool> abortRequested{false};

    /* Progress tracking for stuck detection */
    std::atomic<qint64> lastProgressTime{0};

    /* Token tracking */
    std::atomic<qint64> totalInputTokens{0};
    std::atomic<qint64> totalOutputTokens{0};

    /* Retry tracking */
    int currentRetryCount = 0;

    /**
     * @brief Process a single iteration of the agent loop
     * @return true if the agent completed (no more tool calls), false otherwise
     */
    bool processIteration();

    /**
     * @brief Handle tool calls from the LLM response
     * @param toolCalls JSON array of tool calls
     */
    void handleToolCalls(const json &toolCalls);

    /**
     * @brief Add a message to the conversation history
     * @param role The role (user, assistant, system)
     * @param content The message content
     */
    void addMessage(const QString &role, const QString &content);

    /**
     * @brief Add a tool result message to the conversation history
     * @param toolCallId The ID of the tool call
     * @param content The tool result content
     */
    void addToolMessage(const QString &toolCallId, const QString &content);

    /**
     * @brief Compress history if needed based on token count
     */
    void compressHistoryIfNeeded();

    /**
     * @brief Process streaming iteration
     * @details Initiates one streaming request to LLM, handles response via signals
     */
    void processStreamIteration();

    /**
     * @brief Handle streaming complete response
     * @param response Complete response from LLM
     */
    void handleStreamComplete(const json &response);

    /**
     * @brief Handle streaming chunk from LLM
     * @param chunk Content chunk
     */
    void handleStreamChunk(const QString &chunk);

    /**
     * @brief Handle streaming error from LLM
     * @param error Error message
     */
    void handleStreamError(const QString &error);

    /**
     * @brief Estimate the number of tokens in a text
     * @param text The text to estimate
     * @return Estimated token count (approximately 4 characters per token)
     */
    int estimateTokens(const QString &text) const;

    /**
     * @brief Estimate the total tokens in the message history
     * @return Estimated token count for all messages
     */
    int estimateMessagesTokens() const;
};

#endif // QSOCAGENT_H
