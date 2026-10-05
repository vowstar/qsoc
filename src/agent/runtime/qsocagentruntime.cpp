// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentruntime.h"
#include "agent/runtime/qsocagentruntime_p.h"

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/qsocagent.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocawaysummary.h"
#include "agent/qsocbashtasksource.h"
#include "agent/qsoccontextrestore.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocgoal.h"
#include "agent/qsocgoalprompt.h"
#include "agent/qsochookmanager.h"
#include "agent/qsoclooptasksource.h"
#include "agent/qsocmemorydream.h"
#include "agent/qsocmemoryextractor.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocrewind.h"
#include "agent/qsocsession.h"
#include "agent/qsocsessionrecovery.h"
#include "agent/qsocsessiontitle.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/qsoctaskforecast.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/services/qsocloopscheduler.h"
#include "agent/tool/qsoctoolagent.h"
#include "agent/tool/qsoctoolagentmessage.h"
#include "agent/tool/qsoctoolagentresume.h"
#include "agent/tool/qsoctoolagentstatus.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolbus.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolgenerate.h"
#include "agent/tool/qsoctoolgoalcomplete.h"
#include "agent/tool/qsoctoolhostcatalog.h"
#include "agent/tool/qsoctoollsp.h"
#include "agent/tool/qsoctoolmemory.h"
#include "agent/tool/qsoctoolmodule.h"
#include "agent/tool/qsoctoolmonitor.h"
#include "agent/tool/qsoctooloutputread.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "agent/tool/qsoctoolproject.h"
#include "agent/tool/qsoctoolresources.h"
#include "agent/tool/qsoctoolschedule.h"
#include "agent/tool/qsoctoolsendmessage.h"
#include "agent/tool/qsoctoolshell.h"
#include "agent/tool/qsoctoolskill.h"
#include "agent/tool/qsoctoolsmt.h"
#include "agent/tool/qsoctooltodo.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/config.h"
#include "common/qllmservice.h"
#include "common/qlspconfigloader.h"
#include "common/qlspservice.h"
#include "common/qlspslangbackend.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qsocconsole.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocinterrupt.h"
#include "common/qsocmcptypes.h"
#include "common/qsocmessageauthority.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocpaths.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocproxy.h"
#include "common/qsocshellexecutor.h"
#include "common/qsoctaskregistry.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <limits>

using json = nlohmann::json;

namespace QSocAgentRuntimeInternal {

QString sessionProjectPath(QSocProjectManager *pmanager)
{
    QString projectPath = pmanager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }
    return projectPath;
}

bool freshSessionPathAvailable(const QString &sessionPath)
{
    const QFileInfo info(sessionPath);
    return !info.exists() && !info.isSymLink();
}

bool existingSessionPathIsRegular(const QString &sessionPath)
{
    const QFileInfo info(sessionPath);
    return info.isFile() && !info.isSymLink();
}

std::unique_ptr<QLockFile> lockSession(const QString &sessionPath)
{
    const QFileInfo sessionInfo(sessionPath);
    if (!QDir().mkpath(sessionInfo.absolutePath())) {
        return {};
    }
    auto lock = std::make_unique<QLockFile>(sessionPath + QStringLiteral(".lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        return {};
    }
    return lock;
}

bool persistSessionState(
    QSocAgent *agent, QSocSession *session, json &persistedMessages, int &lastPersistedIndex)
{
    const json messages = agent->getMessages();
    if (session == nullptr || !messages.is_array() || !persistedMessages.is_array()) {
        return false;
    }

    bool appendOnly = persistedMessages.size() <= messages.size();
    for (json::size_type index = 0; appendOnly && index < persistedMessages.size(); ++index) {
        appendOnly = persistedMessages[index] == messages[index];
    }

    if (!appendOnly) {
        if (!session->appendSnapshot(messages)) {
            return false;
        }
        persistedMessages  = messages;
        lastPersistedIndex = static_cast<int>(messages.size());
        return true;
    }

    for (json::size_type index = persistedMessages.size(); index < messages.size(); ++index) {
        if (!session->appendMessage(messages[index])) {
            return false;
        }
        persistedMessages.push_back(messages[index]);
        lastPersistedIndex = static_cast<int>(persistedMessages.size());
    }
    return true;
}

bool persistRecoverySnapshot(
    QSocSession *session, const json &messages, json &persistedMessages, int &lastPersistedIndex)
{
    if (messages == persistedMessages) {
        return true;
    }
    if (session == nullptr || !messages.is_array() || !persistedMessages.is_array()
        || messages.size() > static_cast<json::size_type>(std::numeric_limits<int>::max())
        || !session->appendSnapshot(messages)) {
        return false;
    }
    persistedMessages  = messages;
    lastPersistedIndex = static_cast<int>(messages.size());
    return true;
}

} // namespace QSocAgentRuntimeInternal

using QSocAgentRuntimeInternal::existingSessionPathIsRegular;
using QSocAgentRuntimeInternal::freshSessionPathAvailable;
using QSocAgentRuntimeInternal::lockSession;
using QSocAgentRuntimeInternal::persistRecoverySnapshot;
using QSocAgentRuntimeInternal::persistSessionState;
using QSocAgentRuntimeInternal::sessionProjectPath;

/* The slash commands the runtime owns. Presentation-only commands
 * (help, effort menus, model menus) stay in the frontend. */
namespace {

const QStringList kRuntimeCommands = {
    QStringLiteral("/clear"),  QStringLiteral("/compact"), QStringLiteral("/context"),
    QStringLiteral("/cost"),   QStringLiteral("/cache"),   QStringLiteral("/status"),
    QStringLiteral("/mcp"),    QStringLiteral("/branch"),  QStringLiteral("/rename"),
    QStringLiteral("/memory"), QStringLiteral("/plan"),    QStringLiteral("/local"),
    QStringLiteral("/goal"),   QStringLiteral("/cwd"),     QStringLiteral("/project"),
    QStringLiteral("/ssh"),    QStringLiteral("/agents"),  QStringLiteral("/agents-history"),
    QStringLiteral("/loop"),   QStringLiteral("/diff"),    QStringLiteral("/help"),
    QStringLiteral("/model"),  QStringLiteral("/effort"),  QStringLiteral("/resume"),
    QStringLiteral("/rewind"), QStringLiteral("/btw"),
};

bool isRuntimeCommand(const QString &cmd)
{
    for (const QString &name : kRuntimeCommands) {
        if (cmd == name || cmd.startsWith(name + QLatin1Char(' '))) {
            return true;
        }
    }
    return false;
}

} // namespace

/* Helpers + Private live in qsocagentruntime_p.h. */

QSocAgentRuntime::QSocAgentRuntime(const QSocAgentRuntimeOptions &options, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->ownsManagers   = true;
    d->options        = options;
    d->projectManager = new QSocProjectManager(this);
    d->socConfig      = new QSocConfig(this, d->projectManager);
    d->llmService     = new QLLMService(this, d->socConfig);
    d->busManager     = new QSocBusManager(this, d->projectManager);
    d->moduleManager = new QSocModuleManager(this, d->projectManager, d->busManager, d->llmService);
    d->generateManager
        = new QSocGenerateManager(this, d->projectManager, d->moduleManager, d->busManager);
    assembleInfrastructure(options);
}

QSocAgentRuntime::QSocAgentRuntime(
    const QSocAgentRuntimeOptions &options,
    QSocProjectManager            *projectManager,
    QSocConfig                    *socConfig,
    QLLMService                   *llmService,
    QSocBusManager                *busManager,
    QSocModuleManager             *moduleManager,
    QSocGenerateManager           *generateManager,
    QObject                       *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->ownsManagers   = false;
    d->options        = options;
    d->projectManager = projectManager != nullptr ? projectManager : new QSocProjectManager(this);
    d->socConfig      = socConfig != nullptr ? socConfig : new QSocConfig(this, d->projectManager);
    d->llmService     = llmService != nullptr ? llmService : new QLLMService(this, d->socConfig);
    d->busManager     = busManager != nullptr ? busManager
                                              : new QSocBusManager(this, d->projectManager);
    d->moduleManager
        = moduleManager != nullptr
              ? moduleManager
              : new QSocModuleManager(this, d->projectManager, d->busManager, d->llmService);
    d->generateManager
        = generateManager != nullptr
              ? generateManager
              : new QSocGenerateManager(this, d->projectManager, d->moduleManager, d->busManager);
    assembleInfrastructure(options);
}

QSocAgentRuntime::~QSocAgentRuntime()
{
    // QObject normally deletes children after members. Stop and delete them
    // while the private state captured by their callbacks is still alive.
    for (QObject *child : findChildren<QObject *>())
        child->disconnect(this);
    abort();
    d->remoteConn->setAbortProbe({});
    d->remoteConn->setWorkingDirectoryObserver({});
    d->remoteConn->teardown();
    while (!children().isEmpty())
        delete children().last();
}

/* ---------------------------------------------------------------------- */
/* Infrastructure accessors. */

QSocAgent *QSocAgentRuntime::agent() const
{
    return d->agent;
}

QSocToolRegistry *QSocAgentRuntime::toolRegistry() const
{
    return d->agent ? d->agent->getToolRegistry() : d->toolRegistry;
}

QLLMService *QSocAgentRuntime::llmService() const
{
    return d->llmService;
}

QSocProjectManager *QSocAgentRuntime::projectManager() const
{
    return d->projectManager;
}

QSocConfig *QSocAgentRuntime::config() const
{
    return d->socConfig;
}

QSocBusManager *QSocAgentRuntime::busManager() const
{
    return d->busManager;
}

QSocModuleManager *QSocAgentRuntime::moduleManager() const
{
    return d->moduleManager;
}

QSocGenerateManager *QSocAgentRuntime::generateManager() const
{
    return d->generateManager;
}

QSocMemoryManager *QSocAgentRuntime::memoryManager() const
{
    return d->memoryManager;
}

QSocLoopScheduler *QSocAgentRuntime::loopScheduler() const
{
    return d->loopScheduler;
}

QSocTaskRegistry *QSocAgentRuntime::taskRegistry() const
{
    return d->taskRegistry;
}

QSocSubAgentTaskSource *QSocAgentRuntime::subAgentSource() const
{
    return d->subAgentTaskSource;
}

QSocHostCatalog *QSocAgentRuntime::hostCatalog() const
{
    return d->hostCatalog;
}

QSocGoalCatalog *QSocAgentRuntime::goalCatalog() const
{
    return d->goalCatalog;
}

QSocMcpManager *QSocAgentRuntime::mcpManager() const
{
    return d->mcpManager;
}

QSocPathContext *QSocAgentRuntime::pathContext() const
{
    return d->pathContext;
}

QSocMonitorTaskSource *QSocAgentRuntime::monitorTaskSource() const
{
    return d->monitorTaskSource;
}

