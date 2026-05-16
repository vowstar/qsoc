// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENT_H
#define QSOCAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"

class QSocHookManager;
class QSocLoopScheduler;

#include <atomic>
#include <nlohmann/json.hpp>
#include <QElapsedTimer>
#include <QList>
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
     * @brief Queue a model-visible background task notification.
     * @details Unlike queueRequest(), this bypasses user_prompt_submit hooks
     *          when drained during a running turn because it is system
     *          generated, not human-authored input.
     */
    void queueTaskNotification(const QString &notification);

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
     * @brief Manually trigger context compaction
     * @return Number of tokens saved (0 if nothing compacted)
     */
    int compact();

    /**
     * @brief Find safe message boundary that doesn't split tool_calls/tool pairs
     * @param proposedIndex Desired boundary index
     * @return Adjusted index that keeps tool_calls groups intact
     */
    int findSafeBoundary(int proposedIndex) const;

    /**
     * @brief Format messages for summary prompt
     * @param start Start index (inclusive)
     * @param end End index (exclusive)
     * @return Formatted text for LLM summarization
     */
    QString formatMessagesForSummary(int start, int end) const;

    /**
     * @brief Set the LLM service
     * @param llmService Pointer to the LLM service
     */
    void setLLMService(QLLMService *llmService);

    /**
     * @brief Get the LLM service this agent is wired against.
     *        Used by the spawn-agent tool to clone() the parent's
     *        service for each child so concurrent sub-agents don't
     *        share the single-flight stream invariant.
     */
    QLLMService *getLLMService() const { return llmService; }

    /**
     * @brief Set the tool registry
     * @param toolRegistry Pointer to the tool registry
     */
    void setToolRegistry(QSocToolRegistry *toolRegistry);

    /**
     * @brief Get the current tool registry.
     * @return Pointer (may be nullptr if never set).
     */
    QSocToolRegistry *getToolRegistry() const { return toolRegistry; }

    /**
     * @brief Set the reasoning effort level
     * @param level Effort level: empty/off, "low", "medium", "high"
     */
    void setEffortLevel(const QString &level);

    /**
     * @brief Set the reasoning model
     * @param model Model name to use when effort is set (empty=use primary)
     */
    void setReasoningModel(const QString &model);

    /**
     * @brief Set the memory manager for persistent memory injection
     * @param manager Pointer to the memory manager
     */
    void setMemoryManager(QSocMemoryManager *manager);

    /**
     * @brief Get the current memory manager.
     * @return Pointer (may be nullptr if never set).
     */
    QSocMemoryManager *getMemoryManager() const { return memoryManager; }

    /**
     * @brief Inject the host catalog so the system prompt advertises
     *        available named SSH targets and their capabilities.
     */
    void setHostCatalog(class QSocHostCatalog *catalog) { hostCatalog = catalog; }

    /**
     * @brief Set the hook manager for lifecycle event dispatch.
     * @details The agent fires user-defined hook commands at well-known
     *          lifecycle points (tool dispatch, prompt admission, session
     *          start/end). When unset, no hooks fire and the agent
     *          behaves identically to earlier releases.
     */
    void setHookManager(QSocHookManager *manager);

    /**
     * @brief Set the loop scheduler shared by `/loop` and `schedule_*` tools.
     * @details The CLI agent owns the QSocLoopScheduler instance for the
     *          life of the REPL; passing it here lets schedule tools
     *          registered on this agent reach the same in-memory job list
     *          as the REPL's `/loop` slash dispatch.
     */
    void setLoopScheduler(QSocLoopScheduler *scheduler);

    /**
     * @brief Get the current loop scheduler.
     * @return Pointer (may be nullptr in non-CLI agents).
     */
    QSocLoopScheduler *getLoopScheduler() const { return loopScheduler; }

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

    /**
     * @brief Estimate the number of tokens in a text.
     * @return Estimated token count (approximately 4 characters per token).
     */
    int estimateTokens(const QString &text) const;

    /**
     * @brief Estimate total tokens for the next API call (system prompt +
     *        tool defs + all messages).
     */
    int estimateTotalTokens() const;

    /**
     * @brief Estimate tokens in the message history only.
     */
    int estimateMessagesTokens() const;

    /**
     * @brief Effective input-side context budget after subtracting the
     *        slice reserved for the assistant reply. Threshold checks
     *        should always run against this, never raw maxContextTokens.
     */
    int effectiveContextTokens() const;

    /**
     * @brief Build the full system prompt from modular sections + dynamic
     *        context (project instructions, skills, memory).
     */
    QString buildSystemPromptWithMemory() const;

    /**
     * @brief Test whether a tool name is permitted under the current
     *        agent config. Honors `agentConfig.toolsAllow` allowlist,
     *        and unconditionally rejects the spawn-agent tool when
     *        `agentConfig.isSubAgent` is true.
     */
    bool isToolAllowed(const QString &name) const;

    /**
     * @brief Tool definitions the LLM will actually see this turn:
     *        the registry's full list filtered by the sub-agent
     *        allowlist + recursion guard.
     */
    nlohmann::json getEffectiveToolDefinitions() const;

    /**
     * @brief Fold token usage from a child or other external source
     *        into this agent's running totals. Re-emits `tokenUsage`
     *        with the new totals so the status pill / cost view see
     *        the addition immediately.
     * @param inputTokens  Tokens to add to totalInputTokens.
     * @param outputTokens Tokens to add to totalOutputTokens.
     */
    void addExternalTokenUsage(qint64 inputTokens, qint64 outputTokens);

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

    /**
     * @brief Signal emitted when context compaction occurs
     * @param layer Compaction layer (1=prune, 2=LLM compact)
     * @param beforeTokens Token count before compaction
     * @param afterTokens Token count after compaction
     */
    void compacting(int layer, int beforeTokens, int afterTokens);

    /**
     * @brief Signal emitted for each reasoning chunk during streaming
     * @param chunk The reasoning content chunk
     */
    void reasoningChunk(const QString &chunk);

