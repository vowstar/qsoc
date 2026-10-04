// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentruntime.h"
#include "agent/runtime/qsocagentruntime_p.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocgoal.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocmemoryrecall.h"
#include "agent/qsocsession.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/services/qsocloopscheduler.h"
#include "agent/tool/qsoctoolagent.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolmemory.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "agent/tool/qsoctoolshell.h"
#include "agent/tool/qsoctoolskill.h"
#include "common/config.h"
#include "common/qllmservice.h"
#include "common/qlspservice.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qsocconsole.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocinterrupt.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocpaths.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocproxy.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <limits>

using json = nlohmann::json;

using QSocAgentRuntimeInternal::existingSessionPathIsRegular;
using QSocAgentRuntimeInternal::freshSessionPathAvailable;
using QSocAgentRuntimeInternal::lockSession;
using QSocAgentRuntimeInternal::persistRecoverySnapshot;
using QSocAgentRuntimeInternal::persistSessionState;
using QSocAgentRuntimeInternal::sessionProjectPath;

namespace {
/* Brief the agent after a reconnect. It must not read as "carry on": the
 * link is back, but nothing about the host has been observed since it broke.
 * Naming the in-flight jobs and the unverified operation is what turns a
 * guess into a check, and the read-before-overwrite guard makes the check
 * mandatory for any edit. */
QString reobservationBriefing(QSocRemoteConnection *conn, const QStringList &jobIds)
{
    QString text = QStringLiteral(
                       "The SSH link to %1 dropped and has been re-established. Nothing on "
                       "the host has been observed since it broke, so treat every belief "
                       "about remote state as stale.\n\n")
                       .arg(conn->target());
    text += QStringLiteral(
        "Before acting:\n"
        "- Re-read any file you intend to edit. write_file and edit_file will refuse "
        "until you do.\n");
    if (!jobIds.isEmpty()) {
        text += QStringLiteral(
                    "- Check these background jobs with bash_manage(action=status): %1. They "
                    "may still be running, may have finished, or may be gone if the host "
                    "restarted.\n")
                    .arg(jobIds.join(QStringLiteral(", ")));
    }
    text += QStringLiteral(
        "- Do not re-run a command whose effect you have not verified. The operation "
        "that was in flight when the link broke has an unknown outcome.\n\n"
        "Re-observe what you need, then continue the work.");
    return text;
}

/* Report a remote workspace that can no longer serve tool calls. A link that
 * can be re-established is, and the agent is handed a re-observation brief so
 * an unattended workflow continues without assuming anything survived. Either
 * way the current turn ends: the operation that broke it is unverified, and
 * letting the model retry it is how an uncertain write becomes two. */
/* The job ids are read from the connection rather than passed in: a defaulted
 * parameter is how the briefing's job branch came to be unreachable, with
 * every call site quietly supplying an empty list. */
QString remoteWorkspaceHealth(QSocRemoteConnection *conn, QSocAgent *agent = nullptr)
{
    if (conn == nullptr || conn->isUsable()) {
        return {};
    }
    QString    reconnectErr;
    const auto outcome = conn->reconnect(&reconnectErr);
    if (outcome == QSocRemoteConnection::ReconnectOutcome::NotNeeded) {
        return {};
    }
    if (outcome == QSocRemoteConnection::ReconnectOutcome::Reconnected) {
        if (agent != nullptr) {
            auto cfg = agent->getConfig();
            applyRemoteHostToConfig(conn, &cfg);
            agent->setConfig(cfg);
            /* A continuation, not a user request: a user_prompt_submit hook
             * that blocks would otherwise drop this silently and leave the
             * model working from beliefs nothing has re-checked. */
            agent->queueContinuation(reobservationBriefing(conn, conn->jobs()->liveJobIds()));
        }
        return QStringLiteral(
                   "The SSH link was re-established after %1 attempt%2. Remote state has not been "
                   "observed since it broke, so this turn stops here and the next one starts by "
                   "re-checking it.")
            .arg(conn->lastReconnectAttempts())
            .arg(conn->lastReconnectAttempts() == 1 ? QString() : QStringLiteral("s"));
    }
    QString text = conn->unusableText();
    switch (outcome) {
    case QSocRemoteConnection::ReconnectOutcome::Exhausted:
        if (!reconnectErr.isEmpty()) {
            text += QStringLiteral(" (reconnect failed after %1 attempts: %2)")
                        .arg(conn->lastReconnectAttempts())
                        .arg(reconnectErr);
        }
        break;
    case QSocRemoteConnection::ReconnectOutcome::BudgetSpent:
        /* Must not read as "the host was asked and refused": nothing was sent.
         * A turn that already paid for a reconnect does not pay again, because
         * every later tool call would pay the same full connect sequence with
         * the interface frozen for the sum of them. */
        text += QStringLiteral(" (this turn already reconnected once; it was not retried)");
        break;
    case QSocRemoteConnection::ReconnectOutcome::Aborted:
        text += QStringLiteral(" (you asked to stop, so the reconnect was not retried)");
        break;
    case QSocRemoteConnection::ReconnectOutcome::NotNeeded:
    case QSocRemoteConnection::ReconnectOutcome::Reconnected:
    case QSocRemoteConnection::ReconnectOutcome::Refused:
        break;
    }
    return text
           + QStringLiteral(
               ". The remote workspace is unusable; reconnect with /ssh, "
               "or run /local to work on the local tree.");
}

} // namespace

