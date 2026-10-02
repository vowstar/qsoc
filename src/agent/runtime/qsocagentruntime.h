// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentruntime.h
 * @brief Public API of the qsoc agent runtime library.
 * @details Everything a frontend needs to host a full agent session:
 *          infrastructure assembly (managers, tools, MCP, LSP, scheduler),
 *          session persistence, turn execution, and a structured event
 *          stream. Frontends (TUI, daemon, tests) supply presentation and
 *          user interaction only; they never touch the subsystem wiring.
 */

#ifndef QSOCAGENTRUNTIME_H
#define QSOCAGENTRUNTIME_H

#include "agent/qsocagentconfig.h"
#include "agent/runtime/qsocagentruntimeevent.h"

#include <nlohmann/json.hpp>

#include <QDateTime>
#include <QDeadlineTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QSocAgent;
class QSocAgentDefinitionRegistry;
class QSocBusManager;
class QSocConfig;
class QSocFileHistory;
class QSocGoalCatalog;
class QSocHostCatalog;
class QLLMService;
class QSocLoopScheduler;
class QSocMemoryManager;
class QSocMcpManager;
class QSocModuleManager;
class QSocGenerateManager;
class QSocProjectManager;
class QSocSession;
class QSocSubAgentTaskSource;
class QSocTaskEventQueue;
class QSocTaskRegistry;
class QSocToolRegistry;
class QSocPathContext;
class QSocMonitorTaskSource;
class QSocHookManager;
class QSocRemoteConnection;
class QSocTaskForecast;
struct QSocAskUserOption;
struct QSocAskUserResult;
struct QSocPlanApproval;
struct QSocContextRestore;

/**
 * @brief Options that select and tune one runtime instance.
 * @details Mirrors the knobs the CLI exposed on `qsoc agent`, minus the
 *          presentation-only ones. Config-file values are still read by
 *          the runtime; these are the caller's overrides.
 */
struct QSocAgentRuntimeOptions
{
    /** Project directory (-d). Empty = auto-detect / cwd. */
    QString projectDirectory;
    /** Client launch context used to generate shell resume hints. */
    QString launchDirectory;
    QString clientProgram = QStringLiteral("qsoc");
    /** Project name (-p). Empty = first available. */
    QString projectName;
    /** Working directory for tool execution (--workspace). Absolute. */
    QString workspace;
    /** SSH target ([user@]host[:port] or ssh-config alias). */
    QString sshTarget;
    /** Defer SSH until frontend interaction handlers are installed. */
    bool deferRemoteConnection = false;
    /** Session id (or unique prefix) to resume; "-" = let the frontend pick. */
    QString resumeSessionId;
    /** Continue the most recent session. */
    bool continueLatestSession = false;
    /** Override max context tokens; 0 = model registry / config default. */
    int maxContextTokens = 0;
    /** Override temperature; negative = config default. */
    double temperature = -1.0;
    /** Override reasoning effort; empty = default, "off"/"low"/"medium"/"high". */
    QString effortLevel;
    /** Override tool presentation: direct | catalog | auto. */
    QString toolPresentation;
    /** Streaming mode (default true). */
    bool streaming = true;
    /** Resolve streaming from session configuration unless explicitly overridden. */
    bool streamingFromConfig = false;
    /** Verbose logging to QSocConsole::debug(). */
    bool verbose = false;
};

/**
 * @brief One row of the resume picker / session listing.
 */
struct QSocAgentSessionInfo
{
    QString   id;
    QString   path;
    QDateTime createdAt;
    QDateTime lastModified;
    QString   firstPrompt;
    QString   title;
    QString   branch;
    int       messageCount = 0;
};

/**
 * @brief Result of a completed turn.
 */
struct QSocAgentTurnResult
{
    QString finalText;  /**< Assistant text (streaming already delivered). */
    QString stopNotice; /**< Why a non-complete stop happened; empty on success. */
    bool    error       = false;
    bool    aborted     = false;
    bool    persistedOk = true; /**< Session persistence for this turn. */
    QString errorText;
};