private:
    QLLMService           *llmService    = nullptr;
    QSocToolRegistry      *toolRegistry  = nullptr;
    QSocMemoryManager     *memoryManager = nullptr;
    QSocHookManager       *hookManager   = nullptr;
    QSocLoopScheduler     *loopScheduler = nullptr;
    class QSocHostCatalog *hostCatalog   = nullptr;
    QSocAgentConfig        agentConfig;
    json                   messages;

    /* Streaming state */
    bool    isStreaming     = false;
    int     streamIteration = 0;
    QString streamFinalContent;

    /* Timing state */
    QTimer       *heartbeatTimer = nullptr;
    QElapsedTimer runElapsedTimer;

    struct QueuedRequest
    {
        QString text;
        bool    taskNotification = false;
    };

    /* Request queue for dynamic input during execution */
    QList<QueuedRequest> requestQueue;
    mutable QMutex       queueMutex;
    std::atomic<bool>    abortRequested{false};

    /* Progress tracking for stuck detection */
    std::atomic<qint64> lastProgressTime{0};

    /* Token tracking */
    std::atomic<qint64> totalInputTokens{0};
    std::atomic<qint64> totalOutputTokens{0};

    /* Retry tracking */
    int currentRetryCount         = 0;
    int contextOverflowRetryCount = 0;

    /**
     * @brief Fire user_prompt_submit and react to its outcome.
     * @param[in,out] userQuery prompt about to be added to the
     *                          conversation; the hook may prepend
     *                          injected context.
     * @param[out] blockReason populated on block.
     * @return true to allow, false to drop the prompt.
     */
    bool firePromptSubmitHook(QString *userQuery, QString *blockReason);

    /**
     * @brief Build the common envelope (cwd, remote section) that every
     *        hook payload starts from. Per-event fields are added by the
     *        caller.
     */
    nlohmann::json buildHookEnvelope() const;

    /**
     * @brief Fire session_start once per agent lifetime. Fire-and-forget.
     */
    void fireSessionStartHookOnce();

    /**
     * @brief Fire stop just before runComplete is emitted. Fire-and-forget.
     */
    void fireStopHook(const QString &finalContent);

    /* Whether session_start has fired for this agent instance. */
    bool sessionStartFired = false;

    /**
     * @brief Drop tool definitions that are not allowed for the current
     *        agent (subagent allowlist, recursion guard).
     */
    nlohmann::json filterAllowedTools(const nlohmann::json &defs) const;

    /**
     * @brief Append the dynamic prompt sections (environment, remote
     *        workspace, project instructions, skills, MCP servers,
     *        memory) onto the given buffer. Shared by the regular
     *        and sub-agent prompt assembly paths.
     */
    void appendDynamicSystemSections(QString &prompt) const;

    /**
     * @brief Layer 1: Prune old tool outputs to reduce token usage
     * @param force Skip threshold check (for manual compact)
     * @return true if pruning saved enough tokens
     */
    bool pruneToolOutputs(bool force = false);

    /**
     * @brief Layer 2: Use LLM to generate a structured summary of old messages
     * @param force Skip threshold check (for manual compact)
     * @return true if compaction succeeded
     */
    bool compactWithLLM(bool force = false);

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

public:
    /**
     * @brief A single image attachment lifted from a tool result.
     * @details Tools that fetch images (web_fetch on an image URL) emit a
     *          structured marker the agent loop strips out and forwards
     *          here so the next LLM request can include the image as an
     *          OpenAI-compatible image_url content part.
     */
    struct AttachmentSpec
    {
        QString mime;      /* image/jpeg, image/png, image/webp */
        QString dataB64;   /* base64-encoded payload, no data: prefix */
        QString sourceUrl; /* original URL the bytes were fetched from */
        int     width     = 0;
        int     height    = 0;
        int     byteSize  = 0;
        int     estTokens = 0;
        bool    resized   = false;
    };

    /**
     * @brief Extract image attachments from a raw tool result.
     * @param raw  Tool result possibly containing attachment markers.
     * @param out  Populated with each parsed attachment in source order.
     * @return The text-only view of @p raw (markers removed), suitable
     *         for showing to the user and for storing as the textual
     *         portion of the tool message content.
     */
    static QString extractImageAttachments(const QString &raw, QList<AttachmentSpec> *out);

private:
    /**
     * @brief Add a tool result message to the conversation history
     * @param toolCallId  The ID of the tool call
     * @param content     Stripped tool result content (no markers)
     * @param attachments Image attachments to lift into a content array
     */
    void addToolMessage(
        const QString               &toolCallId,
        const QString               &content,
        const QList<AttachmentSpec> &attachments = {});

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
     * @brief Handle reasoning chunk from LLM
     * @param chunk Reasoning content chunk
     */
    void handleReasoningChunk(const QString &chunk);

    /* estimateMessagesTokens / estimateTotalTokens declared public above */

    /* buildSystemPromptWithMemory declared public above */

    /* Compaction failure tracking */
    int compactFailureCount = 0;
};

#endif // QSOCAGENT_H