/* ---------------------------------------------------------------------- */
/* Frontend interaction callbacks. */

void QSocAgentRuntime::setAskUserHandler(AskUserHandler handler)
{
    d->askUser = std::move(handler);
}

void QSocAgentRuntime::setPlanApprovalHandler(PlanApprovalHandler handler)
{
    d->planApproval = std::move(handler);
}

void QSocAgentRuntime::setSecretHandler(SecretHandler handler)
{
    d->secret = std::move(handler);
}

void QSocAgentRuntime::setTextEditHandler(TextEditHandler handler)
{
    d->textEdit = std::move(handler);
}

void QSocAgentRuntime::setDirectoryPicker(DirPicker picker)
{
    d->dirPicker = std::move(picker);
}

void QSocAgentRuntime::setMenuHandler(MenuHandler handler)
{
    d->menu = std::move(handler);
}

void QSocAgentRuntime::setUserWatchingProbe(std::function<bool()> probe)
{
    d->userWatching = std::move(probe);
}

/* ---------------------------------------------------------------------- */
/* Remote connect / disconnect. */

bool QSocAgentRuntime::connectRemote(const QString &target, QString *error)
{
    if (target.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("empty SSH target");
        }
        return false;
    }
    const QSocHostProfile *profile = d->hostCatalog != nullptr ? d->hostCatalog->find(target)
                                                               : nullptr;
    QString                shellError;
    if (!d->remoteConn
             ->setShellPreference(profile != nullptr ? profile->shell : QString(), &shellError)) {
        if (error != nullptr) {
            *error = QStringLiteral("host.yml entry %1: %2").arg(target, shellError);
        }
        return false;
    }
    d->cancelRequested = false;
    AgentRemoteState staged;
    QString          connectError;
    const auto       cancelled = [this] {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return d->cancelRequested;
    };
    const auto confirmHostKey = [this](const QString &prompt) {
        return d->menu
               && d->menu(
                      prompt,
                      {QStringLiteral("No, cancel the connection"),
                       QStringLiteral("Yes, trust this host key")},
                      {},
                      {})
                      == 1;
    };
    if (!connectAgentSshSession(
            target,
            this,
            &staged,
            &connectError,
            [this](const QString &prompt) { return d->secret ? d->secret(prompt) : QString(); },
            cancelled,
            QDeadlineTimer(30000),
            confirmHostKey)) {
        if (error)
            *error = connectError;
        return false;
    }
    for (const QString &notice : staged.hostKeyNotices) {
        emitOutput(notice + QLatin1Char('\n'), static_cast<int>(QSocAgentRuntimeStyle::Warning));
    }
    const QString workspace = d->options.workspace.isEmpty() ? QStringLiteral("/")
                                                             : d->options.workspace;
    if (cancelled() || !prepareAgentRemoteWorkspace(workspace, &staged, &connectError)) {
        discardAgentRemoteState(&staged);
        if (error)
            *error = d->cancelRequested ? QStringLiteral("SSH connection cancelled") : connectError;
        return false;
    }
    if (!d->remoteConn->adopt(std::move(staged))) {
        /* A refused adopt consumes nothing, so the transport is still
         * ours to free. */
        // cppcheck-suppress accessMoved
        discardAgentRemoteState(&staged);
        if (error != nullptr) {
            *error = QStringLiteral("internal error: incomplete remote transport");
        }
        return false;
    }
    d->remoteConn->setRebuilder([this](
                                    const QString    &target,
                                    const QString    &workspace,
                                    AgentRemoteState *out,
                                    QString          *errorMessage,
                                    QDeadlineTimer    deadline) {
        AgentRemoteState fresh;
        if (!connectAgentSshSession(
                target,
                this,
                &fresh,
                errorMessage,
                {},
                [this] {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                    return d->cancelRequested || QSocInterrupt::requested();
                },
                deadline)) {
            return false;
        }
        if (!prepareAgentRemoteWorkspace(workspace, &fresh, errorMessage, deadline)) {
            discardAgentRemoteState(&fresh);
            return false;
        }
        *out = fresh;
        return true;
    });

    /* Build the remote registry and swap the agent onto it. */
    if (d->remoteRegistry == nullptr) {
        d->remoteRegistry = buildAgentRemoteRegistry(
            this, d->remoteConn, d->socConfig, d->monitorTaskSource, d->llmService);
        /* Re-register the spawn tool + companions so the parent LLM still
         * sees them after the swap. */
        for (const QString &name :
             {QStringLiteral("agent"),
              QStringLiteral("agent_status"),
              QStringLiteral("send_message"),
              QStringLiteral("agent_resume"),
              QStringLiteral("memory_read"),
              QStringLiteral("memory_write"),
              QStringLiteral("memory_delete"),
              QStringLiteral("ask_user"),
              QStringLiteral("enter_plan_mode"),
              QStringLiteral("exit_plan_mode"),
              QStringLiteral("goal_complete")}) {
            if (QSocTool *tool = d->localRegistry->getTool(name)) {
                d->remoteRegistry->registerTool(tool);
            }
        }
    }
    d->agent->setToolRegistry(d->remoteRegistry);
    {
        auto newCfg               = d->agent->getConfig();
        newCfg.remoteMode         = true;
        newCfg.remoteName         = d->remoteConn->target();
        newCfg.remoteDisplay      = d->remoteConn->display();
        newCfg.remoteWorkspace    = d->remoteConn->workspace();
        newCfg.remoteWorkingDir   = d->remoteConn->path()->cwd();
        newCfg.remoteWritableDirs = d->remoteConn->path()->writableDirs();
        applyRemoteHostToConfig(d->remoteConn, &newCfg);
        newCfg.skillListing = d->skillListing();
        d->agent->setConfig(newCfg);
    }
    d->remoteConn->setWorkingDirectoryObserver([this](const QString &cwd) {
        auto cfg             = d->agent->getConfig();
        cfg.remoteWorkingDir = cwd;
        d->agent->setConfig(cfg);
    });
    d->remoteConn->setAbortProbe([this] {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        return d->cancelRequested || QSocInterrupt::requested();
    });
    d->agent->setRequestBoundaryHandler([conn = d->remoteConn] {
        QSocInterrupt::clearRequest();
        conn->resetReconnectBudget();
    });
    d->agent->setWorkspaceHealthProbe([this] {
        const QString         reason = remoteWorkspaceHealth(d->remoteConn, d->agent);
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::RemoteChanged;
        event.text = d->remoteConn->target();
        event.flag = d->remoteConn->isUsable();
        emit eventRaised(event);
        return reason;
    });

    /* Pull the remote project's agent definitions. */
    if (d->remoteConn->sftp() != nullptr && !d->remoteConn->workspace().isEmpty()) {
        d->agentDefinitions->scanFromRemoteSftp(
            d->remoteConn->sftp(), d->remoteConn->workspace() + QStringLiteral("/.qsoc/agents"));
    }

    QSocAgentRuntimeEvent event;
    event.kind      = QSocAgentRuntimeEvent::Kind::RemoteChanged;
    event.flag      = true;
    event.text      = d->remoteConn->target();
    event.secondary = d->remoteConn->workspace();
    event.at        = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    wireSessionTools();
    emit statusChanged();
    return true;
}

