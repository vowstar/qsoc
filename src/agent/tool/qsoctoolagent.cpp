// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagent.h"
#include <QStandardPaths>
#include <QUuid>

#include "agent/qsocagent.h"
#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsochookmanager.h"
#include "agent/qsocsession.h"
#include "agent/qsocsessionrecovery.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/tool/qsoctoolmemory.h"
#include "agent/tool/qsoctoolskill.h"
#include "common/qllmservice.h"
#include "common/qsocinterrupt.h"
#include "common/qsocprojectmanager.h"

#include <memory>
#include <utility>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

QSocToolAgent::QSocToolAgent(
    QObject                     *parent,
    QLLMService                 *llmService,
    QSocToolRegistry            *parentRegistry,
    QSocAgentConfig              parentConfig,
    QSocAgentDefinitionRegistry *defRegistry,
    QSocSubAgentTaskSource      *taskSource)
    : QSocTool(parent)
    , llmService_(llmService)
    , parentRegistry_(parentRegistry)
    , parentConfig_(std::move(parentConfig))
    , defRegistry_(defRegistry)
    , taskSource_(taskSource)
{}

namespace {

/* Liveness budget for reusing a cached host binding. A sibling dispatch
 * should not wait out a transfer timeout to learn the host is gone. */
constexpr int kHostProbeMs = 2000;

/* Who reports a spawned task's terminal outcome to the parent. Undecided
 * while the tool call itself may still return it inline; Inline once an
 * inline return owns it; Async once the call handed the task off and the
 * notification queue owns it. Exactly one owner, decided once. */
enum class Delivery : std::uint8_t { Undecided, Inline, Async };

/* The delivery decision plus the first terminal event, cached so a decision
 * made after the event can still deliver it. */
struct DeliveryState
{
    Delivery         mode         = Delivery::Undecided;
    bool             notified     = false;
    bool             haveTerminal = false;
    QSocTask::Status terminalState{};
    QString          terminalBody;
};

/* The tool-result word for a terminal state: a run that reported an outcome
 * keeps it, and only a run cut off mid-work is uncertain. */
QSocTool::ResultStatus resultStatusFor(QSocTask::Status state)
{
    switch (state) {
    case QSocTask::Status::Completed:
        return QSocTool::ResultStatus::Ok;
    case QSocTask::Status::Failed:
        return QSocTool::ResultStatus::Failed;
    default:
        return QSocTool::ResultStatus::Uncertain;
    }
}

/* A workspace the model chose must already exist, must stay at or below
 * @p root when one is granted, and must not be the root or the login
 * directory, spelled any way that resolves to them. The resolved login
 * directory never appears in the error, because the error reaches the
 * model provider. */
bool acceptNamedWorkspace(
    QSocSftpClient *sftp, const QString &workspace, const QString &root, QString *errorMessage)
{
    const auto refuse = [errorMessage](const QString &why) {
        if (errorMessage != nullptr) {
            *errorMessage = why;
        }
        return false;
    };
    const auto present = [sftp](const QString &path, QString *resolved) {
        QString err;
        return sftp != nullptr && sftp->presence(path, &err) == QSocSftpClient::Presence::Present
               && sftp->realPath(path, resolved, &err) == QSocSftpClient::Presence::Present;
    };
    QString resolved;
    QString home;
    QString granted;
    if (!present(workspace, &resolved)) {
        return refuse(QStringLiteral("workspace %1 does not exist on the host").arg(workspace));
    }
    if (!present(QStringLiteral("."), &home)) {
        return refuse(QStringLiteral("the host login directory could not be read"));
    }
    if (resolved == QStringLiteral("/") || resolved == home) {
        return refuse(
            QStringLiteral("workspace %1 is / or the login directory; name a project directory")
                .arg(workspace));
    }
    if (!root.isEmpty()
        && (!present(root, &granted)
            || (resolved != granted && !resolved.startsWith(granted + QLatin1Char('/'))))) {
        return refuse(QStringLiteral("workspace %1 resolves outside the granted workspace %2")
                          .arg(workspace, root));
    }
    return true;
}

/* Memory tools in front of @p base, bound to @p memory, so a child on this
 * registry reads and writes the project store of the binding it runs on. */
void overlayMemoryTools(
    QSocToolRegistry *registry, QSocToolRegistry *base, QObject *owner, QSocMemoryManager *memory)
{
    if (base == nullptr) {
        return;
    }
    if (base->getTool(QStringLiteral("memory_read")) != nullptr) {
        registry->registerTool(new QSocToolMemoryRead(owner, memory));
    }
    if (base->getTool(QStringLiteral("memory_write")) != nullptr) {
        registry->registerTool(new QSocToolMemoryWrite(owner, memory));
    }
    if (base->getTool(QStringLiteral("memory_delete")) != nullptr) {
        registry->registerTool(new QSocToolMemoryDelete(owner, memory));
    }
}

/* The skill listing the skill tools of @p registry would show. */
QString skillListingOf(QSocToolRegistry *registry)
{
    const auto *finder = dynamic_cast<QSocToolSkillFind *>(
        registry != nullptr ? registry->getTool(QStringLiteral("skill_find")) : nullptr);
    return finder != nullptr ? QSocToolSkillFind::formatPromptListing(finder->scanAllSkills())
                             : QString();
}

} // namespace

std::shared_ptr<QSocToolAgent::HostBinding> QSocToolAgent::resolveHostBinding(
    const QString                &host,
    const QString                &workspace,
    const std::optional<QString> &namedRoot,
    QString                      *errorMessage)
{
    if (!QSocInterrupt::handlerReady()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SSH refused: Ctrl-C handling is unavailable");
        }
        return nullptr;
    }
    ResolvedHostTarget resolved;
    if (!resolveHostTarget(host, hostCatalog_, sshConfigParser_, &resolved, errorMessage)) {
        return nullptr;
    }
    const QString root = workspace.isEmpty() ? resolved.workspaceHint : workspace;
    if (root.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                                "host '%1' has no catalog workspace; pass workspace=<absolute "
                                "directory on that host>")
                                .arg(host);
        }
        return nullptr;
    }
    const QString cacheKey = host + QLatin1Char('\n') + root;
    const auto    cached   = hostCache_.find(cacheKey);
    if (cached != hostCache_.end()) {
        const std::shared_ptr<HostBinding> &binding = cached.value();
        /* A cached registry is only worth reusing while its host can still
         * answer. Handing one back on a dead session gives every later
         * sibling a workspace that cannot serve a call, and the failure then
         * surfaces inside the child instead of here. The liveness flag alone
         * is not enough: a host that went quiet without closing the
         * connection still reads as connected, so spend one bounded round
         * trip before reusing. */
        if (binding->conn.isUsable()
            && remoteHostAnswers(binding->conn.sftp(), binding->conn.path()->root(), kHostProbeMs)) {
            return binding;
        }
        /* Unpublish: no later spawn is handed this binding again. The memory
         * goes with the last child that was already dispatched onto it. */
        hostCache_.erase(cached);
    }

    auto binding = std::make_shared<HostBinding>();
    if (!binding->conn.setShellPreference(resolved.shell, errorMessage)) {
        return nullptr;
    }
    /* No Qt parent for the transport: the connection deletes it, and a second
     * owner in this tool's tree would free it under a binding that outlived
     * the tool. */
    if (!connectAgentSshSession(resolved.connectString, nullptr, &binding->state, errorMessage, {}, [] {
            return QSocInterrupt::requested();
        })) {
        return nullptr;
    }
    if ((namedRoot && !acceptNamedWorkspace(binding->state.sftp, root, *namedRoot, errorMessage))
        || !prepareAgentRemoteWorkspace(root, &binding->state, errorMessage)) {
        discardAgentRemoteState(&binding->state);
        return nullptr;
    }
    if (!binding->conn.adopt(std::move(binding->state))) {
        discardAgentRemoteState(&binding->state);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("internal error: incomplete remote transport");
        }
        return nullptr;
    }
    /* A dropped host B is sensed by the child's probe and recovered here: the
     * connection can now rebuild its own transport instead of only answering
     * Refused. */
    installBindingRecovery(&binding->conn);
    /* One observer over the binding's own child list, installed once here so
     * siblings dispatched onto this shared connection each track its working
     * directory instead of the last one replacing the others. */
    installCwdFanout(&binding->conn, binding->cwdChildren);
    binding->owner    = std::make_unique<QObject>();
    binding->registry = buildAgentRemoteRegistry(
        binding->owner.get(), &binding->conn, parentRegistry_, nullptr, llmService_);
    binding->memory = new QSocMemoryManager(
        binding->owner.get(),
        memoryManager_ != nullptr ? memoryManager_->getProjectManager() : nullptr);
    binding->memory
        ->setRemoteWorkspace(binding->conn.endpointIdentity(), binding->conn.canonicalWorkspace());
    overlayMemoryTools(binding->registry, parentRegistry_, binding->owner.get(), binding->memory);
    hostCache_.insert(cacheKey, binding);
    return binding;
}

