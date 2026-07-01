// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENT_H
#define QSOCAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsoccontextrestore.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocmemoryrecall.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"

class QLongTaskMonitor;
class QSocHookManager;
class QSocLoopScheduler;

#include <atomic>
#include <functional>
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
 * @brief Verdict from the plan-mode shell safety judge
 * @details readOnly is true when the command only observes state; reason
 *          carries a short human explanation surfaced to the model when a
 *          command is blocked.
 */
struct QSocBashSafety
{
    bool    readOnly = false;
    QString reason;
};

/* Judges whether a shell command is read-only for plan mode. Injected so
 * production wires it to an isolated LLM classifier call while tests pass
 * a deterministic stub. Unset = fail-closed (treat every command as
 * mutating). */
using QSocBashSafetyJudge = std::function<QSocBashSafety(const QString &command)>;

/* Reports whether the user is actively watching the terminal (terminal
 * focus, DECSET 1004). Injected so the agent can steer away from
 * blocking ask_user prompts when nobody is looking. Empty / returns true
 * = assume watching (no steering). */
using QSocUserWatchingProbe = std::function<bool()>;

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
     * @brief Set the approved plan carried into the execution phase
     * @details Single-slot string, re-injected as a system message each
     *          turn (never persisted into the history), so a new plan
     *          overwrites the old and it survives pruning/compaction for
     *          free. Empty clears the plan. The caller budget-caps the
     *          text and keeps the full copy on disk.
     * @param plan Approved plan text (already budget-capped by the caller)
     */
    void setApprovedPlan(const QString &plan);

    /**
     * @brief Get the active approved plan (empty when none).
     */
    QString approvedPlan() const { return approvedPlan_; }

    /**
     * @brief Install the plan-mode shell safety judge
     * @details Called at dispatch for bash / remote_shell_bash while in
     *          plan mode. Unset means fail-closed (all shell blocked).
     */
    void setBashSafetyJudge(QSocBashSafetyJudge judge) { bashSafetyJudge_ = std::move(judge); }

    /**
     * @brief Read the installed shell safety judge (may be empty).
     */
    const QSocBashSafetyJudge &bashSafetyJudge() const { return bashSafetyJudge_; }

    /**
     * @brief Install the terminal-focus probe (user-watching signal).
     * @details Consulted each turn; when it returns false the agent is
     *          steered away from non-critical ask_user prompts. Unset =
     *          assume the user is watching.
     */
    void setUserWatchingProbe(QSocUserWatchingProbe probe)
    {
        userWatchingProbe_ = std::move(probe);
    }

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
     * @brief Inject the project goal catalog so the run loop appends
     *        an auto-continuation prompt after each turn when a goal
     *        is Active and the request queue is empty.
     */
    void setGoalCatalog(class QSocGoalCatalog *catalog) { goalCatalog = catalog; }

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
     * @brief Install the lazy producer of the post-compaction restore.
     * @details The CLI owns the read state, skills, and task source, so it
     *          supplies a closure that builds the payload from current
     *          state. The agent invokes it inside compactWithLLM (only when
     *          a compaction actually fires, so no per-turn file I/O) and
     *          appends the resulting messages after the summary. Covers
     *          every compaction path: manual, auto, and overflow.
     */
    void setContextRestoreProvider(std::function<QSocContextRestore()> provider);

    /**
     * @brief The restore applied by the most recent compaction (for the
     *        synchronous /compact render path). Empty when none applied.
     */
    QSocContextRestore takeLastContextRestore();

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
     * @brief Append an ephemeral <system-reminder> block to a wire payload
     *        as trailing user-turn content. Folds into the trailing user or
     *        tool message to avoid consecutive user turns that strict chat
     *        templates reject; never emits a role:"system" message. Static
     *        and pure so the wire contract can be unit-tested directly.
     */
    static void appendTurnReminder(nlohmann::json &wire, const QString &content);

    /**
     * @brief Why a tool name is rejected under the current agent config.
     * @details Single source of truth for the tool gates: sub-agent
     *          recursion guard, plan-mode tool visibility, allowlist,
     *          denylist, and the plan-mode read-only gate. Returns a
     *          human/LLM-readable reason, or an empty string when the
     *          tool is allowed.
     */
    QString toolDenyReason(const QString &name) const;

    /**
     * @brief Test whether a tool name is permitted under the current
     *        agent config. Equivalent to toolDenyReason(name).isEmpty().
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

    /**
     * @brief How a stream error should be retried.
     * @details None: not retryable (surface to the user). Transient:
     *          timeout / network / connection blip. RateLimited: the
     *          provider pushed back (HTTP 429 / 503 / 529 / "overloaded")
     *          and we should wait longer before trying again.
     */
    enum class RetryKind : quint8 { None, Transient, RateLimited };

    /**
     * @brief Classify a stream error string into a retry category.
     * @details Dispatches on the `[HTTP <code>] ` prefix that
     *          QLLMService prepends, plus a few free-text fallbacks.
     *          Context-overflow (413 / 400-with-context-phrase) is NOT
     *          classified here; that is handled separately upstream so
     *          it routes to compaction, not a dumb retry. Static + pure
     *          for unit testing.
     */
    static RetryKind classifyRetry(const QString &error);

    /**
     * @brief Backoff delay in milliseconds before retry attempt @p
     *        attempt (1-based). Exponential (base * 2^(attempt-1),
     *        capped) plus jitter to de-synchronize many sub-agents that
     *        hit the same rate limit at once. Rate-limited errors use a
     *        larger base than transient ones.
     */
    static int backoffDelayMs(int attempt, bool rateLimit);

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
     * @brief Signal emitted after a compaction re-injects context (auto or
     *        overflow paths), so the CLI can render the restore lines. The
     *        payload is read via takeLastContextRestore(); the signal is a
     *        bare trigger so it stays queued-connection safe.
     */
    void contextRestored();

    /**
     * @brief Signal emitted for each reasoning chunk during streaming
     * @param chunk The reasoning content chunk
     */
    void reasoningChunk(const QString &chunk);