/**
 * @brief Assembles and drives one agent session.
 * @details The runtime owns (or borrows, per the shared-manager
 *          constructor) every piece of agent infrastructure:
 *
 *          - managers: project, config, LLM, bus, module, generate, memory,
 *            hooks, goals, hosts
 *          - the tool registry with every built-in tool registered
 *          - MCP servers from the merged config
 *          - the LSP service
 *          - the loop scheduler, task registry, task sources, task forecast
 *          - session persistence (JSONL + file history + artifacts store)
 *          - crash recovery planning
 *          - background memory extraction / dream / session titles
 *
 *          Frontends observe via @ref eventRaised and answer questions via
 *          the callbacks installed with @ref setAskUserHandler,
 *          @ref setPlanApprovalHandler and @ref setSecretHandler. The
 *          runtime never touches a terminal.
 */
class QSocAgentRuntime : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Create a runtime that owns all of its infrastructure.
     * @details Reads the merged config layers, loads the project, registers
     *          every built-in tool, starts MCP and LSP. This is the path the
     *          daemon and the CLI use.
     * @param options Caller overrides (CLI flags, daemon request).
     * @param parent QObject parent.
     */
    explicit QSocAgentRuntime(const QSocAgentRuntimeOptions &options, QObject *parent = nullptr);

    /**
     * @brief Create a runtime over externally owned managers.
     * @details The embedding application (GUI, tests) may already hold the
     *          project / config / LLM stack. The runtime wires the agent
     *          layer on top without re-creating them. Pointers must outlive
     *          the runtime; null ones are created internally.
     */
    explicit QSocAgentRuntime(
        const QSocAgentRuntimeOptions &options,
        QSocProjectManager            *projectManager,
        QSocConfig                    *socConfig,
        QLLMService                   *llmService,
        QSocBusManager                *busManager,
        QSocModuleManager             *moduleManager,
        QSocGenerateManager           *generateManager,
        QObject                       *parent = nullptr);

    ~QSocAgentRuntime() override;

    QSocAgentRuntime(const QSocAgentRuntime &)                = delete;
    QSocAgentRuntime &operator=(const QSocAgentRuntime &)     = delete;
    QSocAgentRuntime(QSocAgentRuntime &&)                     = delete;
    QSocAgentRuntime &operator=(QSocAgentRuntime &&) noexcept = delete;

    /* ------------------------------------------------------------------ */
    /* Access to the assembled infrastructure (read-mostly). */

    /** The agent itself. */
    [[nodiscard]] QSocAgent *agent() const;
    /** Tool registry currently in effect (local or remote). */
    [[nodiscard]] QSocToolRegistry *toolRegistry() const;
    /** LLM service. */
    [[nodiscard]] QLLMService *llmService() const;
    /** Project manager. */
    [[nodiscard]] QSocProjectManager *projectManager() const;
    /** Merged configuration. */
    [[nodiscard]] QSocConfig *config() const;
    /** Bus manager. */
    [[nodiscard]] QSocBusManager *busManager() const;
    /** Module manager. */
    [[nodiscard]] QSocModuleManager *moduleManager() const;
    /** Generate manager. */
    [[nodiscard]] QSocGenerateManager *generateManager() const;
    /** Memory manager. */
    [[nodiscard]] QSocMemoryManager *memoryManager() const;
    /** Loop scheduler shared by /loop and schedule_* tools. */
    [[nodiscard]] QSocLoopScheduler *loopScheduler() const;
    /** Background task registry (loops, bash, sub-agents, monitors). */
    [[nodiscard]] QSocTaskRegistry *taskRegistry() const;
    /** Sub-agent task source. */
    [[nodiscard]] QSocSubAgentTaskSource *subAgentSource() const;
    /** Host catalog (named SSH targets). */
    [[nodiscard]] QSocHostCatalog *hostCatalog() const;
    /** Goal catalog. */
    [[nodiscard]] QSocGoalCatalog *goalCatalog() const;
    /** MCP manager, when servers were configured. */
    [[nodiscard]] QSocMcpManager *mcpManager() const;
    /** Local path context for file tools. */
    [[nodiscard]] QSocPathContext *pathContext() const;
    /** Monitor task source (background monitors). */
    [[nodiscard]] QSocMonitorTaskSource *monitorTaskSource() const;
    /** Task notification queue feeding the agent and the frontend. */
    [[nodiscard]] QSocTaskEventQueue *taskEventQueue() const;
    /** The remote connection binding (empty when local). */
    [[nodiscard]] QSocRemoteConnection *remoteConnection() const;
    /** Remote tool registry, when a remote workspace is bound. */
    [[nodiscard]] QSocToolRegistry *remoteToolRegistry() const;
    /** Local tool registry (always present). */
    [[nodiscard]] QSocToolRegistry *localToolRegistry() const;
    /** The resume session id requested at construction ("-" = pick). */
    [[nodiscard]] QString resumeSessionId() const;

    /* ------------------------------------------------------------------ */
    /* Session lifecycle. */

    /**
     * @brief Resolve the startup session per the options.
     * @details Applies resume / continue semantics: a concrete id is
     *          resolved as a unique prefix, "-" defers to the frontend
     *          (see @ref listSessions), and otherwise a fresh session is
     *          created. Emits @ref eventRaised with SessionResumed /
     *          SessionStarted / recovery notices as appropriate.
     * @retval true Session is ready for turns.
     * @retval false Session could not be established (see lastError()).
     */
    bool openSession();

    /**
     * @brief Open a specific session by id (used after the frontend picked
     *        one from @ref listSessions).
     */
    bool openSessionById(const QString &sessionId);

    /**
     * @brief Sessions available for resume, newest first.
     */
    [[nodiscard]] QList<QSocAgentSessionInfo> listSessions() const;

    /** Current session id, or empty before openSession(). */
    [[nodiscard]] QString sessionId() const;
    /** Shell command to resume the saved session; empty until persisted. */
    [[nodiscard]] QString resumeCommand() const;
    /** Raw session object (for transcript rendering and metadata reads). */
    [[nodiscard]] QSocSession *session() const;
    /** Persisted-message mirror the runtime maintains. */
    [[nodiscard]] nlohmann::json persistedMessages() const;
    /** How many messages have been persisted. */
    [[nodiscard]] int lastPersistedIndex() const;
    /** Memory-extraction cursor. */
    [[nodiscard]] int lastMemoryIndex() const;
    /** Monotonic turn counter for file-history snapshots. */
    [[nodiscard]] int turnCounter() const;
    /** Current session file path, or empty. */
    [[nodiscard]] QString sessionPath() const;
    /** File history bound to the session (for /diff and rewind). */
    [[nodiscard]] QSocFileHistory *fileHistory() const;

    /**
     * @brief Start a fresh session (the /clear action).
     * @details Refuses while a sub-agent is unsettled or storage is unsafe.
     * @retval true A new empty session is active.
     */
    bool clearSession();

    /**
     * @brief Persist any outstanding message delta now.
     */
    bool persistNow();

    /* ------------------------------------------------------------------ */
    /* Turn execution. */

    /**
     * @brief Submit one user turn and run it to completion.
     * @details Streams progress through @ref eventRaised; returns the
     *          terminal state. Re-entrant input arriving while a turn runs
     *          is queued on the agent exactly like the REPL did.
     * @param input Fully expanded user input (paste chips resolved).
     */
    QSocAgentTurnResult runTurn(const QString &input);

    /**
     * @brief Idle work after a turn settles.
     * @details Background memory extraction, once-per-process dream,
     *          auto session title, away recap and the near-capacity
     *          notice. Emits Output / Status events for anything the
     *          user should see. The frontend calls this after runTurn()
     *          when the user is not mid-typing.
     */
    void finishTurnMaintenance();

    /**
     * @brief Record that the user invoked a skill by name.
     * @details Skill dispatch happens in the frontend (it composes the
     *          prompt); the runtime needs the name for the post-compaction
     *          context restore. Bare name, no '/'.
     */
    void noteInvokedSkill(const QString &name);

    /**
     * @brief Drain inputs the runtime queued for the next prompt.
     * @details Scheduler fires, task notifications that arrived while
     *          idle, and plan-approval continuations land here. The
     *          frontend drains at the top of its prompt loop and
     *          submits each as if the user typed it.
     */
    QStringList takePendingAutoInputs();

    /** Whether any pending auto-input is waiting. */
    [[nodiscard]] bool hasPendingAutoInputs() const;

    /** Whether a turn is currently executing. */
    [[nodiscard]] bool isRunning() const;

    /** Abort the active turn (soft stop). */
    void abort();

    /** Queue input while a turn runs; false when the run is stopping. */
    bool                queueRequest(const QString &text);
    bool                hasPendingRecovery() const;
    QSocAgentTurnResult recoverPendingTurn();

    /** Compact the history now (the /compact action). */
    int compactNow();

    /** Last compaction outcome for reporting. */
    enum class CompactionStatus : quint8 { Committed, NoProgress, Cancelled, Failed };
    CompactionStatus lastCompactionStatus() const;

    /** Estimated total context tokens. */
    [[nodiscard]] int estimateTotalTokens() const;
    /** Effective context budget. */
    [[nodiscard]] int effectiveContextTokens() const;
    /** Conversation history snapshot. */
    [[nodiscard]] nlohmann::json messages() const;
    /** Replace the conversation history (rewind support). */
    void setMessages(const nlohmann::json &msgs);
    /** Clear conversation + pending queue (part of /clear). */
    void clearHistory();

    /* ------------------------------------------------------------------ */
    /* Slash-command execution on the runtime side. */

    /**
     * @brief Whether the runtime handles this input itself.
     * @details True for the built-in slash commands and `!`/`#` prefixes
     *          that need infrastructure access. Frontends call this before
     *          runTurn() and route the input here when it returns true.
     */
    [[nodiscard]] static bool handlesCommand(const QString &input);

    /**
     * @brief Execute one runtime-owned command.
     * @details The command's user-visible output is delivered as
     *          Output events on @ref eventRaised (the frontend prints
     *          them); interactive steps (menus, editors, path pickers)
     *          are delegated to the installed handlers.
     * @param input The raw input line ("/compact", "!make", "#fact", ...).
     * @retval true The command was consumed.
     * @retval false The command was not recognised; treat as user text.
     */
    bool        executeCommand(const QString &input);
    QStringList availableCommands() const;

    /* ------------------------------------------------------------------ */
    /* Frontend interaction callbacks. */

    /**
     * @brief Handler for the ask_user tool.
     * @details Return a result synchronously; an empty result with
     *          canceled=true declines. Unset means ask_user is declined.
     */
    using AskUserHandler = std::function<QSocAskUserResult(
        const QString &question, const QString &header, const QList<QSocAskUserOption> &options)>;
    void setAskUserHandler(AskUserHandler handler);

    /**
     * @brief Handler for plan approval (exit_plan_mode + stall recovery).
     */
    using PlanApprovalHandler = std::function<QSocPlanApproval(const QString &plan)>;
    void setPlanApprovalHandler(PlanApprovalHandler handler);

    /**
     * @brief Handler for hidden secrets (SSH passphrases / passwords).
     * @details Called synchronously from a connect sequence.
     */
    using SecretHandler = std::function<QString(const QString &prompt)>;
    void setSecretHandler(SecretHandler handler);

    /**
     * @brief Handler for free-form text editing ($EDITOR flows: /memory).
     * @param current Text to edit.
     * @param[out] result Edited text on success.
     * @param[out] error Failure text on error.
     * @retval true Edit committed.
     */
    using TextEditHandler
        = std::function<bool(const QString &current, QString *result, QString *error)>;
    void setTextEditHandler(TextEditHandler handler);

    /**
     * @brief Handler for directory picking (/cwd, remote workspace).
     * @param title Dialog title.
     * @param startPath Directory to start browsing from.
     * @param homePath Home directory shortcut.
     * @param listDirs Callback the picker uses to list subdirectories.
     * @param listError Callback returning the last listing error.
     * @return Chosen directory, or empty when cancelled.
     */
    using ListDirsFn  = std::function<QStringList(const QString &path)>;
    using ListErrorFn = std::function<QString()>;
    using DirPicker   = std::function<QString(
        const QString     &title,
        const QString     &startPath,
        const QString     &homePath,
        const ListDirsFn  &listDirs,
        const ListErrorFn &listError)>;
    void setDirectoryPicker(DirPicker picker);

    /**
     * @brief Handler for one-choice menus (model / effort pickers).
     * @param title Menu title.
     * @param items Labels, one per row.
     * @param hints Optional per-row hint text.
     * @param marked Which rows are pre-selected (current values).
     * @return Index into items, or -1 when cancelled.
     */
    using MenuHandler = std::function<
        int(const QString     &title,
            const QStringList &items,
            const QStringList &hints,
            const QList<bool> &marked)>;
    void setMenuHandler(MenuHandler handler);

    /**
     * @brief Whether the user is watching right now (focus probe).
     * @details Drives ask_user avoidance and away summaries.
     */
    void setUserWatchingProbe(std::function<bool()> probe);

    /**
     * @brief Handler for remote connect (the /ssh action).
     * @details Runs the whole connect + workspace selection flow, using the
     *          secret and directory-picker handlers for interaction.
     * @param target Raw target string.
     * @param[out] error UI-safe failure text.
     * @retval true Connected; the runtime swapped to remote tools.
     */
    bool connectRemote(const QString &target, QString *error = nullptr);

    /**
     * @brief Drop the remote binding and return to local tools (/local).
     */
    void disconnectRemote();

    /** Whether a remote workspace is currently bound. */
    [[nodiscard]] bool isRemote() const;
    /** Remote target label ("user@host"), empty when local. */
    [[nodiscard]] QString remoteTarget() const;
    /** Remote workspace path, empty when local. */
    [[nodiscard]] QString remoteWorkspace() const;

    /**
     * @brief Change the working directory (the /cwd action).
     * @param requested Directory (absolute, or relative to the working dir).
     * @param[out] error Failure text.
     * @retval true Working directory changed.
     */
    bool setWorkingDirectory(const QString &requested, QString *error = nullptr);

    /** Current working directory (local path context or remote cwd). */
    [[nodiscard]] QString workingDirectory() const;

    /**
     * @brief Switch the active project (the /project action).
     * @details Persists the current session, resets managers, opens a fresh
     *          session in the new project.
     */
    bool switchProject(const QString &directory, QString *error = nullptr);

    /* ------------------------------------------------------------------ */
    /* Model / effort controls. */

    /** Available model ids. */
    [[nodiscard]] QStringList availableModels() const;
    /** Current model id (empty = default). */
    [[nodiscard]] QString currentModelId() const;
    /**
     * @brief Switch the active model.
     * @details Updates the agent context budget and effort, and persists
     *          the choice to the effective config file.
     */
    bool setCurrentModel(const QString &modelId);

    /** Current reasoning effort ("off" when empty). */
    [[nodiscard]] QString effortLevel() const;
    /** Set reasoning effort; empty string means off. */
    void setEffortLevel(const QString &level);

    /** Toggle plan mode. */
    void               setPlanMode(bool enabled);
    [[nodiscard]] bool planMode() const;

    /* ------------------------------------------------------------------ */
    /* Status snapshot for status bars / protocol replies. */

    struct UsageSnapshot
    {
        qint64 inputTokens      = 0;
        qint64 outputTokens     = 0;
        int    usedTokens       = 0;
        int    maxTokens        = 0;
        double compactThreshold = 0.0;
        bool   approximate      = false; /**< usedTokens is an estimate range point. */
    };
    [[nodiscard]] UsageSnapshot usage() const;

    /** Status-line payload (same JSON the CLI status line consumed). */
    [[nodiscard]] nlohmann::json statusLinePayload() const;

    /* ------------------------------------------------------------------ */
    /* Errors. */

    /** Last fatal error text (openSession / connect failures). */
    [[nodiscard]] QString lastError() const;