void QSocAgentRuntime::disconnectRemote()
{
    if (d->remoteConn->session() == nullptr) {
        return;
    }
    d->agent->setToolRegistry(d->localRegistry);
    d->agent->setWorkspaceHealthProbe({});
    if (auto *spawnTool = dynamic_cast<QSocToolAgent *>(
            d->localRegistry->getTool(QStringLiteral("agent")))) {
        if (auto *defs = spawnTool->definitionRegistry()) {
            defs->removeByScope(QStringLiteral("project"));
        }
    }
    if (d->remoteRegistry != nullptr) {
        d->remoteRegistry->deleteLater();
        d->remoteRegistry = nullptr;
    }
    d->remoteConn->teardown();
    {
        auto newCfg       = d->agent->getConfig();
        newCfg.remoteMode = false;
        newCfg.remoteName.clear();
        newCfg.remoteDisplay.clear();
        newCfg.remoteWorkspace.clear();
        newCfg.remoteWorkingDir.clear();
        newCfg.remoteWritableDirs.clear();
        newCfg.remoteOs.clear();
        newCfg.remoteArch.clear();
        newCfg.remoteShell.clear();
        newCfg.skillListing = d->skillListing();
        d->agent->setConfig(newCfg);
    }

    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::RemoteChanged;
    event.flag = false;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    wireSessionTools();
    emit statusChanged();
}

