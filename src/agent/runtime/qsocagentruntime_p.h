// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentruntime_p.h
 * @brief Private implementation shared by the runtime translation units.
 * @details Not installed and not part of the public API.
 */

#ifndef QSOCAGENTRUNTIME_P_H
#define QSOCAGENTRUNTIME_P_H

#include "agent/runtime/qsocagentruntime.h"

#include "agent/qsocagentconfig.h"
#include "agent/qsocmemoryextractor.h"
#include "agent/qsocsession.h"
#include "agent/qsocsessionrecovery.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsshconfigparser.h"

#include <nlohmann/json.hpp>

#include <QDeadlineTimer>
#include <QHash>
#include <QLockFile>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <stop_token>

class QLspService;
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
class QSocSubAgentTaskSource;
class QSocTaskEventQueue;
class QSocTaskRegistry;
class QSocToolRegistry;
class QSocPathContext;
class QSocMonitorTaskSource;
class QSocHookManager;
class QSocBashTaskSource;

namespace QSocAgentRuntimeInternal {

using json = nlohmann::json;

/** Session-project resolution shared by every runtime TU. */
QString sessionProjectPath(QSocProjectManager *pmanager);

/** Fresh-session path guard. */
bool freshSessionPathAvailable(const QString &sessionPath);

/** Existing-session path guard. */
bool existingSessionPathIsRegular(const QString &sessionPath);

/** Lock a session file; empty on failure. */
std::unique_ptr<QLockFile> lockSession(const QString &sessionPath);

/** Append the message delta (or a snapshot when history shrank). */
bool persistSessionState(
    QSocAgent *agent, QSocSession *session, json &persistedMessages, int &lastPersistedIndex);

/** Persist a recovery snapshot (compaction / crash recovery). */
bool persistRecoverySnapshot(
    QSocSession *session, const json &messages, json &persistedMessages, int &lastPersistedIndex);

/**
 * @brief Run a local `!` line and return what it printed.
 * @details The local machine's `!` rule: its executor (`-c`) on POSIX,
 *          `cmd.exe` on Windows, which receives the line unchanged. Ends with
 *          the exit code when non-zero and with the shell that ran it.
 */
QString runLocalShellEscape(const QString &command, const QString &directory, std::stop_token stop);

} // namespace QSocAgentRuntimeInternal

using QSocAgentRuntimeInternal::json;

/**
 * @brief Private state of QSocAgentRuntime.
 */
struct QSocAgentRuntime::Private
{
    /* Options captured at construction. */
    QSocAgentRuntimeOptions options;

    /* Infrastructure (owned unless borrowed via the shared ctor). */
    bool                 ownsManagers    = false;
    QSocProjectManager  *projectManager  = nullptr;
    QSocConfig          *socConfig       = nullptr;
    QLLMService         *llmService      = nullptr;
    QSocBusManager      *busManager      = nullptr;
    QSocModuleManager   *moduleManager   = nullptr;
    QSocGenerateManager *generateManager = nullptr;

    QSocAgentConfig                      agentConfig;
    QSocAgent                           *agent              = nullptr;
    QSocToolRegistry                    *toolRegistry       = nullptr;
    QSocLoopScheduler                   *loopScheduler      = nullptr;
    QSocMemoryManager                   *memoryManager      = nullptr;
    QSocHookManager                     *hookManager        = nullptr;
    QSocHostCatalog                     *hostCatalog        = nullptr;
    QSocGoalCatalog                     *goalCatalog        = nullptr;
    QSocTaskRegistry                    *taskRegistry       = nullptr;
    QSocTaskEventQueue                  *taskEventQueue     = nullptr;
    QSocSubAgentTaskSource              *subAgentTaskSource = nullptr;
    QSocMonitorTaskSource               *monitorTaskSource  = nullptr;
    QSocBashTaskSource                  *bashTaskSource     = nullptr;
    QSocAgentDefinitionRegistry         *agentDefinitions   = nullptr;
    QSocMcpManager                      *mcpManager         = nullptr;
    QSocPathContext                     *pathContext        = nullptr;
    QLspService                         *lspService         = nullptr;
    std::unique_ptr<QSocSshConfigParser> sshConfig;

    /* Remote state. */
    QSocRemoteConnection  remoteConnStorage;
    QSocRemoteConnection *remoteConn     = &remoteConnStorage;
    QSocToolRegistry     *remoteRegistry = nullptr;
    QSocToolRegistry     *localRegistry  = nullptr;
    QString               hostBindingDir;
    QString               remoteAlias; /* The target as the user named it. */

    /* Session state. */
    std::unique_ptr<QSocSession>          currentSession;
    std::unique_ptr<QSocFileHistory>      currentFileHistory;
    std::unique_ptr<QLockFile>            sessionLock;
    QString                               sessionLockPath;
    json                                  persistedMessages  = json::array();
    int                                   lastPersistedIndex = 0;
    QSocMemoryExtractor::Cursor           memoryCursor;
    int                                   turnCounter         = 0;
    bool                                  historyInputBlocked = false;
    QString                               activeRunId;
    std::optional<QSocSession::RunRecord> recoveryRun;
    bool                                  resumeHistory         = false;
    bool                                  recoveryRequiresInput = false;

    /* Frontend handlers. */
    AskUserHandler        askUser;
    PlanApprovalHandler   planApproval;
    SecretHandler         secret;
    TextEditHandler       textEdit;
    DirPicker             dirPicker;
    MenuHandler           menu;
    std::function<bool()> userWatching;

    QPointer<QSocAgent> maintenanceChild;
    bool                cancelRequested = false;
    std::stop_source    commandStop;

    QString terminalStopNotice;

    /* Turn bookkeeping. */
    QSocSession::RunEvent observedTerminal          = QSocSession::RunEvent::Invalid;
    bool                  runTerminalPersisted      = false;
    bool                  planApprovalShownThisTurn = false;
    QString               lastErrorText;

    /* Background notifications and the idle wake gate. */
    struct Wake
    {
        static constexpr qint64 debounceMs  = 500;
        int                     consecutive = 0;     /* turns the user did not start */
        bool                    latched     = false; /* last run aborted or failed */
        qint64                  dueAtMs     = 0;     /* 0 = nothing armed */
        TurnOrigin              origin      = TurnOrigin::User;
        std::function<qint64()> clock       = [] { return QDeadlineTimer::current().deadline(); };
    };
    Wake            wake;
    QSocTaskNotices notices;

    /* Idle maintenance state. */
    QStringList             pendingAutoInputs;
    QHash<QString, quint64> invokedSkills;
    quint64                 skillSeq           = 1;
    bool                    dreamAttempted     = false;
    bool                    titleGenerated     = false;
    bool                    memoryCapNotified  = false;
    bool                    awaySummaryShown   = false;
    bool                    awaySummaryPending = false;

    /* The prompt's skill listing for the workspace bound now. */
    QString skillListing() const;

    /* Stamp the run context (model, effort, dirs) onto a run record. */
    void applyRunContext(QSocAgentRuntime *runtime, QSocSession::RunRecord &record);

    /* Session write barrier installation helper. */
    void installSessionWriteBarrier(
        QSocAgentRuntime *runtime, QSocSession *session, QSocFileHistory *history);
};

#endif /* QSOCAGENTRUNTIME_P_H */