signals:

    /**
     * @brief The single structured event stream.
     * @details Frontends translate these into widgets / protocol frames.
     *          See @ref QSocAgentRuntimeEvent for the payload catalogue.
     */
    void eventRaised(const QSocAgentRuntimeEvent &event);

    /** Convenience signal: the session id changed. */
    void sessionChanged(const QString &sessionId);

    /** Convenience signal: model / effort / plan / remote chips changed. */
    void statusChanged();

protected:
    /** Re-emitted from the agent; runtime subclasses may filter. */
    void emitEvent(const QSocAgentRuntimeEvent &event);

private:
    struct Private;
    std::unique_ptr<Private> d;

    void assembleInfrastructure(const QSocAgentRuntimeOptions &options);
    void registerTools();
    void startMcp();
    void startLsp();
    void wireAgentCallbacks();
    void wirePersistence();
    void wireSessionTools();
    void wireContextRestore();
    void wireAuxiliaryServices();
    void showHistoryDiff();
    void prepareRecovery();
    bool applyOptionsToConfig(const QSocAgentRuntimeOptions &options);
    void connectLocalWorkspace(const QString &workspacePath);
    bool openSessionInternal(const QString &sessionId, bool fresh);
    void emitOutput(const QString &text, int style = static_cast<int>(QSocAgentRuntimeStyle::Normal));
    void emitStatus(const QString &status);
    void fillContextUsage(QSocAgentRuntimeEvent &event) const;
    void maybeGenerateSessionTitle();
    void maybeGenerateAwaySummary();
};

#endif /* QSOCAGENTRUNTIME_H */