QSocTaskEventQueue *QSocAgentRuntime::taskEventQueue() const
{
    return d->taskEventQueue;
}

QSocRemoteConnection *QSocAgentRuntime::remoteConnection() const
{
    return d->remoteConn;
}

QSocToolRegistry *QSocAgentRuntime::remoteToolRegistry() const
{
    return d->remoteRegistry;
}

QSocToolRegistry *QSocAgentRuntime::localToolRegistry() const
{
    return d->localRegistry;
}

QString QSocAgentRuntime::resumeSessionId() const
{
    return d->options.resumeSessionId;
}

QSocFileHistory *QSocAgentRuntime::fileHistory() const
{
    return d->currentFileHistory.get();
}

QString QSocAgentRuntime::sessionId() const
{
    return d->currentSession ? d->currentSession->id() : QString();
}

QSocSession *QSocAgentRuntime::session() const
{
    return d->currentSession.get();
}

nlohmann::json QSocAgentRuntime::persistedMessages() const
{
    return d->persistedMessages;
}

int QSocAgentRuntime::lastPersistedIndex() const
{
    return d->lastPersistedIndex;
}

QString QSocAgentRuntime::resumeCommand() const
{
    return d->currentSession ? d->currentSession->resumeCommand(
                                   d->options.clientProgram,
                                   sessionProjectPath(d->projectManager),
                                   d->options.launchDirectory,
                                   d->remoteConn->target(),
                                   d->remoteConn->workspace())
                             : QString();
}

int QSocAgentRuntime::lastMemoryIndex() const
{
    return d->memoryCursor.index;
}

int QSocAgentRuntime::turnCounter() const
{
    return d->turnCounter;
}

QString QSocAgentRuntime::sessionPath() const
{
    return d->currentSession ? d->currentSession->filePath() : QString();
}

QString QSocAgentRuntime::lastError() const
{
    return d->lastErrorText;
}

void QSocAgentRuntime::assembleInfrastructure(const QSocAgentRuntimeOptions &options)
{
    /* Project selection: -d reloads config, -p loads a named project,
     * otherwise the first available project loads silently. */
    if (!options.projectDirectory.isEmpty()) {
        d->projectManager->setProjectPath(options.projectDirectory);
        d->socConfig->loadConfig();
        d->llmService->setConfig(d->socConfig);
    }
    if (!options.projectName.isEmpty()) {
        if (!d->projectManager->load(options.projectName)) {
            d->lastErrorText = QStringLiteral("failed to load project %1").arg(options.projectName);
        }
    } else {
        d->projectManager->loadFirst(true);
    }

    if (options.streamingFromConfig) {
        const QString stream = d->socConfig->getValue("agent.stream", "true").toLower();
        d->options.streaming = stream == "true" || stream == "1";
    }
    QSocProxy::setQsocWideDefault(QSocProxy::fromLegacyConfig(d->socConfig));

    /* Agent config from the merged config layers. applyOptionsToConfig()
     * assembles the full config (config file + env + caller overrides) and
     * stores it in d->agentConfig for the agent constructor below. */
    applyOptionsToConfig(options);
    QSocAgentConfig config = d->agentConfig;
    config.verbose         = options.verbose || QSocConsole::level() >= QSocConsole::Level::Debug;

    /* Loop scheduler shared by /loop and schedule_* tools. */
    d->loopScheduler = new QSocLoopScheduler(this);

    d->lspService = new QLspService(this);
    registerTools();
    startMcp();
    startLsp();

    /* Agent construction. */
    d->agent = new QSocAgent(this, d->llmService, d->toolRegistry, config);
    d->subAgentTaskSource->enableMessaging(d->agent);
    d->agent->setRequestBoundaryHandler([] { QSocInterrupt::clearRequest(); });
    d->agent->setMemoryManager(d->memoryManager);
    d->agent->setLoopScheduler(d->loopScheduler);
    d->agent->setHostCatalog(d->hostCatalog);
    d->agent->setGoalCatalog(d->goalCatalog);
    d->agent->setHookManager(d->hookManager);
    d->agent->setContextualBashSafetyJudge(QSocAgent::classifyBashCommand);

    wireAgentCallbacks();
    wirePersistence();
    wireContextRestore();
    wireAuxiliaryServices();

    /* Local workspace: apply --workspace before any tool runs. */
    if (!options.workspace.isEmpty() && options.sshTarget.isEmpty()) {
        connectLocalWorkspace(options.workspace);
    }

    /* Remote workspace: connect before the first prompt when requested. */
    if (!options.sshTarget.isEmpty() && !options.deferRemoteConnection) {
        QString error;
        if (!connectRemote(
                {.target = options.sshTarget, .workspace = options.workspace, .remember = false},
                &error)) {
            d->lastErrorText = error;
        }
    }

    /* Task forecast over the registry. */
    auto *forecast = new QSocTaskForecast(d->taskRegistry, d->agent, this);
    forecast->setEnabled(
        !d->socConfig || d->socConfig->getValue("agent.task_estimates", "true") != "false");
}

bool QSocAgentRuntime::applyOptionsToConfig(const QSocAgentRuntimeOptions &options)
{
    /* Config-file values first (identical key set to the old CLI path). */
    QSocAgentConfig config;
    if (d->socConfig) {
        const QString tempStr = d->socConfig->getValue("agent.temperature");
        if (!tempStr.isEmpty()) {
            config.temperature = tempStr.toDouble();
        }
        const QString maxTokensStr = d->socConfig->getValue("agent.max_tokens");
        if (!maxTokensStr.isEmpty()) {
            config.maxContextTokens = maxTokensStr.toInt();
        }
        const QString maxIterStr = d->socConfig->getValue("agent.max_iterations");
        if (!maxIterStr.isEmpty()) {
            config.maxIterations = maxIterStr.toInt();
        }
        const auto readArtifactLimit = [&](const QString &key, qint64 &value) {
            bool         ok     = false;
            const qint64 parsed = d->socConfig->getValue(key).toLongLong(&ok);
            if (ok && parsed > 0)
                value = parsed;
        };
        readArtifactLimit(QStringLiteral("agent.tool_artifact_bytes"), config.toolArtifactBytes);
        readArtifactLimit(
            QStringLiteral("agent.tool_artifact_session_bytes"), config.toolArtifactSessionBytes);
        readArtifactLimit(
            QStringLiteral("agent.tool_artifact_page_bytes"), config.toolArtifactPageBytes);
        const QString pruneThresholdStr = d->socConfig->getValue("agent.prune_threshold");
        if (!pruneThresholdStr.isEmpty()) {
            config.pruneThreshold = pruneThresholdStr.toDouble();
        }
        const QString compactThresholdStr = d->socConfig->getValue("agent.compact_threshold");
        if (!compactThresholdStr.isEmpty()) {
            config.compactThreshold = compactThresholdStr.toDouble();
        }
        const QString compactionModelStr = d->socConfig->getValue("agent.compaction_model");
        if (!compactionModelStr.isEmpty()) {
            config.compactionModel = compactionModelStr;
        }
        const QString systemPrompt = d->socConfig->getValue("agent.system_prompt");
        if (!systemPrompt.isEmpty()) {
            config.systemPromptOverride = systemPrompt;
        }
        const QString toolPresentation = d->socConfig->getValue("agent.tool_presentation");
        if (!toolPresentation.isEmpty())
            config.toolPresentation = toolPresentation;
        const QString effortStr = d->socConfig->getValue("agent.effort");
        if (!effortStr.isEmpty()) {
            config.effortLevel = effortStr;
        }
        const auto readBool = [&](const QString &key, bool &value) {
            const QString raw = d->socConfig->getValue(key);
            if (!raw.isEmpty()) {
                value = (raw.toLower() == "true" || raw == "1");
            }
        };
        readBool(QStringLiteral("agent.auto_load_memory"), config.autoLoadMemory);
        const QString memoryMaxCharsStr = d->socConfig->getValue("agent.memory_max_chars");
        if (!memoryMaxCharsStr.isEmpty()) {
            config.memoryMaxChars = memoryMaxCharsStr.toInt();
        }
        readBool(QStringLiteral("agent.memory_recall"), config.memoryRecallEnabled);
        const QString memoryRecallModelStr = d->socConfig->getValue("agent.memory_recall_model");
        if (!memoryRecallModelStr.isEmpty()) {
            config.memoryRecallModel = memoryRecallModelStr;
        }
        const QString memoryRecallMaxFilesStr = d->socConfig->getValue(
            "agent.memory_recall_max_files");
        if (!memoryRecallMaxFilesStr.isEmpty()) {
            config.memoryRecallMaxFiles = memoryRecallMaxFilesStr.toInt();
        }
        const QString memoryRecallPerFileCapStr = d->socConfig->getValue(
            "agent.memory_recall_per_file_cap");
        if (!memoryRecallPerFileCapStr.isEmpty()) {
            config.memoryRecallPerFileCap = memoryRecallPerFileCapStr.toInt();
        }
        const QString memoryRecallTurnBudgetStr = d->socConfig->getValue(
            "agent.memory_recall_turn_budget");
        if (!memoryRecallTurnBudgetStr.isEmpty()) {
            config.memoryRecallTurnBudget = memoryRecallTurnBudgetStr.toInt();
        }
        readBool(QStringLiteral("agent.memory_extract"), config.memoryExtractEnabled);
        const QString memoryExtractModelStr = d->socConfig->getValue("agent.memory_extract_model");
        if (!memoryExtractModelStr.isEmpty()) {
            config.memoryExtractModel = memoryExtractModelStr;
        }
        const QString memoryExtractCadenceStr = d->socConfig->getValue(
            "agent.memory_extract_cadence");
        if (!memoryExtractCadenceStr.isEmpty()) {
            config.memoryExtractEveryTurns = memoryExtractCadenceStr.toInt();
        }
        const QString memoryExtractMinMsgStr = d->socConfig->getValue(
            "agent.memory_extract_min_messages");
        if (!memoryExtractMinMsgStr.isEmpty()) {
            config.memoryExtractMinNewMessages = memoryExtractMinMsgStr.toInt();
        }
        readBool(QStringLiteral("agent.memory_dream"), config.memoryDreamEnabled);
        const QString memoryDreamModelStr = d->socConfig->getValue("agent.memory_dream_model");
        if (!memoryDreamModelStr.isEmpty()) {
            config.memoryDreamModel = memoryDreamModelStr;
        }
        const QString memoryDreamMinHoursStr = d->socConfig->getValue(
            "agent.memory_dream_min_hours");
        if (!memoryDreamMinHoursStr.isEmpty()) {
            config.memoryDreamMinHours = memoryDreamMinHoursStr.toInt();
        }
        const QString memoryDreamMinSessStr = d->socConfig->getValue(
            "agent.memory_dream_min_sessions");
        if (!memoryDreamMinSessStr.isEmpty()) {
            config.memoryDreamMinSessions = memoryDreamMinSessStr.toInt();
        }
        readBool(QStringLiteral("agent.session_title"), config.sessionTitleEnabled);
        const QString sessionTitleModelStr = d->socConfig->getValue("agent.session_title_model");
        if (!sessionTitleModelStr.isEmpty()) {
            config.sessionTitleModel = sessionTitleModelStr;
        }
        readBool(QStringLiteral("agent.away_summary"), config.awaySummaryEnabled);
        const QString awaySummaryModelStr = d->socConfig->getValue("agent.away_summary_model");
        if (!awaySummaryModelStr.isEmpty()) {
            config.awaySummaryModel = awaySummaryModelStr;
        }
        const QString awaySummaryDelayStr = d->socConfig->getValue(
            "agent.away_summary_delay_seconds");
        if (!awaySummaryDelayStr.isEmpty()) {
            config.awaySummaryDelaySeconds = awaySummaryDelayStr.toInt();
        }
        readBool(QStringLiteral("agent.context_restore"), config.contextRestoreEnabled);
        const QString contextRestoreMaxFilesStr = d->socConfig->getValue(
            "agent.context_restore_max_files");
        if (!contextRestoreMaxFilesStr.isEmpty()) {
            config.contextRestoreMaxFiles = contextRestoreMaxFilesStr.toInt();
        }
        const QString contextRestoreFileBudgetStr = d->socConfig->getValue(
            "agent.context_restore_file_budget");
        if (!contextRestoreFileBudgetStr.isEmpty()) {
            config.contextRestoreFileBudget = contextRestoreFileBudgetStr.toInt();
        }
        const QString contextRestoreMaxTokensFileStr = d->socConfig->getValue(
            "agent.context_restore_max_tokens_per_file");
        if (!contextRestoreMaxTokensFileStr.isEmpty()) {
            config.contextRestoreMaxTokensFile = contextRestoreMaxTokensFileStr.toInt();
        }
        const QString contextRestoreMaxTokensSkillStr = d->socConfig->getValue(
            "agent.context_restore_max_tokens_per_skill");
        if (!contextRestoreMaxTokensSkillStr.isEmpty()) {
            config.contextRestoreMaxTokensSkill = contextRestoreMaxTokensSkillStr.toInt();
        }
        const QString contextRestoreSkillBudgetStr = d->socConfig->getValue(
            "agent.context_restore_skill_budget");
        if (!contextRestoreSkillBudgetStr.isEmpty()) {
            config.contextRestoreSkillBudget = contextRestoreSkillBudgetStr.toInt();
        }
        const QString maxConcurrentStr = d->socConfig->getValue("agent.max_concurrent_subagents");
        if (!maxConcurrentStr.isEmpty()) {
            config.maxConcurrentSubagents = maxConcurrentStr.toInt();
        }
        const QString autoBackgroundMsStr = d->socConfig->getValue("agent.auto_background_ms");
        if (!autoBackgroundMsStr.isEmpty()) {
            config.autoBackgroundMs = autoBackgroundMsStr.toInt();
        }
        config.hooks = d->socConfig->agentHooks();
    }

    /* Environment overrides. */
    if (qEnvironmentVariableIsSet("QSOC_MAX_CONCURRENT_SUBAGENTS")) {
        bool      validInt = false;
        const int parsed = qEnvironmentVariableIntValue("QSOC_MAX_CONCURRENT_SUBAGENTS", &validInt);
        if (validInt) {
            config.maxConcurrentSubagents = parsed;
        }
    }
    if (qEnvironmentVariableIsSet("QSOC_AUTO_BACKGROUND_MS")) {
        bool      validInt = false;
        const int parsed   = qEnvironmentVariableIntValue("QSOC_AUTO_BACKGROUND_MS", &validInt);
        if (validInt) {
            config.autoBackgroundMs = parsed;
        }
    }

    /* Caller overrides. */
    if (options.maxContextTokens > 0) {
        config.maxContextTokens = options.maxContextTokens;
    }
    if (options.temperature >= 0.0) {
        config.temperature = options.temperature;
    }
    if (!options.effortLevel.isEmpty()) {
        config.effortLevel = options.effortLevel.toLower();
    }
    if (!options.toolPresentation.isEmpty()) {
        config.toolPresentation = options.toolPresentation;
    }

    /* Project path for AGENTS.md injection. */
    config.projectPath = d->projectManager->getProjectPath();

    config.skillListing = d->skillListing();

    /* Model id + registry context sync. */
    if (d->llmService) {
        config.modelId        = d->llmService->getCurrentModelId();
        const QString modelId = d->llmService->getCurrentModelId();
        if (!modelId.isEmpty()) {
            LLMModelConfig modelCfg = d->llmService->getModelConfig(modelId);
            if (modelCfg.contextTokens > 0 && options.maxContextTokens <= 0) {
                config.maxContextTokens = modelCfg.contextTokens;
            }
            if (!modelCfg.effort.isEmpty() && options.effortLevel.isEmpty()) {
                config.effortLevel = modelCfg.effort;
            }
        }
    }

    d->agentConfig = config;
    return true;
}