bool QSocAgentRuntime::isRemote() const
{
    return d->remoteConn->session() != nullptr;
}

QString QSocAgentRuntime::remoteTarget() const
{
    return d->remoteConn->target();
}

QString QSocAgentRuntime::remoteWorkspace() const
{
    return d->remoteConn->workspace();
}

/* ---------------------------------------------------------------------- */
/* Working directory + project switching. */

bool QSocAgentRuntime::setWorkingDirectory(const QString &requested, QString *error)
{
    if (d->remoteConn->session() != nullptr) {
        QString why;
        if (d->remoteConn->setWorkingDirectory(requested, &why)
            != QSocRemoteConnection::CwdChange::Changed) {
            if (error != nullptr) {
                *error = why;
            }
            return false;
        }
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::WorkingDirChanged;
        event.text = d->remoteConn->path()->cwd();
        event.at   = QDateTime::currentDateTimeUtc();
        emit eventRaised(event);
        return true;
    }
    const QString   base = d->pathContext ? d->pathContext->getWorkingDir() : QDir::currentPath();
    const QString   resolved = QFileInfo(requested).isAbsolute()
                                   ? requested
                                   : QDir(base).absoluteFilePath(requested);
    const QFileInfo info(resolved);
    if (!info.exists() || !info.isDir()) {
        if (error != nullptr) {
            *error = QStringLiteral("Not a directory: %1").arg(resolved);
        }
        return false;
    }
    const QString canonical = info.canonicalFilePath();
    if (d->pathContext) {
        d->pathContext->setWorkingDir(canonical);
    }
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::WorkingDirChanged;
    event.text = canonical;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    return true;
}

QString QSocAgentRuntime::workingDirectory() const
{
    if (d->remoteConn->session() != nullptr) {
        return d->remoteConn->path()->cwd();
    }
    if (d->pathContext) {
        return d->pathContext->getWorkingDir();
    }
    return QDir::currentPath();
}