std::shared_ptr<QSocToolAgent::LocalBinding> QSocToolAgent::localBinding()
{
    if (localBinding_ == nullptr) {
        auto binding      = std::make_shared<LocalBinding>();
        binding->owner    = std::make_unique<QObject>();
        binding->registry = new QSocToolRegistry(binding->owner.get());
        binding->memory   = new QSocMemoryManager(
            binding->owner.get(),
            memoryManager_ != nullptr ? memoryManager_->getProjectManager() : nullptr);
        overlayMemoryTools(binding->registry, parentRegistry_, binding->owner.get(), binding->memory);
        binding->registry->setFallback(parentRegistry_);
        localBinding_ = binding;
    }
    return localBinding_;
}

void QSocToolAgent::installBindingRecovery(QSocRemoteConnection *conn)
{
    if (conn == nullptr) {
        return;
    }
    /* A blocking reconnect on this binding holds the loop just like the first
     * connect, so it reads the same process-wide interrupt. */
    conn->setAbortProbe([] { return QSocInterrupt::requested(); });
    /* The same two connect helpers resolveHostBinding used for the first bind,
     * reached the same way. reconnect() supplies the binding's own (target,
     * workspace), so there is no second connect path to keep in step. */
    conn->setRebuilder([](const QString    &target,
                          const QString    &workspace,
                          AgentRemoteState *out,
                          QString          *errorMessage,
                          QDeadlineTimer    deadline) {
        AgentRemoteState fresh;
        if (!connectAgentSshSession(
                target,
                nullptr,
                &fresh,
                errorMessage,
                {},
                [] { return QSocInterrupt::requested(); },
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
}

QString QSocToolAgent::probeBindingHealth(QSocRemoteConnection *conn, int *budget)
{
    if (conn == nullptr) {
        return {};
    }
    /* isUsable() alone can read a host that went quiet without closing as
     * live, and a child waiting on the model has run no call to flip the flag,
     * so spend one bounded round trip before trusting it. */
    if (conn->isUsable() && remoteHostAnswers(conn->sftp(), conn->path()->root(), kHostProbeMs)) {
        return {};
    }
    QString    reconnectErr;
    const auto outcome = conn->reconnect(&reconnectErr, budget);
    switch (outcome) {
    case QSocRemoteConnection::ReconnectOutcome::NotNeeded:
        /* The socket is open but the host did not answer the probe above.
         * Report the silence rather than a false all-clear. */
        return QStringLiteral(
                   "The host %1 stopped answering; re-observe remote state before "
                   "acting.")
            .arg(conn->target());
    case QSocRemoteConnection::ReconnectOutcome::Reconnected:
        return QStringLiteral(
                   "The SSH link to %1 was re-established after %2 attempt%3. Remote "
                   "state has not been observed since it broke, so re-check it before "
                   "acting.")
            .arg(conn->target())
            .arg(conn->lastReconnectAttempts())
            .arg(conn->lastReconnectAttempts() == 1 ? QString() : QStringLiteral("s"));
    case QSocRemoteConnection::ReconnectOutcome::Refused:
    case QSocRemoteConnection::ReconnectOutcome::Exhausted:
    case QSocRemoteConnection::ReconnectOutcome::BudgetSpent:
    case QSocRemoteConnection::ReconnectOutcome::Aborted:
        break;
    }
    QString text = conn->unusableText();
    if (!reconnectErr.isEmpty()) {
        text += QStringLiteral(" (reconnect failed: %1)").arg(reconnectErr);
    }
    return text.isEmpty() ? QStringLiteral("the remote workspace is unusable") : text;
}

void QSocToolAgent::installCwdFanout(QSocRemoteConnection *conn, const CwdFanout &fanout)
{
    if (conn == nullptr || !fanout) {
        return;
    }
    conn->setWorkingDirectoryObserver([fanout](const QString &cwd) {
        for (int index = static_cast<int>(fanout->size()) - 1; index >= 0; --index) {
            QSocAgent *child = (*fanout)[index].data();
            if (child == nullptr) {
                fanout->removeAt(index);
                continue;
            }
            auto cfg             = child->getConfig();
            cfg.remoteWorkingDir = cwd;
            child->setConfig(cfg);
        }
    });
}

void QSocToolAgent::bindChildCwd(const CwdFanout &fanout, QSocAgent *child)
{
    if (!fanout || child == nullptr) {
        return;
    }
    fanout->append(QPointer<QSocAgent>(child));
}

namespace {

json toJsonArray(const QStringList &values)
{
    json array = json::array();
    for (const QString &value : values) {
        array.push_back(value.toStdString());
    }
    return array;
}

} // namespace

QString QSocToolAgent::getName() const
{
    return QStringLiteral("agent");
}

QString QSocToolAgent::getDescription() const
{
    QString desc = QStringLiteral(
        "Spawn a child sub-agent to handle a self-contained task. The child runs in a "
        "conversation that inherits parent history in fork mode, or uses the selected role's "
        "focused context. It returns a concise result. "
        "Use for exploration, summarization, focused multi-step work that should not "
        "pollute the main context. Set run_in_background=true to keep working while the "
        "child runs; the active child surfaces in the Ctrl+B task overlay.\n"
        "\nAvailable subagent_type values:\n");
    if (defRegistry_ != nullptr) {
        desc += defRegistry_->describeAvailable();
    }
    desc += QStringLiteral(
        "\nOmit `host` to run the child where the parent works now, and omit `model` to run "
        "it on the parent's model. Set them only when the task needs a host or model that "
        "the system prompt lists for its stated capability.\n");
    return desc;
}

json QSocToolAgent::getParametersSchema() const
{
    json enumValues = json::array();
    enumValues.push_back("fork");
    if (defRegistry_ != nullptr) {
        for (const QString &subName : defRegistry_->availableNames()) {
            enumValues.push_back(subName.toStdString());
        }
    }
    json schema = json{
        {"type", "object"},
        {"properties",
         {{"subagent_type",
           {{"type", "string"},
            {"enum", enumValues},
            {"description",
             "Sub-agent type. Use 'fork' to inherit the parent's full message "
             "history and continue the existing thread. "
             "When omitted, fork mode is also assumed."}}},
          {"description",
           {{"type", "string"},
            {"description", "Short 3-7 word label shown in the task overlay row."}}},
          {"prompt",
           {{"type", "string"},
            {"description",
             "Full instructions the child agent will receive as its first user message."}}},
          {"run_in_background",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             "When true, return task_id immediately and run the child asynchronously."}}},
          {"isolation",
           {{"type", "string"},
            {"enum", json::array({"none", "worktree"})},
            {"default", "none"},
            {"description",
             "When 'worktree', the child runs inside a fresh git worktree of the "
             "current project, so its file changes are isolated from the parent. "
             "Silently falls back to 'none' if the project is not a git repo. "
             "Not supported on remote workspaces."}}},
          {"host",
           {{"type", "string"},
            {"description",
             "Execution host for this sub-agent. Omit it to run on the parent's current "
             "binding; 'local' is this machine. A named host opens (or reuses) an SSH "
             "session for the child only."}}},
          {"workspace",
           {{"type", "string"},
            {"description",
             "Absolute directory on a named `host` the child works in. Required for a "
             "host without a catalog workspace. Must already exist and must not be / "
             "or the login directory."}}}}},
        {"required", json::array({"description", "prompt"})}};
    const QStringList hosts = dispatchPolicy_.hostChoices();
    if (!hosts.isEmpty()) {
        schema["properties"]["host"]["enum"] = toJsonArray(hosts);
    }
    const QStringList models = dispatchPolicy_.modelChoices();
    if (!models.isEmpty()) {
        schema["properties"]["model"]
            = {{"type", "string"},
               {"enum", toJsonArray(models)},
               {"description",
                "llm.models key the child runs on. Omit it to use the parent's model."}};
    }
    return schema;
}

namespace {

/* Marker injected into a forked child's messages so a nested fork
 * can detect "this context is already forked" and refuse. */
constexpr auto kForkMarkerTag = "<!-- qsoc-fork-tag -->";

bool messagesContainForkMarker(const json &messages)
{
    if (!messages.is_array()) {
        return false;
    }
    for (const auto &msg : messages) {
        if (!msg.contains("content")) {
            continue;
        }
        if (!msg["content"].is_string()) {
            continue;
        }
        const std::string content = msg["content"].get<std::string>();
        if (content.find(kForkMarkerTag) != std::string::npos) {
            return true;
        }
    }
    return false;
}

QString jsonStringField(const json &args, const char *key)
{
    if (!args.contains(key) || !args[key].is_string()) {
        return {};
    }
    return QString::fromStdString(args[key].get<std::string>());
}

bool jsonBoolField(const json &args, const char *key, bool fallback)
{
    if (!args.contains(key) || !args[key].is_boolean()) {
        return fallback;
    }
    return args[key].get<bool>();
}

/* Worktree helpers. Synchronous git invocations; failure is reported
 * via empty-string return and lets the caller fall back to no
 * isolation. Used only when isolation == "worktree". */
QString worktreeRootDir()
{
    return QDir::tempPath() + QStringLiteral("/qsoc-worktrees");
}

bool runGit(const QString &cwd, const QStringList &args, QString *errOut = nullptr)
{
    QProcess proc;
    if (!cwd.isEmpty()) {
        proc.setWorkingDirectory(cwd);
    }
    proc.start(QStringLiteral("git"), args);
    if (!proc.waitForFinished(15000)) {
        if (errOut != nullptr) {
            *errOut = QStringLiteral("git timed out");
        }
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (errOut != nullptr) {
            *errOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        }
        return false;
    }
    return true;
}

QString createWorktreeFor(const QString &repoRoot, const QString &taskId)
{
    if (repoRoot.isEmpty()) {
        return {};
    }
    /* Cheap precheck: is the parent a git working tree? */
    if (!runGit(repoRoot, {QStringLiteral("rev-parse"), QStringLiteral("--is-inside-work-tree")})) {
        return {};
    }
    QDir().mkpath(worktreeRootDir());
    const QString wtPath = QDir(worktreeRootDir()).filePath(QStringLiteral("qsoc_wt_") + taskId);
    QString       err;
    if (!runGit(
            repoRoot,
            {QStringLiteral("worktree"),
             QStringLiteral("add"),
             QStringLiteral("--detach"),
             wtPath,
             QStringLiteral("HEAD")},
            &err)) {
        return {};
    }
    return wtPath;
}

void removeWorktreeAt(const QString &repoRoot, const QString &wtPath)
{
    if (wtPath.isEmpty()) {
        return;
    }
    runGit(
        repoRoot,
        {QStringLiteral("worktree"), QStringLiteral("remove"), QStringLiteral("--force"), wtPath});
    /* Belt and braces: remove any leftover directory tree. */
    QDir(wtPath).removeRecursively();
}

/* Declare the workspace a binding's tools actually resolve on, so the child's
 * system prompt and its hook envelope name the host that answers its calls. */
void bindConfigToHost(QSocRemoteConnection *conn, const QString &alias, QSocAgentConfig *cfg)
{
    cfg->remoteMode         = true;
    cfg->remoteName         = conn->target();
    cfg->remoteAlias        = alias;
    cfg->remoteDisplay      = conn->display();
    cfg->remoteWorkspace    = conn->workspace();
    cfg->remoteWorkingDir   = conn->path()->cwd();
    cfg->remoteWritableDirs = conn->path()->writableDirs();
    applyRemoteHostToConfig(conn, cfg);
    cfg->skillListing.clear();
    loadAgentRemoteProjectRules(conn, cfg);
}

/* A child the main agent sends to this machine from a remote binding: the
 * remote declarations go, and the local skill listing of @p local returns. */
void unbindConfigFromHost(const QSocAgentConfig &local, QSocAgentConfig *cfg)
{
    cfg->remoteMode = false;
    cfg->remoteName.clear();
    cfg->remoteAlias.clear();
    cfg->remoteDisplay.clear();
    cfg->remoteWorkspace.clear();
    cfg->remoteWorkingDir.clear();
    cfg->remoteWritableDirs.clear();
    cfg->remoteMachine       = QSocMachine{};
    cfg->remoteGitRepository = false;
    cfg->remoteProjectRules  = {};
    cfg->skillListing        = local.skillListing;
}

/* Where a child built from @p cfg runs: the alias it was bound by, the SSH
 * target behind that alias, and the workspace root. */
QSocSubAgentTaskSource::Dispatch placementOf(const QSocAgentConfig &cfg)
{
    if (!cfg.remoteMode) {
        return {QStringLiteral("local"), QStringLiteral("local"), cfg.projectPath};
    }
    return {
        cfg.remoteAlias.isEmpty() ? cfg.remoteName : cfg.remoteAlias,
        cfg.remoteName,
        cfg.remoteWorkspace};
}

/* JSON spelling of a terminal flavour. Deliberately not statusLine()'s words:
 * QSocTool::classifyResult reads "error" / "uncertain" from a top-level
 * "status" member, and reads any other value, "failed" included, as ok. */
const char *resultStatusWord(QSocTool::ResultStatus status)
{
    switch (status) {
    case QSocTool::ResultStatus::Ok:
        return "ok";
    case QSocTool::ResultStatus::Failed:
        return "error";
    case QSocTool::ResultStatus::Uncertain:
        return "uncertain";
    case QSocTool::ResultStatus::Dispatched:
        return "async_launched";
    }
    return "uncertain";
}

bool runGitFromInsideWorktree(const QStringList &args, const QString &cwd)
{
    /* Without --git-dir, running from inside the worktree is enough
     * for `git worktree remove` to find its source repo via the
     * parent worktree's .git pointer. */
    return runGit(cwd, args);
}

} // namespace

QSocTaskEvent QSocToolAgent::taskEvent(
    const QString &taskId,
    const QString &subagentType,
    const QString &status,
    const QString &body,
    const QString &transcriptPath)
{
    constexpr int kBodyCap = 4000;
    QSocTaskEvent event;
    event.taskId     = taskId;
    event.sourceTag  = QStringLiteral("agent");
    event.kind       = QStringLiteral("task_notification");
    event.status     = status;
    event.agentType  = subagentType;
    event.outputFile = transcriptPath;
    event.content    = body;
    if (body.size() > kBodyCap) {
        event.content = body.left(kBodyCap)
                        + QStringLiteral(
                            "\n[... truncated; read the transcript for the full output ...]");
    }
    return event;
}

QString QSocToolAgent::buildTaskNotification(
    const QString &taskId,
    const QString &subagentType,
    const QString &status,
    const QString &body,
    const QString &transcriptPath)
{
    return QSocTaskEventQueue::formatTaskNotification(
        taskEvent(taskId, subagentType, status, body, transcriptPath));
}

int QSocToolAgent::sweepStaleWorktrees(int maxAgeSec)
{
    const QString root = worktreeRootDir();
    QDir          dir(root);
    if (!dir.exists()) {
        return 0;
    }
    const QDateTime   cutoff  = QDateTime::currentDateTime().addSecs(-maxAgeSec);
    int               removed = 0;
    const QStringList entries
        = dir.entryList({QStringLiteral("qsoc_wt_*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : entries) {
        const QString   fullPath = dir.filePath(name);
        const QFileInfo info(fullPath);
        if (info.lastModified() >= cutoff) {
            continue;
        }
        /* Best-effort: ask git to drop its administrative records,
         * THEN remove the directory tree. We don't know the source
         * repo, so call `git worktree remove` from inside the
         * worktree (git resolves the source via the .git file). */
        runGitFromInsideWorktree(
            {QStringLiteral("worktree"),
             QStringLiteral("remove"),
             QStringLiteral("--force"),
             fullPath},
            fullPath);
        QDir(fullPath).removeRecursively();
        ++removed;
    }
    return removed;
}

QString QSocToolAgent::execute(const json &arguments)
{
    return spawn(arguments, nullptr);
}

QString QSocToolAgent::spawn(const json &arguments, const ResumeSeed *seed)
{
    if (defRegistry_ == nullptr || taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"agent tool is not wired up"})");
    }
    const QPointer<QSocToolCallContext> callContext(currentCallContext());

    const QString subagentType = jsonStringField(arguments, "subagent_type");
    const QString description  = jsonStringField(arguments, "description");
    const QString prompt       = jsonStringField(arguments, "prompt");
    const bool    background   = jsonBoolField(arguments, "run_in_background", false);
    const QString isolation    = jsonStringField(arguments, "isolation").isEmpty()
                                     ? QStringLiteral("none")
                                     : jsonStringField(arguments, "isolation");

    if (prompt.isEmpty()) {
        return QStringLiteral(R"({"status":"error","error":"prompt is required"})");
    }

    /* Forks inherit a captured identity and history, then rebuild binding rules. */
    const bool isFork = subagentType.isEmpty() || subagentType == QStringLiteral("fork");

    const QSocAgentDefinition *def = nullptr;
    if (!isFork) {
        def = defRegistry_->find(subagentType);
        if (def == nullptr) {
            return QString::fromUtf8(
                json{
                    {"status", "error"},
                    {"error", std::string("unknown subagent_type: ") + subagentType.toStdString()},
                    {"available",
                     QString(defRegistry_->availableNames().join(QStringLiteral(", ")))
                         .toStdString()}}
                    .dump()
                    .c_str());
        }
    }

    /* Resolve parent registry + config dynamically from the live parent
     * agent when bound. This makes the spawn tool remote-mode correct:
     * after `/remote` swaps the parent's registry and sets remoteMode,
     * the child built here picks up the SSH-backed tool registry and
     * the remote config in the same step, instead of a stale local
     * snapshot captured at construction time. */
    QSocToolRegistry *effectiveRegistry = parentRegistry_;
    QSocAgentConfig   effectiveConfig   = parentConfig_;
    QLLMService      *effectiveLlm      = llmService_;
    if (parentAgent_ != nullptr) {
        if (auto *liveReg = parentAgent_->getToolRegistry()) {
            effectiveRegistry = liveReg;
        }
        effectiveConfig = parentAgent_->getConfig();
        if (auto *liveLlm = parentAgent_->getLLMService()) {
            effectiveLlm = liveLlm;
        }
    }

    /* Concurrency policy: a sliding window the task source enforces.
     * 0 (the default) means unbounded; spawns run as soon as they are
     * registered and flow control is left to the provider's 429
     * backpressure plus the agent loop's backoff. A positive value
     * caps in-flight children and queues the rest (Pending), admitting
     * the next as each slot frees. Set `agent.max_concurrent_subagents`
     * / `QSOC_MAX_CONCURRENT_SUBAGENTS` to re-bound for a strict
     * single-key provider. The sentinel is owned by the task source;
     * pass the config value through verbatim. */
    taskSource_->setMaxConcurrent(effectiveConfig.maxConcurrentSubagents);

    /* Fork-mode preconditions: needs a live parent agent to copy
     * the message history from, and rejects nested forks via the
     * marker check. Checked BEFORE the llm-null guard so a fork
     * spawn attempt fails for the right reason regardless of llm
     * wiring. */
    std::optional<QSocAgent::ForkSnapshot> forkSnapshot;
    if (isFork) {
        if (parentAgent_ == nullptr) {
            return QStringLiteral(
                R"({"status":"error","error":"fork mode requires a bound parent agent"})");
        }
        forkSnapshot    = parentAgent_->captureForkSnapshot();
        effectiveConfig = forkSnapshot->config;
        if (messagesContainForkMarker(forkSnapshot->messages)) {
            return QStringLiteral(
                R"({"status":"error","error":"forks cannot be nested; this context is already forked"})");
        }
    }

    if (effectiveLlm == nullptr || effectiveRegistry == nullptr) {
        return QStringLiteral(
            R"({"status":"error","error":"LLM service or tool registry not configured"})");
    }

    /* Where and on which model the child runs: the call, then the
     * definition, then the main agent, within what the user granted. */
    QSocChildRequest request;
    request.host      = jsonStringField(arguments, "host");
    request.workspace = jsonStringField(arguments, "workspace");
    request.model     = jsonStringField(arguments, "model");
    request.fork      = isFork;
    if (def != nullptr) {
        request.host            = request.host.isEmpty() ? def->preferredHost : request.host;
        request.definitionModel = def->model;
    }
    QSocMainState mainState;
    mainState.modelId = effectiveLlm->getCurrentModelId();
    mainState.effort  = effectiveConfig.effortLevel;
    mainState.remote  = effectiveConfig.remoteMode;
    mainState.alias   = placementOf(effectiveConfig).host;
    QString    planErr;
    const auto plan = dispatchPolicy_.resolveChild(
        request,
        mainState,
        hostCatalog_,
        [effectiveLlm](const QString &key) -> std::optional<LLMModelConfig> {
            if (!effectiveLlm->availableModels().contains(key)) {
                return std::nullopt;
            }
            return effectiveLlm->getModelConfig(key);
        },
        &planErr);
    if (!plan) {
        return QString::fromStdString(
            json{{"status", "error"}, {"error", planErr.toStdString()}}.dump());
    }
    const QString &hostArg    = plan->host;
    const bool     remoteHost = !hostArg.isEmpty() && hostArg != QStringLiteral("local");
    /* `local` while the main agent is remote: this machine's own tools. */
    const bool toLocal = hostArg == QStringLiteral("local") && effectiveConfig.remoteMode;
    if (isolation == QStringLiteral("worktree")
        && (remoteHost || (effectiveConfig.remoteMode && !toLocal))) {
        return QStringLiteral(
            R"({"status":"error","error":"isolation=worktree is not supported on remote workspaces"})");
    }
    /* The one name for the host the child runs on. Null means it inherits the
     * parent's binding; when set, the child's registry, its config and its
     * workspace health all come from this pointer, so they cannot disagree
     * about which host answers for the child. */
    std::shared_ptr<HostBinding>  childHost;
    std::shared_ptr<LocalBinding> localHost;
    if (remoteHost) {
        QString       hostErr;
        const QString workspace = seed != nullptr ? seed->dispatch.workspace : plan->workspace;
        childHost               = resolveHostBinding(
            hostArg,
            workspace,
            seed == nullptr && plan->workspaceNamed ? plan->workspaceRoot : std::optional<QString>(),
            &hostErr);
        if (childHost == nullptr) {
            return QString::fromUtf8(
                json{
                    {"status", "error"},
                    {"error",
                     QString("host '%1' is not dispatchable: %2").arg(hostArg, hostErr).toStdString()},
                    {"host", hostArg.toStdString()}}
                    .dump()
                    .c_str());
        }
        effectiveRegistry = childHost->registry;
    } else if (toLocal) {
        localHost         = localBinding();
        effectiveRegistry = localHost->registry;
    }

    /* When isolation == "worktree" and the parent is a git repo,
     * create a fresh detached worktree off HEAD and route the
     * child's projectPath there. Silent fallback to no isolation
     * if git is unavailable or the parent isn't a working tree. */
    QString       worktreePath;
    const QString parentRepoRoot = effectiveConfig.projectPath;
    if (isolation == QStringLiteral("worktree") && !parentRepoRoot.isEmpty()) {
        worktreePath = createWorktreeFor(parentRepoRoot, QStringLiteral("pending"));
    }

    QSocAgentConfig childCfg = effectiveConfig;
    childCfg.isSubAgent      = true;
    if (toLocal) {
        unbindConfigFromHost(parentConfig_, &childCfg);
    }
    if (isFork) {
        childCfg.systemPromptOverride = forkSnapshot->identityPrompt;
    } else {
        childCfg.systemPromptOverride = def->promptBody;
        childCfg.toolsDeny.append(def->toolsDeny);
        if (!def->toolsAllow.isEmpty()) {
            for (const auto &name : def->toolsAllow) {
                if (!childCfg.toolsAllow.isEmpty() && !childCfg.toolsAllow.contains(name)) {
                    childCfg.toolsDeny.append(name);
                }
            }
            childCfg.toolsAllow = def->toolsAllow;
        }
        childCfg.toolsAllow.removeDuplicates();
        childCfg.toolsDeny.removeDuplicates();
        childCfg.maxTurnsOverride = def->maxTurns;
        childCfg.criticalReminder = def->criticalReminder;
        childCfg.autoLoadMemory   = def->injectMemory;
        childCfg.injectProjectMd  = def->injectProjectMd;
        if (!def->injectSkills) {
            childCfg.skillListing.clear();
        }
    }
    childCfg.effortLevel = plan->effort;
    if (plan->model) {
        childCfg.modelId = plan->model->id;
        if (plan->model->contextTokens > 0) {
            childCfg.maxContextTokens = plan->model->contextTokens;
        }
    }
    if (!worktreePath.isEmpty()) {
        childCfg.projectPath = worktreePath;
        if (isFork || def->injectSkills) {
            QSocProjectManager project;
            project.setProjectPath(worktreePath);
            QSocToolSkillFind scanner(nullptr, &project);
            childCfg.skillListing = QSocToolSkillFind::formatPromptListing(scanner.scanAllSkills());
        }
    }
    if (childHost != nullptr) {
        bindConfigToHost(&childHost->conn, hostArg, &childCfg);
        if (isFork || def->injectSkills) {
            childCfg.skillListing = skillListingOf(childHost->registry);
        }
    }
    QSocSubAgentTaskSource::Dispatch placed = placementOf(childCfg);
    placed.model                            = request.model;
    if (seed != nullptr
        && (placed.endpoint != seed->dispatch.endpoint
            || (!seed->dispatch.workspace.isEmpty()
                && placed.workspace != seed->dispatch.workspace))) {
        return QString::fromStdString(
            json{
                {"status", "error"},
                {"error",
                 QStringLiteral("the run used %1:%2, the child would now run on %3:%4")
                     .arg(seed->dispatch.host, seed->dispatch.workspace, placed.host, placed.workspace)
                     .toStdString()}}
                .dump());
    }

    /* Per-child LLMService: clone the live parent's service so the
     * child has its own streaming reply and buffers. Without this,
     * concurrent sub-agents trample each other's single-flight
     * invariant. A child on another model gets that entry as resolved
     * when the call was made. */
    auto *childLlm = effectiveLlm->clone(nullptr);
    childLlm->setModel(effectiveLlm->getCurrentModelConfig());
    if (plan->model) {
        childLlm->setCurrentModel(plan->model->id);
        childLlm->setModel(*plan->model);
    }
    auto *child = new QSocAgent(nullptr, childLlm, effectiveRegistry, childCfg);
    childLlm->setParent(child); /* tie LLM lifetime to child */
    if (childHost != nullptr) {
        /* The child resolves its tools through this binding on every call, so
         * it holds its own reference: a sibling spawn that finds the host gone
         * unpublishes the binding without freeing it under this child. */
        new HostBindingHold(child, childHost);
        /* Route the binding's cwd into the child's config so a reconnect's
         * rewind cannot leave the system prompt naming a directory host B has
         * left. */
        bindChildCwd(childHost->cwdChildren, child);
    }
    if (localHost != nullptr) {
        new HostBindingHold(child, localHost);
    }
    /* planMode rides childCfg (copied from the parent). The shell safety
     * judge is a separate member, so hand it down too: a read-only
     * exploration child judges its own bash the same way. */
    if (childCfg.planMode && parentAgent_ != nullptr) {
        if (parentAgent_->contextualBashSafetyJudge()) {
            child->setContextualBashSafetyJudge(parentAgent_->contextualBashSafetyJudge());
        } else {
            child->setBashSafetyJudge(parentAgent_->bashSafetyJudge());
        }
    }
    /* Health probe by host. A child on host B senses and recovers host B's own
     * link through its binding, not the parent's host: the captured binding
     * outlives the probe, held alongside the HostBindingHold above. A child on
     * the parent's host forwards the parent's probe, which captures CLI-owned
     * widgets that do not outlive the parent, so a dead parent reads as usable,
     * matching the unset semantics. */
    if (childHost != nullptr) {
        std::shared_ptr<HostBinding> boundHost = childHost;
        /* The generation this child last acted on. A sibling child on the same
         * binding can replace the transport between this child's turns; the new
         * link answering the probe is not continuity, so a generation it did
         * not observe must make it re-check rather than read as healthy. */
        auto observed = std::make_shared<quint64>(boundHost->conn.generation());
        /* This child's own reconnect budget, not the shared connection's: a
         * sibling on the same binding must not spend or re-credit it. One spawn
         * is one request from the parent, so it starts fresh here and bounds
         * this child's reconnects across its run. */
        auto budget = std::make_shared<int>(0);
        child->setWorkspaceHealthProbe([boundHost, observed, budget]() -> QString {
            QSocRemoteConnection *conn = &boundHost->conn;
            const quint64         now  = conn->generation();
            if (*observed != 0 && *observed != now) {
                *observed = now;
                return QStringLiteral(
                           "The SSH link to %1 was replaced since this task last acted "
                           "on it. Remote state has not been observed on the new link, "
                           "so re-check it before acting.")
                    .arg(conn->target());
            }
            const QString health = probeBindingHealth(conn, budget.get());
            /* A reconnect inside the probe bumps the generation; adopt it so the
             * child's own recovery is not re-reported as a sibling's next turn. */
            *observed = conn->generation();
            return health;
        });
    } else if (!toLocal && parentAgent_ != nullptr && parentAgent_->hasWorkspaceHealthProbe()) {
        QPointer<QSocAgent> probeParent(parentAgent_);
        child->setWorkspaceHealthProbe([probeParent]() -> QString {
            return probeParent.isNull() ? QString() : probeParent->probeWorkspaceHealth();
        });
    }
    QSocMemoryManager *placedMemory = childHost != nullptr   ? childHost->memory
                                      : localHost != nullptr ? localHost->memory
                                                             : nullptr;
    if (placedMemory != nullptr && (isFork || def->injectMemory)) {
        child->setMemoryManager(placedMemory);
    } else if (isFork && worktreePath.isEmpty() && placedMemory == nullptr) {
        child->setMemoryManager(parentAgent_->getMemoryManager());
    } else if (memoryManager_ != nullptr && def != nullptr && def->injectMemory) {
        child->setMemoryManager(memoryManager_);
    }
    /* Per-definition hooks override: when def declares its own
     * hooks, build a child-scoped hook manager. Otherwise inherit
     * the parent's. The child-owned manager is parented to the
     * child so it goes when the child does. */
    if (def != nullptr && !def->hooks.isEmpty()) {
        auto *childHooks = new QSocHookManager(child);
        childHooks->setConfig(def->hooks);
        child->setHookManager(childHooks);
        /* Mirror config onto the child's QSocAgentConfig so the
         * sub-agent suppression check (in fire* methods) sees a
         * non-empty hooks structure and lets lifecycle events fire. */
        childCfg.hooks = def->hooks;
        child->setConfig(childCfg);
    } else if (hookManager_ != nullptr) {
        child->setHookManager(hookManager_);
    }
    if (loopScheduler_ != nullptr) {
        child->setLoopScheduler(loopScheduler_);
    }

    /* Fork mode: copy parent's message history into the child + a
     * marker system message so subsequent forks detect the chain. */
    if (isFork) {
        child->setApprovedPlan(forkSnapshot->approvedPlan);
        json forkedMessages = forkSnapshot->messages;
        if (!forkedMessages.is_array()) {
            forkedMessages = json::array();
        }
        forkedMessages.push_back({
            {"role", "system"},
            {"content",
             std::string(kForkMarkerTag) + "\nFork point: continuing as a forked sub-agent."},
        });
        child->setMessages(forkedMessages);
    }
    if (seed != nullptr) {
        child->setMessages(seed->history);
    }

    if (parentAgent_ && parentAgent_->toolResultStore()) {
        const QString artifactOwner = QUuid::createUuid().toString(QUuid::Id128);
        const QString artifactDirectory
            = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                  .filePath(QStringLiteral("tool-artifacts/") + artifactOwner);
        QString artifactError;
        bool    artifactsReady = bool(child->toolResultStore());
        if (!parentAgent_->toolResultStore()->isTemporary())
            artifactsReady
                = child->bindToolResultStore(artifactDirectory, artifactOwner, &artifactError);
        if (artifactsReady && isFork && !forkSnapshot->artifactRefs.isEmpty())
            artifactsReady = forkSnapshot->artifactStore
                             && child->toolResultStore()->inherit(
                                 *forkSnapshot->artifactStore,
                                 forkSnapshot->artifactRefs,
                                 &artifactError);
        if (!artifactsReady) {
            const auto failedStore = child->toolResultStore();
            if (failedStore && !failedStore->isTemporary() && failedStore->isBound())
                QDir(failedStore->directory()).removeRecursively();
            delete child;
            return QStringLiteral("Error: child tool result storage could not be prepared. %1")
                .arg(artifactError);
        }
    }

    const QString effectiveType = isFork ? QStringLiteral("fork") : subagentType;
    const QString label         = description.isEmpty() ? effectiveType : description;
    const QString taskId        = taskSource_->registerRun(label, effectiveType, child, prompt);
    /* Stash isolation + worktree on the run so the meta sidecar
     * captures them; mirrors what the response JSON reports. */
    taskSource_->setIsolationMetadata(taskId, isolation, worktreePath);
    taskSource_->setDispatchMetadata(taskId, placed);

    /* Forward child token usage into the parent's running totals so
     * the parent's status pill / cost view reflects total cost in
     * real time. Routes only the DELTA on each emission to avoid
     * double counting. */
    if (parentAgent_ != nullptr) {
        QSocAgent *parent     = parentAgent_.data();
        auto       prevIn     = std::make_shared<qint64>(0);
        auto       prevOut    = std::make_shared<qint64>(0);
        const auto resetUsage = [prevIn, prevOut](const QString &) {
            *prevIn  = 0;
            *prevOut = 0;
        };
        QObject::connect(child, &QSocAgent::runComplete, parent, resetUsage);
        QObject::connect(child, &QSocAgent::runError, parent, resetUsage);
        QObject::connect(child, &QSocAgent::runAborted, parent, resetUsage);
        QObject::connect(
            child,
            &QSocAgent::tokenUsage,
            parent,
            [parent, prevIn, prevOut](qint64 inputTok, qint64 outputTok) {
                const qint64 dIn  = inputTok - *prevIn;
                const qint64 dOut = outputTok - *prevOut;
                *prevIn           = inputTok;
                *prevOut          = outputTok;
                parent->addExternalTokenUsage(dIn, dOut);
            });
    }

    /* Stream child progress into the overlay's transcript buffer. */
    QPointer<QSocSubAgentTaskSource> srcGuard(taskSource_);
    QObject::connect(
        child, &QSocAgent::contentChunk, taskSource_, [srcGuard, taskId](const QString &chunk) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(taskId, chunk);
            }
        });
    QObject::connect(
        child,
        &QSocAgent::toolCalled,
        taskSource_,
        [srcGuard, taskId](const QString &name, const QString &args) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(
                    taskId,
                    QStringLiteral("\n[tool] ") + name + QStringLiteral(" ") + args.left(200)
                        + QStringLiteral("\n"));
                srcGuard->setWaitingForPeer(taskId, name == QStringLiteral("wait_agent"));
            }
        });
    QObject::connect(
        child,
        &QSocAgent::toolResult,
        taskSource_,
        [srcGuard, taskId](const QString &name, const QString &result) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(
                    taskId,
                    QStringLiteral("[result ") + name + QStringLiteral("] ") + result.left(400)
                        + QStringLiteral("\n"));
                srcGuard->setWaitingForPeer(taskId, false);
            }
        });
    QObject::connect(
        child,
        &QSocAgent::toolCallFinished,
        taskSource_,
        [srcGuard,
         taskId](const QString &, const QString &name, const QString &, QSocToolResultStatus status) {
            if (!srcGuard)
                return;
            srcGuard->appendTranscript(
                taskId, QStringLiteral("[outcome %1] %2").arg(name, QSocTool::statusLine(status)));
            emit srcGuard->taskEvidenceChanged(taskId);
        });

    QObject::connect(
        child,
        &QSocAgent::toolCallOutput,
        taskSource_,
        [srcGuard, taskId](const QString &, const QString &text) {
            if (srcGuard)
                srcGuard->appendTranscript(taskId, text);
        });

    /* Worktree cleanup hook captured by lambdas below. Empty path
     * = no isolation, helpers are no-ops. */
    auto wtCleanup = [parentRepoRoot, worktreePath]() {
        removeWorktreeAt(parentRepoRoot, worktreePath);
    };
    const bool retainWorkspace = taskSource_->mailbox() != nullptr;
    if (retainWorkspace && !worktreePath.isEmpty()) {
        QObject::connect(child, &QObject::destroyed, [wtCleanup]() { wtCleanup(); });
    }
    auto cancelTask = [srcGuard, taskId, wtCleanup]() {
        if (srcGuard.isNull()) {
            return false;
        }
        QSocTask::Row row;
        const bool    pending   = srcGuard->findRow(taskId, &row)
                                  && row.status == QSocTask::Status::Pending;
        const bool    cancelled = srcGuard->killTask(taskId);
        if (cancelled && pending) {
            wtCleanup();
        }
        return cancelled;
    };

    /* If def declares specific `skills`, prepend their content to
     * the prompt as a context block. Capped at 4 KB per skill so a
     * fat SKILL.md doesn't dominate the child's context window. */
    QString effectivePrompt = prompt;
    if (def != nullptr && !def->skills.isEmpty()) {
        if (auto *skillTool = dynamic_cast<QSocToolSkillFind *>(
                effectiveRegistry->getTool(QStringLiteral("skill_find")))) {
            const QList<QSocToolSkillFind::SkillInfo> all = skillTool->scanAllSkills();
            QString                                   prefix;
            constexpr int                             kSkillCapBytes = 4 * 1024;
            for (const QString &wanted : def->skills) {
                for (const auto &skill : all) {
                    if (skill.name != wanted) {
                        continue;
                    }
                    QString content = skillTool->readSkillContent(skill);
                    if (content.size() > kSkillCapBytes) {
                        content = content.left(kSkillCapBytes)
                                  + QStringLiteral("\n[... skill content truncated ...]\n");
                    }
                    prefix += QStringLiteral("[Skill: ") + wanted + QStringLiteral("]\n") + content
                              + QStringLiteral("\n\n");
                    break;
                }
            }
            if (!prefix.isEmpty()) {
                effectivePrompt = prefix + QStringLiteral("=== Task ===\n") + prompt;
            }
        }
    }

    /* Terminal wiring: the child's three terminal signals and a panel kill all
     * funnel into markTerminal, which emits taskTerminal exactly once. The one
     * consumer below caches that first event and delivers it through whichever
     * channel owns delivery, so there is a single producer of parent
     * notifications and no second channel to double-report through. The
     * parent identity lets the bus drop a completion whose parent is gone. */
    QPointer<QSocAgent> childGuard(child);
    auto                delivery = std::make_shared<DeliveryState>();
    /* Why the child stopped, when it stopped for a reason rather than by
     * request. takeStopNotice() is destructive, so exactly one reader:
     * the persistent runAborted handler, which Qt invokes before the
     * foreground one because it was connected first. */
    auto          abortReason    = std::make_shared<QString>();
    const QString parentIdentity = parentAgent_ ? parentAgent_->agentIdentity() : QString();
    const QPointer<QSocTaskEventQueue> busGuard(
        seed != nullptr && !seed->notifyParent ? nullptr : eventQueue_.data());
    auto deliverAsync = [srcGuard, busGuard, parentIdentity, taskId, effectiveType, delivery]() {
        if (delivery->notified || !delivery->haveTerminal || busGuard.isNull()) {
            return;
        }
        delivery->notified           = true;
        const QString transcriptPath = srcGuard.isNull() ? QString()
                                                         : srcGuard->transcriptPathFor(taskId);
        QSocTaskEvent event          = taskEvent(
            taskId,
            effectiveType,
            QSocTask::statusWord(delivery->terminalState),
            delivery->terminalBody,
            transcriptPath);
        event.agentId = parentIdentity;
        busGuard->enqueue(event);
    };
    QObject::connect(
        child, &QSocAgent::runComplete, taskSource_, [srcGuard, taskId](const QString &finalText) {
            if (!srcGuard.isNull()) {
                srcGuard->markTerminal(taskId, QSocTask::Status::Completed, finalText);
            }
        });
    QObject::connect(
        child, &QSocAgent::runError, taskSource_, [srcGuard, taskId](const QString &error) {
            if (!srcGuard.isNull()) {
                srcGuard->markTerminal(taskId, QSocTask::Status::Failed, error);
            }
        });
    QObject::connect(
        child,
        &QSocAgent::runAborted,
        taskSource_,
        [srcGuard, taskId, childGuard, abortReason](const QString &) {
            *abortReason      = childGuard.isNull() ? QString() : childGuard->takeStopNotice();
            const QString why = abortReason->isEmpty() ? QStringLiteral("aborted") : *abortReason;
            /* Cut off mid-work: the child never reported an outcome, so
             * its side effects are unknown. Not Failed. */
            if (!srcGuard.isNull()) {
                srcGuard->markTerminal(taskId, QSocTask::Status::Aborted, why);
            }
        });
    /* The one terminal consumer: cache the first event, deliver it only when
     * the async channel owns delivery, clean up always (idempotent). A task
     * killed while still pending reaches here too, through the source's own
     * transition, so a run with no child signals still lands. */
    QObject::connect(
        taskSource_,
        &QSocTaskSource::taskTerminal,
        child,
        [taskId,
         delivery,
         deliverAsync,
         retainWorkspace,
         wtCleanup](const QString &id, QSocTask::Status state, const QString &text) {
            if (id != taskId) {
                return;
            }
            if (!delivery->haveTerminal) {
                delivery->haveTerminal  = true;
                delivery->terminalState = state;
                delivery->terminalBody  = text;
            }
            if (delivery->mode == Delivery::Async) {
                deliverAsync();
            }
            if (!retainWorkspace || state == QSocTask::Status::Aborted)
                wtCleanup();
        });

    const json launchedResponse = json{
        {"status", "async_launched"},
        {"agent_id", child->agentIdentity().toStdString()},
        {"task_id", taskId.toStdString()},
        {"subagent_type", effectiveType.toStdString()},
        {"description", label.toStdString()},
        {"isolation", isolation.toStdString()},
        {"worktree", worktreePath.toStdString()}};

    /* The task source owns admission: start() runs the child now when a
     * slot is free, else queues it (Pending) until one frees. */
    auto launcher = [child, effectivePrompt]() { child->runStream(effectivePrompt); };

    if (background) {
        QMetaObject::Connection cancelConnection;
        if (!callContext.isNull()) {
            cancelConnection = QObject::connect(
                callContext,
                &QSocToolCallContext::cancellationRequested,
                taskSource_,
                [cancelTask]() { cancelTask(); });
            if (callContext->isCancellationRequested()) {
                cancelTask();
            }
        }
        QSocTask::Row beforeStart;
        if (taskSource_->findRow(taskId, &beforeStart)
            && beforeStart.status == QSocTask::Status::Pending) {
            taskSource_->start(taskId, launcher);
        }
        QObject::disconnect(cancelConnection);
        if (!callContext.isNull() && callContext->isCancellationRequested()) {
            /* Cancellation can arrive while start() sits in a hook's event
             * loop, so the child may already have reached a real outcome
             * before it was killed. The cached first terminal wins: a run
             * that failed reports failed, and only a run with no outcome is
             * uncertain. This return IS the delivery either way: the terminal
             * event must not also reach the parent as a notification. */
            delivery->mode = Delivery::Inline;
            json resp      = launchedResponse;
            if (delivery->haveTerminal) {
                resp["status"] = resultStatusWord(resultStatusFor(delivery->terminalState));
                resp["result"] = delivery->terminalBody.toStdString();
            } else {
                resp["status"] = resultStatusWord(QSocTool::ResultStatus::Uncertain);
                resp["result"] = "aborted";
            }
            return QString::fromUtf8(resp.dump().c_str());
        }
        QSocTask::Row row;
        const bool    queued = taskSource_->findRow(taskId, &row)
                               && row.status == QSocTask::Status::Pending;
        json          resp   = launchedResponse;
        if (queued) {
            resp["status"] = "queued";
        }
        /* Handed off: the notification queue owns delivery from here. A child
         * that already finished inside start()'s nested loop cached its event
         * while nobody owned delivery, so flush it now. */
        delivery->mode = Delivery::Async;
        deliverAsync();
        return QString::fromUtf8(resp.dump().c_str());
    }

    /* Foreground: drive the child through the streaming loop and wait
     * on a local event loop that quits on either the child's terminal
     * state or the auto-background timeout. The temp connections below
     * steer only this local loop; they are disconnected before it
     * leaves scope, so a terminal signal arriving after the run is
     * backgrounded reaches only the persistent handlers above (never a
     * dangling stack object). Nesting depth matches the legacy
     * blocking path, which also ran a nested loop per child turn. */
    QEventLoop fgLoop;
    bool       fgTerminal = false;
    QString    fgBody;
    /* Which of the three terminal signals fired. An abort is Uncertain, not
     * Failed: the child was cut off mid-work, so its effects are unknown. */
    auto fgStatus = QSocTool::ResultStatus::Ok;
    auto stopFg   = [&fgLoop,
                     &fgTerminal,
                     &fgBody,
                     &fgStatus,
                     delivery](QSocTool::ResultStatus status, const QString &body) {
        if (fgTerminal) {
            return;
        }
        /* A terminal outcome claims inline delivery, and only from Undecided:
         * once the auto-background timer handed the task off, the outcome
         * belongs to the notification, not to this return. */
        if (delivery->mode == Delivery::Async) {
            return;
        }
        delivery->mode = Delivery::Inline;
        fgTerminal     = true;
        fgStatus       = status;
        fgBody         = body;
        fgLoop.quit();
    };
    QList<QMetaObject::Connection> fgConns;
    fgConns << QObject::connect(
        child, &QSocAgent::runComplete, &fgLoop, [&stopFg](const QString &text) {
            stopFg(QSocTool::ResultStatus::Ok, text);
        });
    fgConns << QObject::connect(child, &QSocAgent::runError, &fgLoop, [&stopFg](const QString &err) {
        stopFg(QSocTool::ResultStatus::Failed, err);
    });
    fgConns << QObject::connect(
        child, &QSocAgent::runAborted, &fgLoop, [&stopFg, abortReason](const QString &partial) {
            if (abortReason->isEmpty()) {
                stopFg(QSocTool::ResultStatus::Uncertain, partial);
                return;
            }
            stopFg(
                QSocTool::ResultStatus::Uncertain,
                partial.isEmpty() ? *abortReason : partial + QStringLiteral("\n") + *abortReason);
        });
    /* A panel kill of a still-pending foreground run ends it without a child
     * signal, so without this the wait below never quits and, with no
     * auto-background timer, hangs forever. The child-signal handlers above
     * fire first for a run that actually started, so this only lands the
     * never-started case. */
    fgConns << QObject::connect(
        taskSource_,
        &QSocTaskSource::taskTerminal,
        &fgLoop,
        [&stopFg, taskId](const QString &id, QSocTask::Status state, const QString &text) {
            if (id != taskId) {
                return;
            }
            QSocTool::ResultStatus status = QSocTool::ResultStatus::Uncertain;
            if (state == QSocTask::Status::Completed) {
                status = QSocTool::ResultStatus::Ok;
            } else if (state == QSocTask::Status::Failed) {
                status = QSocTool::ResultStatus::Failed;
            }
            stopFg(status, text.isEmpty() ? QSocTask::statusWord(state) : text);
        });
    if (!callContext.isNull()) {
        const auto cancelForeground = [cancelTask, &stopFg]() {
            if (cancelTask()) {
                stopFg(QSocTool::ResultStatus::Uncertain, QStringLiteral("aborted"));
            }
        };
        fgConns << QObject::connect(
            callContext, &QSocToolCallContext::cancellationRequested, &fgLoop, cancelForeground);
        if (callContext->isCancellationRequested()) {
            cancelForeground();
        }
    }

    QTimer autoBg;
    autoBg.setSingleShot(true);
    QObject::connect(&autoBg, &QTimer::timeout, &fgLoop, [&fgLoop, delivery, deliverAsync]() {
        /* Hand off only while nobody owns delivery: a cancellation or a child
         * that already finished decided Inline, and a timer firing later must
         * not take the outcome away from the return that owns it. */
        if (delivery->mode != Delivery::Undecided) {
            return;
        }
        delivery->mode = Delivery::Async;
        deliverAsync();
        fgLoop.quit();
    });
    if (effectiveConfig.autoBackgroundMs > 0) {
        autoBg.start(effectiveConfig.autoBackgroundMs);
    }

    if (!fgTerminal) {
        taskSource_->start(taskId, launcher);
    }
    /* start() may run the child synchronously through a hook's nested event
     * loop, and both terminal signals and the auto-background timer can fire
     * inside it. Their quit() lands before exec(), which Qt resets on entry,
     * so entering the loop after either decision would block forever. Only
     * wait while the delivery is still undecided. */
    if (!fgTerminal && delivery->mode == Delivery::Undecided) {
        fgLoop.exec();
    }

    /* Detach the local-loop steering before fgLoop / autoBg leave
     * scope. No event processing happens between exec() returning and
     * here, so no steered signal can fire in the gap. */
    for (const auto &conn : fgConns) {
        QObject::disconnect(conn);
    }
    autoBg.stop();

    if (fgTerminal) {
        /* Finished (or was cancelled) within the window: this return owns the
         * outcome, and the notification channel never does. `result` carries
         * the body in every flavour; `status` says whether it can be
         * believed. */
        return QString::fromUtf8(
            json{
                {"status", resultStatusWord(fgStatus)},
                {"agent_id", child->agentIdentity().toStdString()},
                {"task_id", taskId.toStdString()},
                {"subagent_type", effectiveType.toStdString()},
                {"result", fgBody.toStdString()},
                {"isolation", isolation.toStdString()},
                {"worktree", worktreePath.toStdString()}}
                .dump()
                .c_str());
    }

    /* Handed off: the child keeps running on the parent's event loop and its
     * terminal state arrives as a task notification. */
    return QString::fromUtf8(launchedResponse.dump().c_str());
}