void QSocAgentRuntime::registerTools()
{
    d->toolRegistry = new QSocToolRegistry(this);
    d->toolRegistry->registerTool(new QSocToolOutputRead(d->toolRegistry));

    /* Project tools */
    auto *projectListTool   = new QSocToolProjectList(this, d->projectManager);
    auto *projectShowTool   = new QSocToolProjectShow(this, d->projectManager);
    auto *projectCreateTool = new QSocToolProjectCreate(this, d->projectManager);
    d->toolRegistry->registerTool(projectListTool);
    d->toolRegistry->registerTool(projectShowTool);
    d->toolRegistry->registerTool(projectCreateTool);

    /* Module tools */
    auto *moduleListTool   = new QSocToolModuleList(this, d->moduleManager);
    auto *moduleShowTool   = new QSocToolModuleShow(this, d->moduleManager);
    auto *moduleImportTool = new QSocToolModuleImport(this, d->moduleManager);
    auto *moduleBusAddTool = new QSocToolModuleBusAdd(this, d->moduleManager);
    d->toolRegistry->registerTool(moduleListTool);
    d->toolRegistry->registerTool(moduleShowTool);
    d->toolRegistry->registerTool(moduleImportTool);
    d->toolRegistry->registerTool(moduleBusAddTool);

    /* Bus tools */
    auto *busListTool   = new QSocToolBusList(this, d->busManager);
    auto *busShowTool   = new QSocToolBusShow(this, d->busManager);
    auto *busImportTool = new QSocToolBusImport(this, d->busManager);
    d->toolRegistry->registerTool(busListTool);
    d->toolRegistry->registerTool(busShowTool);
    d->toolRegistry->registerTool(busImportTool);

    /* Generate tools */
    auto *generateVerilogTool  = new QSocToolGenerateVerilog(this, d->generateManager);
    auto *generateTemplateTool = new QSocToolGenerateTemplate(this, d->generateManager);
    d->toolRegistry->registerTool(generateVerilogTool);
    d->toolRegistry->registerTool(generateTemplateTool);

    /* Path context (must be before file tools) */
    d->pathContext = new QSocPathContext(this, d->projectManager);
    if (!d->options.projectDirectory.isEmpty())
        d->pathContext->setWorkingDir(d->options.projectDirectory);
    auto *pathContextTool = new QSocToolPathContext(this, d->pathContext);
    d->toolRegistry->registerTool(pathContextTool);

    /* File tools */
    auto *fileReadTool  = new QSocToolFileRead(this, d->pathContext, d->llmService);
    auto *fileListTool  = new QSocToolFileList(this, d->pathContext);
    auto *fileWriteTool = new QSocToolFileWrite(this, d->pathContext);
    auto *fileEditTool  = new QSocToolFileEdit(this, d->pathContext);
    d->toolRegistry->registerTool(fileReadTool);
    d->toolRegistry->registerTool(fileListTool);
    fileWriteTool->setLspService(d->lspService);
    fileEditTool->setLspService(d->lspService);
    d->toolRegistry->registerTool(fileWriteTool);
    d->toolRegistry->registerTool(fileEditTool);

    /* Shell tools */
    auto *shellBashTool = new QSocToolShellBash(this, d->projectManager);
    shellBashTool->setWorkingDirectoryProvider([this] { return workingDirectory(); });
    auto *bashManageTool = new QSocToolBashManage(this);
    bashManageTool->setOwner(shellBashTool);
    /* With no local shell every call would fail, so the tools are not offered. */
    const bool localShell = localShellExecutor().available();
    if (localShell) {
        d->toolRegistry->registerTool(shellBashTool);
        d->toolRegistry->registerTool(bashManageTool);
    }

    d->taskEventQueue    = new QSocTaskEventQueue(this);
    d->monitorTaskSource = new QSocMonitorTaskSource(this, d->taskEventQueue, d->projectManager);
    if (localShell) {
        d->toolRegistry->registerTool(new QSocToolMonitor(this, d->monitorTaskSource));
        d->toolRegistry->registerTool(new QSocToolMonitorStop(this, d->monitorTaskSource));
    }

    d->toolRegistry->registerTool(new QSocToolResources(this, [this] {
        return isRemote() ? QStringList{} : QStringList{workingDirectory()};
    }));

    /* Documentation tools */
    d->toolRegistry->registerTool(new QSocToolDocQuery(this));
    if (QSocToolSmt::supported())
        d->toolRegistry->registerTool(new QSocToolSmt(this));

    /* Memory manager and tools */
    d->memoryManager = new QSocMemoryManager(this, d->projectManager);
    d->toolRegistry->registerTool(new QSocToolMemoryRead(this, d->memoryManager));
    d->toolRegistry->registerTool(new QSocToolMemoryWrite(this, d->memoryManager));
    d->toolRegistry->registerTool(new QSocToolMemoryDelete(this, d->memoryManager));

    /* Todo tools */
    d->toolRegistry->registerTool(new QSocToolTodoList(this, d->projectManager));
    d->toolRegistry->registerTool(new QSocToolTodoAdd(this, d->projectManager));
    d->toolRegistry->registerTool(new QSocToolTodoUpdate(this, d->projectManager));
    d->toolRegistry->registerTool(new QSocToolTodoDelete(this, d->projectManager));

    /* Skill tools */
    auto *skillFindTool   = new QSocToolSkillFind(this, d->projectManager);
    auto *skillCreateTool = new QSocToolSkillCreate(this, d->projectManager);
    d->toolRegistry->registerTool(skillFindTool);
    d->toolRegistry->registerTool(skillCreateTool);

    /* Web tools */
    d->toolRegistry->registerTool(new QSocToolWebFetch(this, d->socConfig, d->llmService));
    if (d->socConfig && !d->socConfig->getValue("web.search_api_url").isEmpty()) {
        d->toolRegistry->registerTool(new QSocToolWebSearch(this, d->socConfig));
    }

    /* Schedule tools */
    d->toolRegistry->registerTool(new QSocToolScheduleCreate(this, d->loopScheduler));
    d->toolRegistry->registerTool(new QSocToolScheduleList(this, d->loopScheduler));
    d->toolRegistry->registerTool(new QSocToolScheduleDelete(this, d->loopScheduler));

    /* Task registry */
    d->taskRegistry = new QSocTaskRegistry(this);
    d->taskRegistry->registerSource(new QSocLoopTaskSource(d->loopScheduler, this));
    d->taskRegistry->registerSource(new QSocBashTaskSource(shellBashTool, this));
    d->taskRegistry->registerSource(d->monitorTaskSource);
    d->subAgentTaskSource = new QSocSubAgentTaskSource(this);
    d->subAgentTaskSource->loadHistoricalRuns();
    d->taskRegistry->registerSource(d->subAgentTaskSource);

    /* LSP tool */
    auto *lspTool = new QSocToolLsp(this);
    lspTool->setService(d->lspService);
    lspTool->setWorkingDirectoryProvider([this] { return workingDirectory(); });
    d->toolRegistry->registerTool(lspTool);

    /* Sub-agent definitions + spawn tool. */
    d->agentDefinitions = new QSocAgentDefinitionRegistry(this);
    d->agentDefinitions->registerBuiltins();
    {
        const QString userAgentsDir = QStandardPaths::writableLocation(
                                          QStandardPaths::AppConfigLocation)
                                      + QStringLiteral("/agents");
        const QString projectRoot   = d->projectManager->getProjectPath();
        QString       projectAgentsDir;
        if (!projectRoot.isEmpty()) {
            projectAgentsDir = QDir(projectRoot).filePath(QStringLiteral(".qsoc/agents"));
        }
        d->agentDefinitions->scanFromDisk(userAgentsDir, projectAgentsDir);
    }
    QSocToolAgent::sweepStaleWorktrees();

    /* Host catalog. */
    d->hostCatalog = new QSocHostCatalog(this);
    {
        const QString userHostDir = QStandardPaths::writableLocation(
            QStandardPaths::AppConfigLocation);
        const QString projectRoot = d->projectManager->getProjectPath();
        d->hostCatalog->load(userHostDir, projectRoot);
    }
    d->hostBindingDir = QSocHostBindingStore::defaultDir();

    /* Goal catalog. */
    d->goalCatalog = new QSocGoalCatalog(this);
    d->goalCatalog->load(d->projectManager->getProjectPath());

    /* Hook manager. */
    d->hookManager = new QSocHookManager(this);
    d->hookManager->setConfig(d->agentConfig.hooks);

    /* Spawn tool + companions. */
    auto *agentTool = new QSocToolAgent(
        this,
        d->llmService,
        d->toolRegistry,
        d->agentConfig,
        d->agentDefinitions,
        d->subAgentTaskSource);
    agentTool->setMemoryManager(d->memoryManager);
    agentTool->setHookManager(d->hookManager);
    agentTool->setLoopScheduler(d->loopScheduler);
    agentTool->setHostCatalog(d->hostCatalog);
    d->sshConfig = std::make_unique<QSocSshConfigParser>();
    {
        const QString cfg = QDir::homePath() + QStringLiteral("/.ssh/config");
        if (QFileInfo::exists(cfg)) {
            d->sshConfig->parse(cfg);
        }
    }
    agentTool->setSshConfigParser(d->sshConfig.get());
    d->toolRegistry->registerTool(agentTool);

    auto *agentStatusTool = new QSocToolAgentStatus(this, d->subAgentTaskSource);
    d->toolRegistry->registerTool(agentStatusTool);
    auto *sendMessageTool = new QSocToolSendMessage(this, d->subAgentTaskSource);
    d->toolRegistry->registerTool(sendMessageTool);
    for (const auto *name :
         {"agent_list", "agent_inbox", "wait_agent", "followup_task", "interrupt_agent"}) {
        d->toolRegistry->registerTool(new QSocToolAgentMessage(
            this, d->subAgentTaskSource->mailbox(), QString::fromLatin1(name)));
    }

    d->toolRegistry->registerTool(new QSocToolHostRegister(this, d->hostCatalog));
    d->toolRegistry->registerTool(new QSocToolHostUpdate(this, d->hostCatalog));
    d->toolRegistry->registerTool(new QSocToolHostRemove(this, d->hostCatalog));

    /* User-question + plan-mode tools: callbacks installed by the frontend. */
    d->toolRegistry->registerTool(new QSocToolAskUser(this, nullptr));
    d->toolRegistry->registerTool(new QSocToolEnterPlanMode(this, nullptr));
    d->toolRegistry->registerTool(new QSocToolExitPlanMode(this, nullptr));

    d->toolRegistry->registerTool(new QSocToolGoalComplete(this, d->goalCatalog));

    auto *resumeTool = new QSocToolAgentResume(this, d->subAgentTaskSource);
    d->toolRegistry->registerTool(resumeTool);

    d->localRegistry = d->toolRegistry;
}