bool QSocAgentRuntime::switchProject(const QString &directory, QString *error)
{
    const QFileInfo info(directory);
    if (!info.exists() || !info.isDir()) {
        if (error != nullptr) {
            *error = QStringLiteral("Not a directory: %1").arg(directory);
        }
        return false;
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical == d->projectManager->getProjectPath()) {
        if (error != nullptr) {
            *error = QStringLiteral("Already on project: %1").arg(canonical);
        }
        return false;
    }

    if (d->currentSession
        && !persistSessionState(
            d->agent, d->currentSession.get(), d->persistedMessages, d->lastPersistedIndex)) {
        if (error != nullptr) {
            *error = QStringLiteral("Session persistence failed; project unchanged.");
        }
        return false;
    }

    const QString  nextSessionId = QSocSession::generateId();
    const QString &projectPath   = canonical;
    const QString  sessionPath
        = QDir(QSocSession::sessionsDir(projectPath)).filePath(nextSessionId + ".jsonl");
    if (!freshSessionPathAvailable(sessionPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not prepare a session in the new project.");
        }
        return false;
    }
    auto nextSession
        = std::make_unique<QSocSession>(nextSessionId, sessionPath, QSocSession::StorageMode::Fresh);
    auto nextHistory = std::make_unique<QSocFileHistory>(projectPath, nextSessionId);
    if (!nextHistory->storageIsBound()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not prepare a session in the new project.");
        }
        return false;
    }
    d->installSessionWriteBarrier(this, nextSession.get(), nextHistory.get());
    nextSession->appendMeta(
        QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    nextSession->appendMeta(QStringLiteral("cwd"), projectPath);

    /* Reset per-project state. */
    d->busManager->resetBusData();
    d->moduleManager->resetModuleData();
    d->generateManager->resetGenerateData();
    if (auto *shell = dynamic_cast<QSocToolShellBash *>(d->localRegistry->getTool("bash")))
        shell->killOwned();
    if (auto *lsp = d->lspService) {
        lsp->stopAll();
    }
    if (d->pathContext) {
        d->pathContext->clearUserDirs();
    }

    d->projectManager->setProjectPath(canonical);
    d->socConfig->loadConfig();
    d->llmService->setConfig(d->socConfig);
    d->projectManager->loadFirst(true);

    if (auto *lsp = d->lspService) {
        lsp->startAll(d->projectManager->getProjectPath());
    }

    {
        QSocAgentConfig cfg = d->agent->getConfig();
        cfg.projectPath     = d->projectManager->getProjectPath();
        cfg.skillListing    = d->skillListing();
        d->agent->setConfig(cfg);
    }
    if (d->pathContext) {
        d->pathContext->setWorkingDir(d->projectManager->getProjectPath());
    }
    if (d->goalCatalog) {
        d->goalCatalog->load(d->projectManager->getProjectPath());
    }

    d->agent->clearPendingRequests();
    d->agent->clearHistory();
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
    d->recoveryRun.reset();
    d->recoveryRequiresInput = false;
    d->pendingAutoInputs.clear();
    d->titleGenerated = false;
    if (d->pathContext) {
        d->pathContext->readState().clear();
    }
    d->loopScheduler->setProjectDir(d->projectManager->getProjectPath());

    QSocAgentRuntimeEvent event;
    event.kind      = QSocAgentRuntimeEvent::Kind::ProjectChanged;
    event.text      = canonical;
    event.secondary = nextSessionId;
    event.at        = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    emit sessionChanged(nextSessionId);
    wireSessionTools();
    emit statusChanged();
    return true;
}

/* ---------------------------------------------------------------------- */
/* Model / effort / plan controls. */

QStringList QSocAgentRuntime::availableModels() const
{
    return d->llmService->availableModels();
}

QString QSocAgentRuntime::currentModelId() const
{
    return d->llmService->getCurrentModelId();
}

bool QSocAgentRuntime::setCurrentModel(const QString &modelId)
{
    if (!d->llmService->setCurrentModel(modelId)) {
        emitOutput(QStringLiteral("Unknown model: %1\n").arg(modelId));
        return false;
    }
    LLMModelConfig  cfg      = d->llmService->getModelConfig(modelId);
    QSocAgentConfig agentCfg = d->agent->getConfig();
    if (cfg.contextTokens > 0) {
        agentCfg.maxContextTokens = cfg.contextTokens;
    }
    agentCfg.effortLevel = cfg.effort;
    agentCfg.modelId     = modelId;
    d->agent->setConfig(agentCfg);
    d->agent->resetTokenCounting();

    /* Persist model selection to the effective config file. */
    QString configPath;
    if (QFile::exists(QDir(d->projectManager->getProjectPath()).filePath(".qsoc.yml"))) {
        configPath = QDir(d->projectManager->getProjectPath()).filePath(".qsoc.yml");
    } else {
        configPath = QDir(QSocPaths::userRoot()).filePath("qsoc.yml");
    }
    try {
        YAML::Node root      = YAML::LoadFile(configPath.toStdString());
        root["llm"]["model"] = modelId.toStdString();
        std::ofstream fout(configPath.toStdString());
        fout << root;
    } catch (const YAML::Exception &err) {
        QSocConsole::warn() << "Failed to save model to config:" << err.what();
    }

    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::ModelChanged;
    event.text = modelId;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    wireSessionTools();
    emit statusChanged();
    return true;
}