json QSocToolAgent::resumeRun(
    const QString &taskId, const QString &instruction, const QString &sender)
{
    const QString text = instruction.trimmed().isEmpty()
                             ? QStringLiteral(
                                   "Continue the task. If it is already complete, report the "
                                   "result.")
                             : instruction;
    if (taskSource_ == nullptr) {
        return {{"status", "error"}, {"error", "task source not configured"}};
    }
    if (taskSource_->liveAgentFor(taskId) != nullptr) {
        json receipt = taskSource_->sendTo(taskId, text, sender);
        if (receipt.value("status", std::string()) == "ok") {
            receipt["resume"] = receipt.value("delivery", std::string()) == "queued" ? "queued"
                                                                                     : "live";
        }
        return receipt;
    }
    QSocSubAgentTaskSource::HistoricalRun meta;
    const bool                            known = taskSource_->findHistoricalRun(taskId, &meta);
    const json history = known && !meta.legacy && !meta.historyFile.isEmpty()
                                 && meta.isolation != QStringLiteral("worktree")
                             ? QSocSession::loadMessages(meta.historyFile)
                             : json();
    if (!history.is_array() || history.empty()
        || !QSocSessionRecovery::historySafeForNewTurn(history)) {
        return {
            {"status", "error"},
            {"resume", "unavailable"},
            {"error",
             known ? "the run has no stored history to continue from"
                   : "no metadata sidecar found for this task_id"}};
    }
    ResumeSeed seed;
    seed.history = history;
    seed.dispatch
        = {meta.host.isEmpty() ? QStringLiteral("local") : meta.host,
           meta.endpoint.isEmpty() ? QStringLiteral("local") : meta.endpoint,
           meta.workspace,
           meta.model};
    seed.notifyParent = sender != QSocAgentMailbox::userSender();
    /* The parent's binding serves the run only when it is the very endpoint and
     * workspace the run used; any other run goes back to its own alias. */
    const QSocSubAgentTaskSource::Dispatch parentAt = parentAgent_ != nullptr
                                                          ? placementOf(parentAgent_->getConfig())
                                                          : QSocSubAgentTaskSource::Dispatch{};
    const bool    inherit = parentAt.endpoint == seed.dispatch.endpoint
                            && (seed.dispatch.workspace.isEmpty()
                                || parentAt.workspace == seed.dispatch.workspace);
    const QString host    = inherit ? QString() : seed.dispatch.host;
    json          result  = json::parse(
        spawn(
            json{
                {"subagent_type", meta.definition.toStdString()},
                {"description", meta.label.toStdString()},
                {"prompt", text.toStdString()},
                {"host", host.toStdString()},
                {"model", meta.model.toStdString()},
                {"run_in_background", true}},
            &seed)
            .toStdString(),
        nullptr,
        false);
    if (!result.is_object()) {
        return {{"status", "error"}, {"error", "resume failed"}};
    }
    if (result.value("status", std::string()) != "error") {
        result["resume"]       = "history";
        result["resumed_from"] = taskId.toStdString();
        result["delivery"]     = "resumed";
    }
    return result;
}

void QSocToolAgent::abort()
{
    if (taskSource_ != nullptr) {
        taskSource_->abortAll();
    }
}

void QSocToolAgent::setParentAgent(QSocAgent *agent)
{
    parentAgent_ = agent;
}

void QSocToolAgent::setTaskEventQueue(QSocTaskEventQueue *queue)
{
    eventQueue_ = queue;
}

#include "moc_qsoctoolagent.cpp"