void QSocAgentRuntime::startMcp()
{
    if (d->mcpManager != nullptr || d->socConfig == nullptr) {
        return;
    }
    const QList<McpServerConfig> mcpConfigs = d->socConfig->mcpServers();
    if (mcpConfigs.isEmpty()) {
        return;
    }
    d->mcpManager = new QSocMcpManager(mcpConfigs, d->toolRegistry, this);
    d->mcpManager->startAll();
    QElapsedTimer mcpTimer;
    mcpTimer.start();
    const int handshakeBudgetMs = 1500;
    while (mcpTimer.elapsed() < handshakeBudgetMs) {
        bool allSettled = true;
        for (const QString &name : d->mcpManager->serverNames()) {
            auto *client = d->mcpManager->findClient(name);
            if (client == nullptr) {
                continue;
            }
            if (client->state() != QSocMcpClient::State::Ready
                && client->state() != QSocMcpClient::State::Failed
                && !d->mcpManager->hasGivenUp(name)) {
                allSettled = false;
                break;
            }
        }
        if (allSettled) {
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void QSocAgentRuntime::startLsp()
{
    auto *lspService = d->lspService;
    lspService->addBackend(new QLspSlangBackend(lspService));
    QLspConfigLoader::loadAndRegister(lspService, d->socConfig);
    lspService->startAll(d->projectManager->getProjectPath());
}

void QSocAgentRuntime::wireAgentCallbacks()
{
    connect(d->agent, &QSocAgent::verboseOutput, this, [this](const QString &message) {
        QSocConsole::debug().noquote().nospace() << Q_FUNC_INFO << ":" << message;
    });

    /* Streaming content. */
    connect(d->agent, &QSocAgent::contentChunk, this, [this](const QString &chunk) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::ContentChunk;
        event.text = chunk;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::reasoningChunk, this, [this](const QString &chunk) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::ReasoningChunk;
        event.text = chunk;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::runComplete, this, [this](const QString &final) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::RunComplete;
        event.text = final;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::runError, this, [this](const QString &error) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::RunError;
        event.text = error;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::runAborted, this, [this](const QString &) {
        QSocAgentRuntimeEvent event;
        event.kind            = QSocAgentRuntimeEvent::Kind::RunAborted;
        d->terminalStopNotice = d->agent->takeStopNotice();
        event.text            = d->terminalStopNotice;
        event.at              = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });

    /* Tool traffic. */
    connect(
        d->agent,
        &QSocAgent::toolCallStarted,
        this,
        [this](const QString &callId, const QString &name, const QString &arguments) {
            QSocAgentRuntimeEvent event;
            event.kind      = QSocAgentRuntimeEvent::Kind::ToolStarted;
            event.callId    = callId;
            event.secondary = name;
            event.text      = arguments;
            event.at        = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });
    connect(
        d->agent,
        &QSocAgent::toolCallOutput,
        this,
        [this](const QString &callId, const QString &text) {
            QSocAgentRuntimeEvent event;
            event.kind   = QSocAgentRuntimeEvent::Kind::ToolOutput;
            event.callId = callId;
            event.text   = text;
            event.at     = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });
    connect(
        d->agent,
        &QSocAgent::toolCallFinished,
        this,
        [this](
            const QString       &callId,
            const QString       &name,
            const QString       &result,
            QSocToolResultStatus outcome) {
            QSocAgentRuntimeEvent event;
            event.kind      = QSocAgentRuntimeEvent::Kind::ToolFinished;
            event.callId    = callId;
            event.secondary = name;
            event.text      = result;
            event.ok        = outcome == QSocToolResultStatus::Ok;
            event.at        = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });

    /* Progress signals. */
    connect(d->agent, &QSocAgent::heartbeat, this, [this](int iteration, int elapsedSeconds) {
        QSocAgentRuntimeEvent event;
        event.kind           = QSocAgentRuntimeEvent::Kind::Heartbeat;
        event.iteration      = iteration;
        event.elapsedSeconds = elapsedSeconds;
        event.at             = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(
        d->agent,
        &QSocAgent::retrying,
        this,
        [this](int attempt, int maxAttempts, const QString &error) {
            QSocAgentRuntimeEvent event;
            event.kind        = QSocAgentRuntimeEvent::Kind::Retrying;
            event.attempt     = attempt;
            event.maxAttempts = maxAttempts;
            event.text        = error;
            event.at          = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });
    connect(d->agent, &QSocAgent::tokenUsage, this, [this](qint64 input, qint64 output) {
        QSocAgentRuntimeEvent event;
        event.kind         = QSocAgentRuntimeEvent::Kind::Tokens;
        event.inputTokens  = input;
        event.outputTokens = output;
        event.at           = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(
        d->agent, &QSocAgent::compacting, this, [this](int layer, int beforeTokens, int afterTokens) {
            QSocAgentRuntimeEvent event;
            event.kind         = QSocAgentRuntimeEvent::Kind::Compacting;
            event.layer        = layer;
            event.beforeTokens = beforeTokens;
            event.afterTokens  = afterTokens;
            event.at           = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });
    connect(d->agent, &QSocAgent::stuckDetected, this, [this](int, int silentSeconds) {
        QSocAgentRuntimeEvent event;
        event.kind           = QSocAgentRuntimeEvent::Kind::Stuck;
        event.elapsedSeconds = silentSeconds;
        event.at             = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::compactionFellBack, this, [this](const QString &reason) {
        emitOutput(
            QStringLiteral("Summary request failed (%1): used a mechanical summary.\n").arg(reason),
            static_cast<int>(QSocAgentRuntimeStyle::Dim));
    });
    connect(d->agent, &QSocAgent::tokenCountFellBack, this, [this](const QString &line) {
        emitOutput(line + QLatin1Char('\n'), static_cast<int>(QSocAgentRuntimeStyle::Dim));
    });
    connect(d->agent, &QSocAgent::contextRestored, this, [this]() {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::ContextRestored;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::processingQueuedRequest, this, [this](const QString &request, int) {
        const auto notice = d->agent->takeStopNotice();
        if (!notice.isEmpty())
            emitOutput("\n" + notice + "\n");
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::ProcessingQueued;
        event.text = request;
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    });

    /* Task notifications from the event queue. */
    if (d->taskEventQueue != nullptr) {
        connect(
            d->taskEventQueue,
            &QSocTaskEventQueue::taskNotificationReady,
            this,
            [this](const QString &message, const QString &agentId) {
                if (!agentId.isEmpty()) {
                    return;
                }
                if (d->agent->isRunning() && d->agent->queueTaskNotification(message)) {
                    return;
                }
                QSocAgentRuntimeEvent event;
                event.kind = QSocAgentRuntimeEvent::Kind::TaskNotification;
                event.text = message;
                event.at   = QDateTime::currentDateTimeUtc();
                emit eventRaised(event);
            });
    }

    /* Loop scheduler fires. */
    connect(
        d->loopScheduler,
        &QSocLoopScheduler::promptDue,
        this,
        [this](const QString &prompt, const QString &jobId) {
            d->pendingAutoInputs.append(prompt);
            QSocAgentRuntimeEvent event;
            event.kind      = QSocAgentRuntimeEvent::Kind::TaskNotification;
            event.secondary = jobId;
            event.text      = prompt;
            event.at        = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });

    /* Task registry changes. */
    if (d->taskRegistry != nullptr) {
        connect(d->taskRegistry, &QSocTaskRegistry::anySourceChanged, this, [this]() {
            QSocAgentRuntimeEvent event;
            event.kind = QSocAgentRuntimeEvent::Kind::TasksChanged;
            event.at   = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        });
    }

    /* Goal changes. */
    if (d->goalCatalog != nullptr) {
        connect(d->goalCatalog, &QSocGoalCatalog::goalChanged, this, [this]() {
            QSocAgentRuntimeEvent event;
            event.kind = QSocAgentRuntimeEvent::Kind::GoalChanged;
            event.at   = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
            emit statusChanged();
        });
    }

    /* ask_user + plan mode callbacks, bridged to the installed handlers. */
    if (auto *askTool = dynamic_cast<QSocToolAskUser *>(
            d->localRegistry->getTool(QStringLiteral("ask_user")))) {
        askTool->setCallback(
            [this](
                const QString                  &question,
                const QString                  &header,
                const QList<QSocAskUserOption> &options) -> QSocAskUserResult {
                QSocAskUserResult result;
                result.canceled = true;
                if (!d->askUser) {
                    return result;
                }
                return d->askUser(question, header, options);
            });
    }
    if (auto *enterPlanTool = dynamic_cast<QSocToolEnterPlanMode *>(
            d->localRegistry->getTool(QStringLiteral("enter_plan_mode")))) {
        enterPlanTool->setCallback([this]() { setPlanMode(true); });
    }
    if (auto *exitPlanTool = dynamic_cast<QSocToolExitPlanMode *>(
            d->localRegistry->getTool(QStringLiteral("exit_plan_mode")))) {
        exitPlanTool->setCallback([this](const QString &plan) -> QSocPlanApproval {
            QSocPlanApproval approval;
            if (!d->planApproval) {
                approval.feedback = QStringLiteral(
                    "plan approval is unavailable in this frontend; refine the approach");
                return approval;
            }
            d->planApprovalShownThisTurn = true;
            approval                     = d->planApproval(plan);
            if (approval.approved) {
                setPlanMode(false);
                d->agent->setApprovedPlan(plan.left(8000));
                const QString directory
                    = QDir(sessionProjectPath(d->projectManager)).filePath(".qsoc/plans");
                QDir().mkpath(directory);
                QFile file(QDir(directory).filePath(sessionId() + ".md"));
                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    file.write(plan.toUtf8());
            }
            return approval;
        });
    }

    /* User-watching probe. */
    d->agent->setUserWatchingProbe([this]() { return d->userWatching ? d->userWatching() : true; });
}

void QSocAgentRuntime::wirePersistence()
{
    if (d->agent == nullptr) {
        return;
    }

    connect(d->agent, &QSocAgent::runComplete, this, [this](const QString &) {
        d->observedTerminal = QSocSession::RunEvent::Completed;
    });
    connect(d->agent, &QSocAgent::runError, this, [this](const QString &) {
        d->observedTerminal = QSocSession::RunEvent::Error;
    });
    connect(d->agent, &QSocAgent::runAborted, this, [this](const QString &) {
        d->observedTerminal = QSocSession::RunEvent::Aborted;
    });

    d->agent->setCompactionCommitter([this](const QSocAgent::CompactionCandidate &candidate) {
        const auto previous    = d->memoryCursor;
        const int  replacement = static_cast<int>(candidate.candidateMessages.size());
        if (d->agent->getConfig().memoryExtractEnabled && d->agent->getMemoryManager())
            QSocMemoryExtractor::carryOver(d->memoryCursor, d->agent->getMessages(), replacement);
        else
            d->memoryCursor.index = replacement;
        // Save pending turns before replacing history so a crash cannot lose them.
        QSocMemoryExtractor::saveCursor(d->currentSession.get(), d->memoryCursor);
        if (!persistRecoverySnapshot(
                d->currentSession.get(),
                candidate.candidateMessages,
                d->persistedMessages,
                d->lastPersistedIndex)) {
            d->memoryCursor = previous;
            QSocMemoryExtractor::saveCursor(d->currentSession.get(), d->memoryCursor);
            return false;
        }
        return true;
    });

    d->agent->setPersistenceBarrier(
        [this](QSocAgent::PersistencePoint point, const QString &toolCallId) {
            if (d->activeRunId.isEmpty() || d->currentSession == nullptr) {
                return true;
            }

            QSocSession::RunEvent event = QSocSession::RunEvent::Invalid;
            switch (point) {
            case QSocAgent::PersistencePoint::BeforeRequest:
                event = QSocSession::RunEvent::Checkpoint;
                break;
            case QSocAgent::PersistencePoint::BeforeTool:
                event = QSocSession::RunEvent::ToolStarted;
                break;
            case QSocAgent::PersistencePoint::Completed:
                event = QSocSession::RunEvent::Completed;
                break;
            case QSocAgent::PersistencePoint::Error:
                event = QSocSession::RunEvent::Error;
                break;
            case QSocAgent::PersistencePoint::Aborted:
                event = QSocSession::RunEvent::Aborted;
                break;
            }

            const bool             terminal = event == QSocSession::RunEvent::Completed
                                              || event == QSocSession::RunEvent::Error
                                              || event == QSocSession::RunEvent::Aborted;
            QSocSession::RunRecord record{
                .runId      = d->activeRunId,
                .event      = event,
                .toolCallId = toolCallId,
            };
            d->applyRunContext(this, record);

            if (terminal && event != QSocSession::RunEvent::Completed) {
                if (!d->currentSession->appendRun(record)) {
                    return false;
                }
            }
            if (!persistSessionState(
                    d->agent, d->currentSession.get(), d->persistedMessages, d->lastPersistedIndex)) {
                return false;
            }
            if ((!terminal || event == QSocSession::RunEvent::Completed)
                && !d->currentSession->appendRun(record)) {
                return false;
            }
            if (terminal) {
                d->runTerminalPersisted = true;
                QSocSession::removeRecoveryClaim(d->activeRunId);
            }
            return true;
        });
}

QString QSocAgentRuntime::Private::skillListing() const
{
    const auto        audience = remoteConn->session() != nullptr
                                     ? QSocToolSkillFind::ListingAudience::UserOnly
                                     : QSocToolSkillFind::ListingAudience::Model;
    QSocToolSkillFind scanner(nullptr, projectManager);
    return QSocToolSkillFind::formatPromptListing(scanner.scanAllSkills(), audience);
}

void QSocAgentRuntime::Private::applyRunContext(
    QSocAgentRuntime *runtime, QSocSession::RunRecord &record)
{
    const QSocAgentConfig config = agent->getConfig();
    /* Read through the owner, not through the config's copy of it: a `path`
     * tool call changes the directory mid-turn and the record has to name
     * the one the session is actually in. */
    QString workingDir = config.remoteMode ? remoteConn->path()->cwd()
                                           : (pathContext != nullptr ? pathContext->getWorkingDir()
                                                                     : QDir::currentPath());
    if (workingDir.isEmpty()) {
        workingDir = config.remoteMode ? QStringLiteral("/") : QDir::currentPath();
    }
    record.contextPresent   = true;
    record.modelId          = llmService->getCurrentModelId();
    record.effortLevel      = config.effortLevel;
    record.toolPresentation = config.toolPresentation;
    record.planMode         = config.planMode;
    record.remoteMode       = config.remoteMode;
    record.remoteName       = config.remoteName;
    if (config.remoteMode) {
        record.projectRoot = remoteConn->path()->root();
        record.workingDir  = workingDir;
    } else {
        const QFileInfo rootInfo(sessionProjectPath(projectManager));
        const QFileInfo workingInfo(workingDir);
        record.projectRoot = rootInfo.canonicalFilePath();
        record.workingDir  = workingInfo.canonicalFilePath();
        if (record.projectRoot.isEmpty()) {
            record.projectRoot = rootInfo.absoluteFilePath();
        }
        if (record.workingDir.isEmpty()) {
            record.workingDir = workingInfo.absoluteFilePath();
        }
    }
    Q_UNUSED(runtime);
}

void QSocAgentRuntime::Private::installSessionWriteBarrier(
    QSocAgentRuntime *runtime, QSocSession *session, QSocFileHistory *history)
{
    if (session == nullptr) {
        return;
    }
    const QString sessionPath = session->filePath();
    session->setWriteBarrier([this, runtime, session, sessionPath, history]() {
        if (currentSession.get() != session || (history != nullptr && !history->storageIsBound())) {
            return false;
        }
        if (sessionLock) {
            if (sessionLockPath != sessionPath)
                return false;
        } else {
            auto nextLock = lockSession(sessionPath);
            if (!nextLock)
                return false;
            /* The binding is read from disk again, now under the lock. */
            // cppcheck-suppress knownConditionTrueFalse
            if (history != nullptr && !history->storageIsBound()) {
                nextLock->unlock();
                return false;
            }
            sessionLock     = std::move(nextLock);
            sessionLockPath = sessionPath;
        }
        const QString directory = sessionPath + QStringLiteral(".artifacts");
        auto          store     = agent->toolResultStore();
        if (!store) {
            if (!agent->bindToolResultStore(directory, session->id()))
                return false;
            store = agent->toolResultStore();
        }
        return (history == nullptr || history->storageIsBound()) && store->owner() == session->id()
               && store->directory() == QFileInfo(directory).canonicalFilePath()
               && !QFileInfo(directory).isSymLink() && store->isBound();
    });
}

void QSocAgentRuntime::connectLocalWorkspace(const QString &workspacePath)
{
    if (workspacePath.isEmpty() || !workspacePath.startsWith(QLatin1Char('/'))) {
        emitOutput(
            QStringLiteral("--workspace must be an absolute local path; ignoring:") + workspacePath,
            static_cast<int>(QSocAgentRuntimeStyle::Warning));
        return;
    }
    const QFileInfo info(workspacePath);
    if (!info.exists() || !info.isDir()) {
        emitOutput(
            QStringLiteral("--workspace path does not exist or is not a directory; ignoring:")
                + workspacePath,
            static_cast<int>(QSocAgentRuntimeStyle::Warning));
        return;
    }
    QString error;
    if (!setWorkingDirectory(workspacePath, &error)) {
        d->lastErrorText = error;
    }
}

bool QSocAgentRuntime::openSession()
{
    QString sessionId = d->options.resumeSessionId;
    if (d->options.continueLatestSession) {
        const QString projectPath = sessionProjectPath(d->projectManager);
        const auto    sessions    = QSocSession::listAll(projectPath);
        if (!sessions.isEmpty()) {
            sessionId = sessions.first().id;
        }
    }
    if (sessionId == QStringLiteral("-")) {
        /* Sentinel: the frontend picks from listSessions() and calls
         * openSessionById(). Until then, a fresh session. */
        sessionId.clear();
    }
    if (!sessionId.isEmpty())
        return openSessionById(sessionId);
    return openSessionInternal(QString(), true);
}

bool QSocAgentRuntime::openSessionById(const QString &sessionId)
{
    const QString projectPath = sessionProjectPath(d->projectManager);
    const QString resolved    = QSocSession::resolveId(projectPath, sessionId);
    if (resolved.isEmpty()) {
        d->lastErrorText = QStringLiteral("no session matches '%1'").arg(sessionId);
        return false;
    }
    if (d->currentSession && d->currentSession->id() == resolved)
        return true;
    if (isRunning() || (d->subAgentTaskSource && d->subAgentTaskSource->hasUnsettledRun())) {
        d->lastErrorText = QStringLiteral("Resume refused: a turn or sub-agent is still running.");
        return false;
    }
    if (d->currentSession) {
        if (!d->currentFileHistory || !d->currentFileHistory->storageIsBound()
            || QFileInfo(d->currentSession->filePath()).isSymLink()) {
            d->lastErrorText = QStringLiteral(
                "Resume refused: the project storage binding changed.");
            return false;
        }
        // Blocked history is intentionally absent from the agent, so saving it
        // here would replace the original transcript with an empty one.
        if (!d->historyInputBlocked && !persistNow()) {
            d->lastErrorText = QStringLiteral("Session persistence failed; session unchanged.");
            return false;
        }
    }
    return openSessionInternal(resolved, false);
}

bool QSocAgentRuntime::openSessionInternal(const QString &sessionId, bool fresh)
{
    const QString projectPath = sessionProjectPath(d->projectManager);
    QString       id          = sessionId;
    if (fresh) {
        id = QSocSession::generateId();
    }
    const QString sessionPath = QDir(QSocSession::sessionsDir(projectPath)).filePath(id + ".jsonl");
    const bool    sessionExists = existingSessionPathIsRegular(sessionPath);
    if (fresh && !freshSessionPathAvailable(sessionPath)) {
        d->lastErrorText = QStringLiteral("could not prepare a fresh session path: %1").arg(id);
        return false;
    }
    if (!fresh && !sessionExists) {
        d->lastErrorText = QStringLiteral("session is not a regular file: %1").arg(id);
        return false;
    }
    if (d->currentSession && d->currentSession->id() == id)
        return true;
    auto history = std::make_unique<QSocFileHistory>(projectPath, id);
    if (!history->storageIsBound()) {
        d->lastErrorText
            = QStringLiteral("project storage binding is unsafe for session: %1").arg(id);
        return false;
    }
    std::unique_ptr<QLockFile> nextLock;
    if (!fresh) {
        nextLock = lockSession(sessionPath);
        if (!nextLock
            || !existingSessionPathIsRegular(sessionPath)
            /* The binding is read from disk again, now under the lock. */
            // cppcheck-suppress knownConditionTrueFalse
            || !history->storageIsBound()) {
            d->lastErrorText = QStringLiteral("session could not be locked safely: %1").arg(id);
            return false;
        }
        if (!d->agent->bindToolResultStore(sessionPath + ".artifacts", id)) {
            d->lastErrorText = QStringLiteral(
                "tool result storage could not be bound to the session.");
            return false;
        }
    }
    auto session = std::make_unique<QSocSession>(
        id,
        sessionPath,
        fresh ? QSocSession::StorageMode::Fresh : QSocSession::StorageMode::Existing);
    d->sessionLock       = std::move(nextLock);
    d->sessionLockPath   = fresh ? QString() : sessionPath;
    d->titleGenerated    = false;
    d->memoryCapNotified = false;
    d->memoryCursor      = {};
    d->pendingAutoInputs.clear();
    d->agent->clearPendingRequests();
    if (d->pathContext)
        d->pathContext->readState().clear();
    d->invokedSkills.clear();
    d->skillSeq = 1;
    d->agent->setApprovedPlan({});
    d->currentSession = std::move(session);
    d->agent->bindSessionIdentity(d->currentSession->id());
    d->currentFileHistory = std::move(history);
    d->lastErrorText.clear();
    d->activeRunId.clear();
    d->recoveryRun.reset();
    d->recoveryRequiresInput = false;
    d->installSessionWriteBarrier(this, d->currentSession.get(), d->currentFileHistory.get());
    wireSessionTools();
    if (fresh) {
        d->agent->unbindToolResultStore();
        d->persistedMessages   = json::array();
        d->lastPersistedIndex  = 0;
        d->memoryCursor        = {};
        d->turnCounter         = 0;
        d->historyInputBlocked = false;
        d->agent->clearHistory();
    }

    if (sessionExists) {
        const json restored = QSocSession::loadMessages(sessionPath);
        if (restored.is_array()) {
            d->persistedMessages  = restored;
            d->lastPersistedIndex = static_cast<int>(restored.size());
            if (d->subAgentTaskSource)
                d->subAgentTaskSource->reserveIdsFrom(restored);
            const bool baselineSafe = QSocSessionRecovery::historySafeForNewTurn(restored);
            d->agent->setMessages(baselineSafe ? restored : json::array());
            d->historyInputBlocked = !baselineSafe;
            d->memoryCursor = QSocMemoryExtractor::loadCursor(sessionPath, d->lastPersistedIndex);
            d->turnCounter  = d->currentFileHistory->latestTurn();
            int persistedUserTurns = 0;
            for (const auto &msg : d->persistedMessages) {
                if (QSocMessageAuthority::isUserRequest(msg)) {
                    persistedUserTurns++;
                }
            }
            d->turnCounter = qMax(d->turnCounter, persistedUserTurns);

            /* Restore an approved plan. */
            const QString planFile = QDir(QDir(projectPath).filePath(QStringLiteral(".qsoc/plans")))
                                         .filePath(id + QStringLiteral(".md"));
            QFile         planIn(planFile);
            if (planIn.exists() && planIn.open(QIODevice::ReadOnly)) {
                QString plan = QString::fromUtf8(planIn.readAll());
                planIn.close();
                static constexpr int kPlanCharCap = 8000;
                if (plan.size() > kPlanCharCap) {
                    plan = plan.left(kPlanCharCap)
                           + QStringLiteral("\n...[truncated; full plan at %1]").arg(planFile);
                }
                d->agent->setApprovedPlan(plan);
            }

            QSocAgentRuntimeEvent event;
            event.kind         = QSocAgentRuntimeEvent::Kind::SessionResumed;
            event.secondary    = id;
            event.messageCount = static_cast<int>(restored.size());
            event.text         = QStringLiteral("(Resumed session %1, %2 messages)\n")
                                     .arg(id.left(8))
                                     .arg(restored.size());
            event.at           = QDateTime::currentDateTimeUtc();
            emit eventRaised(event);
        }
    } else {
        d->currentSession->appendMeta(
            QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        d->currentSession->appendMeta(QStringLiteral("cwd"), projectPath);
        QSocAgentRuntimeEvent event;
        event.kind      = QSocAgentRuntimeEvent::Kind::SessionStarted;
        event.secondary = id;
        event.text      = QStringLiteral("(New session %1)\n").arg(id.left(8));
        event.at        = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    }

    if (sessionExists)
        prepareRecovery();
    emit sessionChanged(id);
    return true;
}

QList<QSocAgentSessionInfo> QSocAgentRuntime::listSessions() const
{
    QList<QSocAgentSessionInfo> result;
    const QString               projectPath = sessionProjectPath(d->projectManager);
    for (const QSocSession::Info &info : QSocSession::listAll(projectPath)) {
        QSocAgentSessionInfo row;
        row.id           = info.id;
        row.path         = info.path;
        row.createdAt    = info.createdAt;
        row.lastModified = info.lastModified;
        row.firstPrompt  = info.firstPrompt;
        row.title        = info.title;
        row.branch       = info.branch;
        row.messageCount = info.messageCount;
        result.append(row);
    }
    return result;
}

bool QSocAgentRuntime::clearSession()
{
    if (!d->currentFileHistory || !d->currentFileHistory->storageIsBound()
        || (d->currentSession && QFileInfo(d->currentSession->filePath()).isSymLink())) {
        emitOutput(QStringLiteral("History clear refused: the project storage binding changed.\n"));
        return false;
    }
    if (auto *spawnTool = dynamic_cast<QSocToolAgent *>(
            d->localRegistry->getTool(QStringLiteral("agent")));
        spawnTool != nullptr && spawnTool->taskSource() != nullptr
        && spawnTool->taskSource()->hasUnsettledRun()) {
        emitOutput(
            QStringLiteral("History clear refused: a sub-agent is still pending or running.\n"));
        return false;
    }
    const QString projectPath = sessionProjectPath(d->projectManager);
    const QString nextId      = QSocSession::generateId();
    const QString sessionPath
        = QDir(QSocSession::sessionsDir(projectPath)).filePath(nextId + ".jsonl");
    if (!freshSessionPathAvailable(sessionPath)) {
        emitOutput(
            QStringLiteral("History clear refused: a fresh session could not be prepared.\n"));
        return false;
    }
    auto nextSession
        = std::make_unique<QSocSession>(nextId, sessionPath, QSocSession::StorageMode::Fresh);
    auto nextHistory = std::make_unique<QSocFileHistory>(projectPath, nextId);
    if (!nextHistory->storageIsBound()) {
        emitOutput(
            QStringLiteral("History clear refused: a fresh session could not be prepared.\n"));
        return false;
    }
    d->installSessionWriteBarrier(this, nextSession.get(), nextHistory.get());
    if (!nextSession->appendMeta(
            QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
        || !nextSession->appendMeta(QStringLiteral("cwd"), projectPath)) {
        emitOutput(
            QStringLiteral("History clear refused: a fresh session could not be prepared.\n"));
        return false;
    }

    d->sessionLock.reset();
    d->sessionLockPath.clear();
    d->currentSession = std::move(nextSession);
    d->agent->bindSessionIdentity(d->currentSession->id());
    d->agent->unbindToolResultStore();
    d->currentFileHistory  = std::move(nextHistory);
    d->persistedMessages   = json::array();
    d->lastPersistedIndex  = 0;
    d->memoryCursor        = {};
    d->turnCounter         = 0;
    d->historyInputBlocked = false;
    d->activeRunId.clear();
    d->recoveryRun.reset();
    d->recoveryRequiresInput = false;
    d->pendingAutoInputs.clear();
    d->titleGenerated = false;
    clearHistory();
    wireSessionTools();
    if (d->pathContext) {
        d->pathContext->readState().clear();
    }

    QSocAgentRuntimeEvent event;
    event.kind      = QSocAgentRuntimeEvent::Kind::SessionCleared;
    event.secondary = nextId;
    event.text      = QStringLiteral("History cleared.\n");
    event.at        = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    emit sessionChanged(nextId);
    return true;
}

bool QSocAgentRuntime::persistNow()
{
    if (!d->currentSession) {
        return true;
    }
    const bool ok = persistSessionState(
        d->agent, d->currentSession.get(), d->persistedMessages, d->lastPersistedIndex);
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::SessionPersisted;
    event.ok   = ok;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    return ok;
}

QSocAgentTurnResult QSocAgentRuntime::runTurn(const QString &input)
{
    QSocAgentTurnResult result;
    if (isRunning()) {
        result.error     = true;
        result.errorText = QStringLiteral("a turn is already running; queue the input instead");
        return result;
    }
    if (d->historyInputBlocked) {
        result.error     = true;
        result.errorText = QStringLiteral(
            "session history cannot accept a new turn. Use /clear or exit.");
        return result;
    }

    d->recoveryRun.reset();
    d->terminalStopNotice.clear();
    emitStatus(QStringLiteral("Reasoning"));
    d->cancelRequested           = false;
    d->recoveryRequiresInput     = false;
    d->observedTerminal          = QSocSession::RunEvent::Invalid;
    d->runTerminalPersisted      = false;
    d->planApprovalShownThisTurn = false;

    /* Run record + recovery claim. */
    if (d->activeRunId.isEmpty()) {
        d->activeRunId = QSocSession::generateId();
        if (!d->persistedMessages.is_array()
            || d->persistedMessages.size()
                   > static_cast<json::size_type>(std::numeric_limits<int>::max())) {
            result.error     = true;
            result.errorText = QStringLiteral("session history is too large to resume safely.");
            d->activeRunId.clear();
            return result;
        }
        const bool recoveryClaimCreated = QSocSession::createRecoveryClaim(d->activeRunId);
        if (!recoveryClaimCreated) {
            emitOutput(
                QStringLiteral("Crash recovery is unavailable for this turn.\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
        QString activeGoalId;
        if (d->goalCatalog != nullptr) {
            const auto goal = d->goalCatalog->current();
            if (goal.has_value() && goal->status == QSocGoalStatus::Active) {
                activeGoalId = goal->id;
            }
        }
        const QSocHookConfig hooks = d->agent->getConfig().hooks;
        const bool inputReplaySafe = hooks.matchersFor(QSocHookEvent::SessionStart).isEmpty()
                                     && hooks.matchersFor(QSocHookEvent::UserPromptSubmit).isEmpty();
        QSocSession::RunRecord started{
            .runId           = d->activeRunId,
            .event           = QSocSession::RunEvent::Started,
            .input           = input,
            .goalId          = activeGoalId,
            .messageCount    = static_cast<int>(d->persistedMessages.size()),
            .historyDigest   = QSocSession::historyDigest(d->persistedMessages),
            .inputReplaySafe = inputReplaySafe,
        };
        d->applyRunContext(this, started);
        if (d->currentSession == nullptr || !d->currentSession->appendRun(started)) {
            QSocSession::removeRecoveryClaim(d->activeRunId);
            result.error     = true;
            result.errorText = QStringLiteral("session persistence failed; request not started.");
            d->activeRunId.clear();
            return result;
        }
    }

    QEventLoop loop;
    bool       running = true;
    QString    finalText;
    QString    errorText;
    QString    stopNotice;
    bool       errored = false;
    bool       aborted = false;

    auto connComplete = connect(d->agent, &QSocAgent::runComplete, &loop, [&](const QString &final) {
        finalText = final;
        if (running)
            loop.quit();
    });
    auto connError   = connect(d->agent, &QSocAgent::runError, &loop, [&](const QString &error) {
        errored   = true;
        errorText = error;
        if (running)
            loop.quit();
    });
    auto connAborted = connect(d->agent, &QSocAgent::runAborted, &loop, [&](const QString &) {
        aborted    = true;
        stopNotice = d->terminalStopNotice.isEmpty() ? d->agent->takeStopNotice()
                                                     : d->terminalStopNotice;
        if (running)
            loop.quit();
    });

    if (d->options.streaming || d->resumeHistory) {
        if (d->resumeHistory)
            d->agent->resumeStream();
        else
            d->agent->runStream(input);
        if (d->agent->isRunning()) {
            loop.exec();
        }
    } else {
        /* Non-streaming: the synchronous API runs a nested event loop, so
         * the terminal signals above still fire; Ctrl-C edges arrive on the
         * interrupt pipe and must be drained into abort() calls, exactly
         * like the old REPL did around agent->run(). */
        bool                             interruptSeen = false;
        std::unique_ptr<QSocketNotifier> interruptNotifier;
        if (QSocInterrupt::handlerReady()) {
            interruptNotifier = std::make_unique<QSocketNotifier>(
                QSocInterrupt::signalReadFd(), QSocketNotifier::Read);
            QObject::connect(
                interruptNotifier.get(),
                &QSocketNotifier::activated,
                interruptNotifier.get(),
                [this, &interruptSeen]() {
                    const int edges = QSocInterrupt::drainSignalPipe();
                    if (edges < 0) {
                        d->agent->abort();
                        return;
                    }
                    for (int edge = 0; edge < edges; ++edge) {
                        interruptSeen = true;
                        d->agent->abort();
                    }
                });
        }
        finalText = d->agent->run(input);
        if (interruptSeen || d->cancelRequested) {
            /* The synchronous run reports an interrupt as an error string;
             * the caller needs to see it as an abort. */
            errored = false;
            aborted = true;
            errorText.clear();
            stopNotice = d->terminalStopNotice.isEmpty() ? d->agent->takeStopNotice()
                                                         : d->terminalStopNotice;
        }
    }
    running = false;
    QObject::disconnect(connComplete);
    QObject::disconnect(connError);
    QObject::disconnect(connAborted);

    if (!errored && !aborted && planMode() && !d->planApprovalShownThisTurn
        && !finalText.trimmed().isEmpty()) {
        if (auto *tool = d->localRegistry->getTool("exit_plan_mode"))
            tool->execute({{"plan", finalText.toStdString()}});
        /* An approved exit_plan_mode turns plan mode off. */
        // cppcheck-suppress knownConditionTrueFalse
        if (!planMode())
            d->pendingAutoInputs.append("I approved the plan above. Execute it now.");
    }

    /* Terminal persistence. */
    bool saved = true;
    if (!d->activeRunId.isEmpty() && d->currentSession != nullptr) {
        if (!d->runTerminalPersisted && d->observedTerminal != QSocSession::RunEvent::Invalid) {
            QSocSession::RunRecord record{
                .runId = d->activeRunId,
                .event = d->observedTerminal,
            };
            d->applyRunContext(this, record);
            if (d->observedTerminal == QSocSession::RunEvent::Completed) {
                saved = persistSessionState(
                            d->agent,
                            d->currentSession.get(),
                            d->persistedMessages,
                            d->lastPersistedIndex)
                        && d->currentSession->appendRun(record);
            } else {
                saved = d->currentSession->appendRun(record)
                        && persistSessionState(
                            d->agent,
                            d->currentSession.get(),
                            d->persistedMessages,
                            d->lastPersistedIndex);
            }
        }
        QSocSession::removeRecoveryClaim(d->activeRunId);
        d->activeRunId.clear();
    }
    d->observedTerminal     = QSocSession::RunEvent::Invalid;
    d->runTerminalPersisted = false;

    /* File-history checkpoint for the turn. */
    if (d->currentFileHistory) {
        if (!d->resumeHistory)
            d->turnCounter++;
        d->currentFileHistory->makeSnapshot(d->turnCounter);
    }

    /* Idle compaction. */
    {
        const qint64 tokens    = d->agent->contextEstimate().upper();
        const qint64 threshold = static_cast<qint64>(
            d->agent->effectiveContextTokens() * d->agent->getConfig().compactThreshold);
        if (tokens > threshold) {
            compactNow();
        }
    }

    /* Context usage event. */
    {
        QSocAgentRuntimeEvent event;
        fillContextUsage(event);
        event.at = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
    }

    result.finalText   = finalText;
    result.error       = errored;
    result.aborted     = aborted;
    result.errorText   = errorText;
    result.stopNotice  = stopNotice;
    result.persistedOk = saved;
    return result;
}

bool QSocAgentRuntime::isRunning() const
{
    return d->agent && d->agent->isRunning();
}

void QSocAgentRuntime::abort()
{
    d->cancelRequested = true;
    d->commandStop.request_stop();
    if (d->maintenanceChild)
        d->maintenanceChild->abort();
    if (d->agent) {
        d->agent->abort();
    }
}

bool QSocAgentRuntime::queueRequest(const QString &text)
{
    return isRunning() && d->agent->queueRequest(text);
}

int QSocAgentRuntime::compactNow()
{
    emitStatus(QStringLiteral("Compacting"));
    const int             saved = d->agent->compact();
    QSocAgentRuntimeEvent event;
    event.kind        = QSocAgentRuntimeEvent::Kind::Compacted;
    event.savedTokens = saved;
    event.at          = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    switch (lastCompactionStatus()) {
    case CompactionStatus::Committed:
        emitOutput(QStringLiteral("Compacted: saved ~%1 tokens.\n").arg(saved));
        break;
    case CompactionStatus::NoProgress:
        emitOutput(QStringLiteral("Nothing to compact.\n"));
        break;
    case CompactionStatus::Cancelled:
        emitOutput(QStringLiteral("Compaction cancelled. The history is unchanged.\n"));
        break;
    case CompactionStatus::Failed:
        emitOutput(QStringLiteral("Compaction failed. The history is unchanged.\n"));
        break;
    }
    fillContextUsage(event);
    emit eventRaised(event);
    emitStatus(QStringLiteral("Ready"));
    return saved;
}

QSocAgentRuntime::CompactionStatus QSocAgentRuntime::lastCompactionStatus() const
{
    switch (d->agent->lastCompactionStatus()) {
    case QSocAgent::CompactionStatus::Committed:
        return CompactionStatus::Committed;
    case QSocAgent::CompactionStatus::NoProgress:
        return CompactionStatus::NoProgress;
    case QSocAgent::CompactionStatus::Cancelled:
        return CompactionStatus::Cancelled;
    case QSocAgent::CompactionStatus::Failed:
        return CompactionStatus::Failed;
    }
    return CompactionStatus::Failed;
}

int QSocAgentRuntime::estimateTotalTokens() const
{
    return d->agent->estimateTotalTokens();
}

int QSocAgentRuntime::effectiveContextTokens() const
{
    return d->agent->effectiveContextTokens();
}

nlohmann::json QSocAgentRuntime::messages() const
{
    return d->agent->getMessages();
}

void QSocAgentRuntime::setMessages(const nlohmann::json &msgs)
{
    d->agent->setMessages(msgs);
}

void QSocAgentRuntime::clearHistory()
{
    if (!d->agent) {
        return;
    }
    d->agent->clearPendingRequests();
    d->agent->clearHistory();
    d->persistedMessages  = json::array();
    d->lastPersistedIndex = 0;
}

QStringList QSocAgentRuntime::availableCommands() const
{
    QStringList result;
    for (const auto &command : kRuntimeCommands)
        result.append(command);
    result.append("/exit");
    for (const auto &skill : QSocToolSkillFind(nullptr, d->projectManager).scanAllSkills())
        if (skill.userInvocable && !result.contains("/" + skill.name))
            result.append("/" + skill.name);
    return result;
}

bool QSocAgentRuntime::handlesCommand(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.startsWith(QLatin1Char('!')) || trimmed.startsWith(QLatin1Char('#'))) {
        return true;
    }
    if (!trimmed.startsWith(QLatin1Char('/'))) {
        return false;
    }
    const int     spaceIdx = trimmed.indexOf(QLatin1Char(' '));
    const QString cmd      = spaceIdx > 0 ? trimmed.left(spaceIdx) : trimmed;
    return isRuntimeCommand(cmd.toLower());
}

void QSocAgentRuntime::emitOutput(const QString &text, int style)
{
    QSocAgentRuntimeEvent event;
    event.kind  = QSocAgentRuntimeEvent::Kind::Output;
    event.text  = text;
    event.style = static_cast<QSocAgentRuntimeStyle>(style);
    event.at    = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
}

void QSocAgentRuntime::emitStatus(const QString &status)
{
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::Status;
    event.text = status;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
}

/* ---------------------------------------------------------------------- */
/* Idle maintenance + pending inputs. */

void QSocAgentRuntime::finishTurnMaintenance()
{
    if (d->agent == nullptr || d->agent->getMemoryManager() == nullptr) {
        return;
    }

    d->commandStop     = std::stop_source();
    d->cancelRequested = false;
    emitStatus(QStringLiteral("Saving memory"));

    /* Background extraction. */
    if (QSocMemoryExtractor(d->agent, d->agent->getMemoryManager(), d->agent->getLLMService())
            .extract(d->memoryCursor, d->turnCounter, [this](QSocAgent *child) {
                d->maintenanceChild = child;
            })) {
        QSocMemoryExtractor::saveCursor(d->currentSession.get(), d->memoryCursor);
    }

    if (d->cancelRequested) {
        emitStatus("Ready");
        return;
    }
    /* One-shot auto session title. */
    maybeGenerateSessionTitle();

    /* Deferred away recap. */
    if (d->awaySummaryPending) {
        maybeGenerateAwaySummary();
    }

    /* Once-per-process dream. */
    if (!d->dreamAttempted) {
        d->dreamAttempted          = true;
        const QString dreamSession = d->currentSession ? d->currentSession->id() : QString();
        const auto    dreamOutcome
            = QSocMemoryDream(d->agent, d->agent->getMemoryManager(), d->agent->getLLMService())
                  .maybeRun(d->agent->getConfig().projectPath, dreamSession, [this](QSocAgent *child) {
                      d->maintenanceChild = child;
                  });
        if (dreamOutcome.ran) {
            emitOutput(
                QStringLiteral("(memory consolidated: %1 -> %2 topics)\n")
                    .arg(dreamOutcome.before)
                    .arg(dreamOutcome.after),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
    }

    /* Near-capacity notice, once. */
    if (!d->memoryCapNotified && d->agent->getMemoryManager() != nullptr) {
        const int cap   = QSocMemoryManager::maxTopicFiles();
        const int count = d->agent->getMemoryManager()->topicFileCount("all");
        if (count >= cap * 9 / 10) {
            d->memoryCapNotified = true;
            emitOutput(
                QStringLiteral(
                    "(memory near capacity: %1/%2 topics; the oldest stop being "
                    "recalled past the cap. Prune via /memory or let it consolidate)\n")
                    .arg(count)
                    .arg(cap),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
    }

    emitStatus(QStringLiteral("Ready"));
}

void QSocAgentRuntime::maybeGenerateSessionTitle()
{
    const QSocAgentConfig &cfg = d->agent->getConfig();
    if (d->titleGenerated || !cfg.sessionTitleEnabled || !d->currentSession || d->turnCounter < 1) {
        return;
    }
    const QString path = d->currentSession->filePath();
    if (!QSocSession::readMeta(path, QStringLiteral("title")).isEmpty()
        || !QSocSession::readMeta(path, QStringLiteral("auto_title")).isEmpty()) {
        d->titleGenerated = true; /* already titled */
        return;
    }
    QString    firstPrompt;
    const json msgs = d->agent->getMessages();
    if (msgs.is_array()) {
        for (const auto &msg : msgs) {
            if (QSocMessageAuthority::isUserRequest(msg) && msg.contains("content")
                && msg["content"].is_string()) {
                firstPrompt = QString::fromStdString(msg["content"].get<std::string>());
                break;
            }
        }
    }
    QLLMService *llm = d->agent->getLLMService();
    if (firstPrompt.trimmed().isEmpty() || llm == nullptr || !llm->hasEndpoint()) {
        return;
    }
    d->titleGenerated = true; /* one attempt per process regardless of outcome */
    emitStatus(QStringLiteral("Naming session"));

    json titleMsgs = json::array();
    titleMsgs.push_back(
        {{"role", "system"}, {"content", QSocSessionTitle::systemPrompt().toStdString()}});
    titleMsgs.push_back(
        {{"role", "user"},
         {"content", QSocSessionTitle::buildUserMessage(firstPrompt.left(2000)).toStdString()}});

    LLMModelConfig endpoint = llm->getCurrentModelConfig();
    if (!cfg.sessionTitleModel.isEmpty()) {
        if (!llm->availableModels().contains(cfg.sessionTitleModel)) {
            emitStatus(QStringLiteral("Ready"));
            QSocConsole::warn() << "Unknown title model:" << cfg.sessionTitleModel;
            return;
        }
        endpoint = llm->getModelConfig(cfg.sessionTitleModel);
    }
    const json resp = llm->sendChatCompletionTo(
        endpoint, titleMsgs, json::array(), 0.3, d->commandStop.get_token(), cfg.effortLevel);
    emitStatus(QStringLiteral("Ready"));

    QString raw;
    if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty()) {
        const auto &message = resp["choices"][0]["message"];
        if (message.contains("content") && message["content"].is_string()) {
            raw = QString::fromStdString(message["content"].get<std::string>());
        }
    }
    const QString title = QSocSessionTitle::sanitize(raw);
    if (!title.isEmpty()) {
        d->currentSession->appendMeta(QStringLiteral("auto_title"), title);
    }
}

void QSocAgentRuntime::maybeGenerateAwaySummary()
{
    const QSocAgentConfig &cfg = d->agent->getConfig();
    if (!cfg.awaySummaryEnabled || d->awaySummaryShown || (d->userWatching && d->userWatching())) {
        return;
    }
    if (d->agent->isRunning()) {
        d->awaySummaryPending = true; /* flushed when the turn settles */
        return;
    }
    d->awaySummaryPending = false;
    QLLMService *llm      = d->agent->getLLMService();
    if (llm == nullptr || !llm->hasEndpoint()) {
        return;
    }
    const json msgs  = d->agent->getMessages();
    const int  count = msgs.is_array() ? static_cast<int>(msgs.size()) : 0;
    if (count == 0) {
        return;
    }
    const int     start      = qMax(0, count - 30);
    const QString transcript = d->agent->formatMessagesForSummary(start, count);
    if (transcript.trimmed().isEmpty()) {
        return;
    }
    d->awaySummaryShown = true; /* one recap per away period regardless of outcome */
    emitStatus(QStringLiteral("Recapping"));

    json sumMsgs = json::array();
    sumMsgs.push_back(
        {{"role", "system"}, {"content", QSocAwaySummary::systemPrompt().toStdString()}});
    sumMsgs.push_back(
        {{"role", "user"},
         {"content", QSocAwaySummary::buildUserMessage(transcript).toStdString()}});

    LLMModelConfig endpoint = llm->getCurrentModelConfig();
    if (!cfg.awaySummaryModel.isEmpty()) {
        if (!llm->availableModels().contains(cfg.awaySummaryModel)) {
            emitStatus(QStringLiteral("Ready"));
            QSocConsole::warn() << "Unknown away summary model:" << cfg.awaySummaryModel;
            return;
        }
        endpoint = llm->getModelConfig(cfg.awaySummaryModel);
    }
    const json resp = llm->sendChatCompletionTo(
        endpoint, sumMsgs, json::array(), 0.3, d->commandStop.get_token(), cfg.effortLevel);
    emitStatus(QStringLiteral("Ready"));

    QString raw;
    if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty()) {
        const auto &message = resp["choices"][0]["message"];
        if (message.contains("content") && message["content"].is_string()) {
            raw = QString::fromStdString(message["content"].get<std::string>());
        }
    }
    const QString recap = QSocAwaySummary::sanitize(raw);
    if (recap.isEmpty()) {
        return;
    }
    emitOutput(QStringLiteral("※ %1\n").arg(recap), static_cast<int>(QSocAgentRuntimeStyle::Dim));
}

void QSocAgentRuntime::noteInvokedSkill(const QString &name)
{
    d->invokedSkills.insert(name, d->skillSeq++);
}

QStringList QSocAgentRuntime::takePendingAutoInputs()
{
    QStringList taken = d->pendingAutoInputs;
    d->pendingAutoInputs.clear();
    return taken;
}

bool QSocAgentRuntime::hasPendingAutoInputs() const
{
    return !d->recoveryRequiresInput && !d->historyInputBlocked && !d->pendingAutoInputs.isEmpty();
}

#include "moc_qsocagentruntime.cpp"