QString QSocAgentRuntime::effortLevel() const
{
    const QString level = d->agent->getConfig().effortLevel;
    return level.isEmpty() ? QStringLiteral("off") : level;
}

void QSocAgentRuntime::setEffortLevel(const QString &level)
{
    QString normalized = level.toLower().trimmed();
    if (normalized == QStringLiteral("off")) {
        normalized.clear();
    }
    d->agent->setEffortLevel(normalized);
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::EffortChanged;
    event.text = normalized.isEmpty() ? QStringLiteral("off") : normalized;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    emit statusChanged();
}

void QSocAgentRuntime::setPlanMode(bool enabled)
{
    auto cfg     = d->agent->getConfig();
    cfg.planMode = enabled;
    d->agent->setConfig(cfg);
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::PlanModeChanged;
    event.flag = enabled;
    event.at   = QDateTime::currentDateTimeUtc();
    emit eventRaised(event);
    emit statusChanged();
}

bool QSocAgentRuntime::planMode() const
{
    return d->agent->getConfig().planMode;
}

/* ---------------------------------------------------------------------- */
/* Status snapshot. */

QSocAgentRuntime::UsageSnapshot QSocAgentRuntime::usage() const
{
    UsageSnapshot snapshot;
    const auto    observed = d->agent->observedUsage();
    snapshot.inputTokens   = observed.inputTokens;
    snapshot.outputTokens  = observed.outputTokens;
    const auto estimate    = d->agent->contextEstimate();
    snapshot.usedTokens    = static_cast<int>(
        qMin<qint64>(estimate.point(), std::numeric_limits<int>::max()));
    snapshot.maxTokens        = d->agent->effectiveContextTokens();
    snapshot.compactThreshold = d->agent->getConfig().compactThreshold;
    snapshot.approximate      = estimate.approximate();
    return snapshot;
}

void QSocAgentRuntime::fillContextUsage(QSocAgentRuntimeEvent &event) const
{
    const UsageSnapshot snapshot = usage();
    event.kind                   = QSocAgentRuntimeEvent::Kind::ContextUsage;
    event.usedTokens             = snapshot.usedTokens;
    event.maxTokens              = snapshot.maxTokens;
    event.threshold              = snapshot.compactThreshold;
    event.flag                   = snapshot.approximate;
}

nlohmann::json QSocAgentRuntime::statusLinePayload() const
{
    nlohmann::json payload;
    payload["version"] = QSOC_VERSION;
    payload["model"]   = {{"id", currentModelId().toStdString()}};
    payload["effort"]  = effortLevel().toStdString();
    payload["workspace"]
        = {{"cwd", workingDirectory().toStdString()},
           {"project_dir", d->projectManager->getProjectPath().toStdString()}};
    const UsageSnapshot snapshot = usage();
    payload["context"]
        = {{"used_tokens", snapshot.usedTokens},
           {"max_tokens", snapshot.maxTokens},
           {"used_percentage",
            snapshot.maxTokens > 0 ? 100.0 * snapshot.usedTokens / snapshot.maxTokens : 0.0}};
    payload["tokens"] = {{"input", snapshot.inputTokens}, {"output", snapshot.outputTokens}};
    if (d->currentSession) {
        payload["session"] = {{"id", d->currentSession->id().toStdString()}};
    }
    if (isRemote()) {
        payload["remote"]
            = {{"target", remoteTarget().toStdString()},
               {"workspace", remoteWorkspace().toStdString()}};
    }
    return payload;
}

void QSocAgentRuntime::emitEvent(const QSocAgentRuntimeEvent &event)
{
    emit eventRaised(event);
}