private:
    /* Approved plan carried into the execution phase (plan mode). Single
     * slot: pinned into the message history exactly once. */
    QString approvedPlan_;

    /* Selective memory recall (Phase 1). recallBlock_ holds the reminder
     * text computed once per user turn and appended to the wire payload
     * each iteration; never persisted to `messages`, so the system-prompt
     * prefix stays byte-stable and the provider cache survives.
     * recallLlm_ is a lazily-cloned service pinned to the recall model
     * when one is configured; nullptr means use the primary llmService. */
    QString      recallBlock_;
    QLLMService *recallLlm_ = nullptr;
    /* Plan-mode shell safety judge (empty = fail-closed). */
    QSocBashSafetyJudge bashSafetyJudge_;
    /* Terminal-focus probe (empty = assume the user is watching). */
    QSocUserWatchingProbe userWatchingProbe_;

    QLLMService           *llmService    = nullptr;
    QSocToolRegistry      *toolRegistry  = nullptr;
    QSocMemoryManager     *memoryManager = nullptr;
    QSocHookManager       *hookManager   = nullptr;
    QSocLoopScheduler     *loopScheduler = nullptr;
    class QSocHostCatalog *hostCatalog   = nullptr;
    class QSocGoalCatalog *goalCatalog   = nullptr;
    /* Re-entry guard for the goal-continuation hook. Atomic so the
     * sync run() and async runStream() paths cannot race. Mirrors
     * codex's continuation_lock semaphore. */
    std::atomic<bool> goalContinuationInFlight{false};

    /* Per-stream-iteration accounting state. Snapshot at the start
     * of each runStream() so each iteration's token + elapsed delta
     * can be charged against the active goal's budget. */
    int             streamPrevTokensEstimate = 0;
    QElapsedTimer   streamIterationTimer;
    QSocAgentConfig agentConfig;
    json            messages;

    /* Post-compaction context restore. contextRestoreProvider_ is a lazy
     * builder installed by the CLI and invoked inside compactWithLLM;
     * lastApplied_ keeps the produced payload for the synchronous /compact
     * render path. */
    std::function<QSocContextRestore()> contextRestoreProvider_;
    QSocContextRestore                  lastApplied_;

    /* Streaming state */
    bool    isStreaming     = false;
    int     streamIteration = 0;
    QString streamFinalContent;
    /* Monotonic run id. Bumped at every runStream() so a deferred
     * backoff retry scheduled in run N is dropped if it fires after the
     * run was aborted and a new run N+1 started (no double-driving the
     * stream loop across turns). */
    quint64 streamEpoch_ = 0;

    /* Timing state */
    QTimer       *heartbeatTimer = nullptr;
    QElapsedTimer runElapsedTimer;

    /* Per-iteration stall + wall-clock watchdog. Reborn at every
     * processStreamIteration() so each LLM round trip is measured
     * independently. nullptr between iterations or when no run is in
     * flight. */
    QLongTaskMonitor *streamMonitor = nullptr;

    struct QueuedRequest
    {
        QString text;
        bool    taskNotification = false;
    };

    /* Request queue for dynamic input during execution */
    QList<QueuedRequest> requestQueue;
    mutable QMutex       queueMutex;
    std::atomic<bool>    abortRequested{false};

    /* Token tracking */
    std::atomic<qint64> totalInputTokens{0};
    std::atomic<qint64> totalOutputTokens{0};

    /* Retry tracking */
    int currentRetryCount         = 0;
    int contextOverflowRetryCount = 0;
    int emptyResponseRetryCount   = 0;

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
     * @brief Append the per-turn ephemeral reminders (critical reminder,
     *        plan mode, focus, approved plan, memory recall) to the wire
     *        payload as trailing <system-reminder> user-turn content.
     *        Keeps the cached system prefix (messages[0]) byte-stable and
     *        never emits a role:"system" message after the history, which
     *        strict chat templates reject. Shared by the streaming and
     *        synchronous iteration paths.
     */
    void injectPerTurnReminders(nlohmann::json &wire) const;

    /**
     * @brief Charge the active goal's usage counters with the token
     *        delta since the previous call and the wall-clock seconds
     *        spent on this iteration. No-op when there is no catalog
     *        or no active goal. Updates @p prevTokensEstimate in place
     *        and restarts @p iterationTimer.
     */
    void accountGoalUsageForIteration(int &prevTokensEstimate, class QElapsedTimer &iterationTimer);

    /**
     * @brief When the run loop ends a turn with no pending user input
     *        and the active goal is still Active, append a
     *        continuation (or budget-limit) prompt as a user message
     *        and return true so the caller can loop again. Returns
     *        false otherwise. Atomic re-entry guard prevents two
     *        continuations from racing.
     */
    bool maybeQueueGoalContinuation();

    /**
     * @brief When a plan-mode turn is about to end with prose instead
     *        of exit_plan_mode / ask_user, append a one-shot protocol
     *        reminder as a user message and return true so the caller
     *        loops again. Fires at most once per run and never for
     *        sub-agents (plan tools are gated off for them).
     */
    bool maybeQueuePlanModeNudge();

    /* Whether the plan-mode nudge already fired in the current run. */
    bool planNudgeUsed = false;

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
     * @brief Rank topic-file headers against the turn query and fill
     *        recallBlock_ with the relevant memories. Synchronous (runs
     *        at turn start, before the stream begins). No-op when recall
     *        is disabled, this is a sub-agent, the query is too short, or
     *        no memory exists. Uses a cheap selector model when configured,
     *        and a fast path that skips the selector when candidates are
     *        few enough to inject wholesale.
     */
    void computeRecallForTurn(const QString &query);

    /**
     * @brief Process streaming iteration
     * @details Initiates one streaming request to LLM, handles response via signals
     */
    void processStreamIteration();

    /**
     * @brief Construct, wire, and start a fresh per-iteration watchdog.
     */
    void armStreamMonitor();

    /**
     * @brief Stop and delete the current per-iteration watchdog.
     * @param cancelReason non-empty marks the teardown as a cancel
     *                    (emits the monitor's cancelled signal); empty
     *                    marks it as a normal finish (silent).
     */
    void teardownStreamMonitor(const QString &cancelReason);

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
