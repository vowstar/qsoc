// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagent.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsochookmanager.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/tool/qsoctoolskill.h"
#include "common/qllmservice.h"

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

QSocToolAgent::~QSocToolAgent()
{
    /* Tear down every cached host binding: registry first (it owns
     * the tool instances), then SFTP, then the SSH session, then
     * the ProxyJump chain in reverse so children disconnect before
     * their parents. */
    for (auto *binding : std::as_const(hostCache_)) {
        if (binding == nullptr) {
            continue;
        }
        if (binding->registry != nullptr) {
            delete binding->registry;
        }
        if (binding->state.sftp != nullptr) {
            binding->state.sftp->close();
            delete binding->state.sftp;
        }
        if (binding->state.session != nullptr) {
            binding->state.session->disconnectFromHost();
            delete binding->state.session;
        }
        for (auto it = binding->state.jumps.rbegin(); it != binding->state.jumps.rend(); ++it) {
            (*it)->disconnectFromHost();
            delete *it;
        }
        delete binding;
    }
    hostCache_.clear();
}

QSocToolRegistry *QSocToolAgent::resolveHostRegistry(const QString &host, QString *errorMessage)
{
    const auto cached = hostCache_.constFind(host);
    if (cached != hostCache_.constEnd()) {
        return cached.value()->registry;
    }

    ResolvedHostTarget resolved;
    if (!resolveHostTarget(host, hostCatalog_, sshConfigParser_, &resolved, errorMessage)) {
        return nullptr;
    }
    if (resolved.workspaceHint.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                                "no workspace registered for host '%1'; call "
                                "host_register or /ssh first")
                                .arg(host);
        }
        return nullptr;
    }

    auto *binding = new HostBinding{};
    if (!connectAgentSshSession(resolved.connectString, this, &binding->state, errorMessage)) {
        delete binding;
        return nullptr;
    }
    if (!prepareAgentRemoteWorkspace(resolved.workspaceHint, &binding->state, errorMessage)) {
        if (binding->state.sftp != nullptr) {
            binding->state.sftp->close();
            delete binding->state.sftp;
        }
        if (binding->state.session != nullptr) {
            binding->state.session->disconnectFromHost();
            delete binding->state.session;
        }
        for (auto it = binding->state.jumps.rbegin(); it != binding->state.jumps.rend(); ++it) {
            (*it)->disconnectFromHost();
            delete *it;
        }
        delete binding;
        return nullptr;
    }
    /* Pass nullptr for socConfig + monitorTaskSource: sub-agent
     * dispatch only needs file/shell/path tools on the remote.
     * Web/doc are intentionally local-only for now. */
    binding->registry = buildAgentRemoteRegistry(this, &binding->state, nullptr, nullptr);
    hostCache_.insert(host, binding);
    return binding->registry;
}

QString QSocToolAgent::getName() const
{
    return QStringLiteral("agent");
}

QString QSocToolAgent::getDescription() const
{
    QString desc = QStringLiteral(
        "Spawn a child sub-agent to handle a self-contained task. The child runs in a "
        "fresh conversation with a focused tool set and returns a single concise result. "
        "Use for exploration, summarization, focused multi-step work that should not "
        "pollute the main context. Set run_in_background=true to keep working while the "
        "child runs; the active child surfaces in the Ctrl+B task overlay.\n"
        "\nAvailable subagent_type values:\n");
    if (defRegistry_ != nullptr) {
        desc += defRegistry_->describeAvailable();
    }
    if (hostCatalog_ != nullptr) {
        const auto entries = hostCatalog_->allList();
        if (!entries.isEmpty()) {
            desc += QStringLiteral(
                "\nAvailable host values (sub-agent dispatch target):\n"
                "  local: parent's current execution context\n");
            for (const auto &entry : entries) {
                const QString cap = entry.capability.isEmpty()
                                        ? QStringLiteral("(no capability text)")
                                        : entry.capability;
                desc += QStringLiteral("  %1: %2\n").arg(entry.alias, cap);
            }
            desc += QStringLiteral(
                "\nOmit `host` to use the parent's current binding. Pick a "
                "named host above only when its capability matches the task.\n");
        }
    }
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
    json hostEnum = json::array();
    hostEnum.push_back("local");
    if (hostCatalog_ != nullptr) {
        for (const auto &entry : hostCatalog_->allList()) {
            hostEnum.push_back(entry.alias.toStdString());
        }
    }
    return json{
        {"type", "object"},
        {"properties",
         {{"subagent_type",
           {{"type", "string"},
            {"enum", enumValues},
            {"description",
             "Sub-agent type. Use 'fork' to inherit the parent's full message "
             "history (cache-cheap delegation that continues the existing thread). "
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
             "Silently falls back to 'none' if the project is not a git repo."}}},
          {"host",
           {{"type", "string"},
            {"enum", hostEnum},
            {"description",
             "Execution host for this sub-agent. 'local' (default) runs on the "
             "parent's current binding. A named catalog alias opens (or reuses) "
             "an SSH session to that host. Only catalog entries with a "
             "workspace are dispatchable; pure ~/.ssh/config aliases without a "
             "catalog entry are not listed here."}}}}},
        {"required", json::array({"description", "prompt"})}};
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

bool runGitFromInsideWorktree(const QStringList &args, const QString &cwd)
{
    /* Without --git-dir, running from inside the worktree is enough
     * for `git worktree remove` to find its source repo via the
     * parent worktree's .git pointer. */
    return runGit(cwd, args);
}

} // namespace

QString QSocToolAgent::buildTaskNotification(
    const QString &taskId,
    const QString &subagentType,
    const QString &status,
    const QString &body,
    const QString &transcriptPath)
{
    constexpr int kBodyCap = 4000;
    QString       capped   = body;
    if (capped.size() > kBodyCap) {
        capped = capped.left(kBodyCap)
                 + QStringLiteral("\n[... truncated; read the transcript for the full output ...]");
    }
    const bool isError = (status == QStringLiteral("failed") || status == QStringLiteral("aborted"));
    const QString bodyTag = isError ? QStringLiteral("error") : QStringLiteral("result");
    QString       out;
    out += QStringLiteral("<task-notification>\n");
    out += QStringLiteral("<task-id>") + taskId + QStringLiteral("</task-id>\n");
    out += QStringLiteral("<subagent-type>") + subagentType + QStringLiteral("</subagent-type>\n");
    out += QStringLiteral("<status>") + status + QStringLiteral("</status>\n");
    if (!transcriptPath.isEmpty()) {
        out += QStringLiteral("<transcript>") + transcriptPath + QStringLiteral("</transcript>\n");
    }
    out += QStringLiteral("<") + bodyTag + QStringLiteral(">\n") + capped + QStringLiteral("\n</")
           + bodyTag + QStringLiteral(">\n");
    out += QStringLiteral("</task-notification>");
    return out;
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
    if (defRegistry_ == nullptr || taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"agent tool is not wired up"})");
    }

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

    /* Fork mode: subagent_type empty or "fork" → spawn a child that
     * inherits the parent's message history + system prompt for
     * cache-identical prefix continuation. The parent's full
     * conversation is forwarded; the child gets `prompt` as the
     * next user turn. Recursion guard via the kForkMarkerTag
     * sentinel: if the parent's history already carries one, this
     * is a forked context, refuse a second fork. */
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

    /* Optional per-spawn host override. Catalog alias -> open (or
     * reuse) an SSH session for the child only, leaving the
     * parent's binding alone. 'local' or empty falls back to the
     * named definition's `preferred_host` (when set), then to the
     * parent's effective registry above. */
    QString hostArg = jsonStringField(arguments, "host");
    if (hostArg.isEmpty() && def != nullptr && !def->preferredHost.isEmpty()) {
        hostArg = def->preferredHost;
    }
    if (!hostArg.isEmpty() && hostArg != QStringLiteral("local")) {
        QString           hostErr;
        QSocToolRegistry *hostReg = resolveHostRegistry(hostArg, &hostErr);
        if (hostReg == nullptr) {
            return QString::fromUtf8(
                json{
                    {"status", "error"},
                    {"error",
                     QString("host '%1' is not dispatchable: %2").arg(hostArg, hostErr).toStdString()},
                    {"host", hostArg.toStdString()}}
                    .dump()
                    .c_str());
        }
        effectiveRegistry = hostReg;
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
    if (isFork) {
        if (parentAgent_ == nullptr) {
            return QStringLiteral(
                R"({"status":"error","error":"fork mode requires a bound parent agent"})");
        }
        if (messagesContainForkMarker(parentAgent_->getMessages())) {
            return QStringLiteral(
                R"({"status":"error","error":"forks cannot be nested; this context is already forked"})");
        }
    }

    if (effectiveLlm == nullptr || effectiveRegistry == nullptr) {
        return QStringLiteral(
            R"({"status":"error","error":"LLM service or tool registry not configured"})");
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

    /* Build child config. Two paths:
     *   - Named def: apply that def's restrictions / prompt body.
     *   - Fork:      reuse parent's rendered system prompt verbatim
     *                so the LLM cache stays warm; no allowlist /
     *                denylist / max_turns override; isSubAgent is
     *                still on so the recursion guard blocks the
     *                spawn-agent tool. */
    QSocAgentConfig childCfg = effectiveConfig;
    childCfg.isSubAgent      = true;
    if (isFork) {
        childCfg.systemPromptOverride = parentAgent_->buildSystemPromptWithMemory();
        childCfg.toolsAllow.clear();
        childCfg.toolsDeny.clear();
        childCfg.maxTurnsOverride = 0;
        childCfg.criticalReminder.clear();
    } else {
        childCfg.systemPromptOverride = def->promptBody;
        childCfg.toolsAllow           = def->toolsAllow;
        childCfg.toolsDeny            = def->toolsDeny;
        childCfg.maxTurnsOverride     = def->maxTurns;
        childCfg.criticalReminder     = def->criticalReminder;
        childCfg.autoLoadMemory       = def->injectMemory;
        childCfg.injectProjectMd      = def->injectProjectMd;
        if (!def->injectSkills) {
            childCfg.skillListing.clear();
        }
        if (!def->model.isEmpty()) {
            childCfg.modelId = def->model;
        }
    }
    if (!worktreePath.isEmpty()) {
        childCfg.projectPath = worktreePath;
    }

    /* Per-child LLMService: clone the live parent's service so the
     * child has its OWN streaming state (currentStreamReply,
     * streamCompleted, …). Without this, two concurrent sub-agents
     * trample each other's single-flight invariant. The clone
     * shares the same QSocConfig, so model + endpoint selection
     * stays in sync. */
    auto *childLlm = effectiveLlm->clone(nullptr);
    auto *child    = new QSocAgent(nullptr, childLlm, effectiveRegistry, childCfg);
    childLlm->setParent(child); /* tie LLM lifetime to child */
    /* planMode rides childCfg (copied from the parent). The shell safety
     * judge is a separate member, so hand it down too: a read-only
     * exploration child judges its own bash the same way. */
    if (childCfg.planMode && parentAgent_ != nullptr) {
        child->setBashSafetyJudge(parentAgent_->bashSafetyJudge());
    }
    /* def is null in fork mode; a fork inherits the parent context and
     * never opts into memory injection, so guard the deref. */
    if (memoryManager_ != nullptr && def != nullptr && def->injectMemory) {
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
        json forkedMessages = parentAgent_->getMessages();
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

    const QString effectiveType = isFork ? QStringLiteral("fork") : subagentType;
    const QString label         = description.isEmpty() ? effectiveType : description;
    const QString taskId        = taskSource_->registerRun(label, effectiveType, child);
    /* Stash isolation + worktree on the run so the meta sidecar
     * captures them; mirrors what the response JSON reports. */
    taskSource_->setIsolationMetadata(taskId, isolation, worktreePath);

    /* Forward child token usage into the parent's running totals so
     * the parent's status pill / cost view reflects total cost in
     * real time. Routes only the DELTA on each emission to avoid
     * double counting. */
    if (parentAgent_ != nullptr) {
        auto *parent  = parentAgent_;
        auto  prevIn  = std::make_shared<qint64>(0);
        auto  prevOut = std::make_shared<qint64>(0);
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
            }
        });

    /* Worktree cleanup hook captured by lambdas below. Empty path
     * = no isolation, helpers are no-ops. */
    auto wtCleanup = [parentRepoRoot, worktreePath]() {
        removeWorktreeAt(parentRepoRoot, worktreePath);
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
                    QString content = skillTool->readSkillContent(skill.path);
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

    /* Terminal-state wiring shared by foreground and background runs.
     * Order per the lifecycle invariant: state transition first
     * (markCompleted/markFailed), then the notification (delivery,
     * only once the run is backgrounded), then the hang-prone worktree
     * cleanup last so it never gates either. `notified` makes the
     * three terminal signals mutually single-shot; `parentGuard`
     * tolerates a parent that died before the child finished;
     * `backgrounded` is true from the start for an explicit background
     * spawn and flips true when a foreground run exceeds
     * autoBackgroundMs. A foreground run that finishes in time returns
     * its result inline and never notifies (the result is the tool's
     * return value, so a notification would double-report it). */
    QPointer<QSocAgent> parentGuard(parentAgent_);
    auto                notified     = std::make_shared<bool>(false);
    auto                backgrounded = std::make_shared<bool>(background);
    auto pushNotification            = [srcGuard,
                                        parentGuard,
                                        notified,
                                        backgrounded,
                                        taskId,
                                        effectiveType](const QString &status, const QString &body) {
        if (!*backgrounded || parentGuard.isNull() || *notified) {
            return;
        }
        *notified                    = true;
        const QString transcriptPath = srcGuard.isNull() ? QString()
                                                         : srcGuard->transcriptPathFor(taskId);
        parentGuard->queueTaskNotification(
            buildTaskNotification(taskId, effectiveType, status, body, transcriptPath));
    };
    QObject::connect(
        child,
        &QSocAgent::runComplete,
        taskSource_,
        [srcGuard, taskId, wtCleanup, pushNotification](const QString &finalText) {
            if (!srcGuard.isNull()) {
                srcGuard->markCompleted(taskId, finalText);
            }
            pushNotification(QStringLiteral("completed"), finalText);
            wtCleanup();
        });
    QObject::connect(
        child,
        &QSocAgent::runError,
        taskSource_,
        [srcGuard, taskId, wtCleanup, pushNotification](const QString &error) {
            if (!srcGuard.isNull()) {
                srcGuard->markFailed(taskId, error);
            }
            pushNotification(QStringLiteral("failed"), error);
            wtCleanup();
        });
    QObject::connect(
        child,
        &QSocAgent::runAborted,
        taskSource_,
        [srcGuard, taskId, wtCleanup, pushNotification](const QString &) {
            if (!srcGuard.isNull()) {
                srcGuard->markFailed(taskId, QStringLiteral("aborted"));
            }
            pushNotification(QStringLiteral("aborted"), QStringLiteral("aborted"));
            wtCleanup();
        });

    const json launchedResponse = json{
        {"status", "async_launched"},
        {"task_id", taskId.toStdString()},
        {"subagent_type", effectiveType.toStdString()},
        {"description", label.toStdString()},
        {"isolation", isolation.toStdString()},
        {"worktree", worktreePath.toStdString()}};

    /* The task source owns admission: start() runs the child now when a
     * slot is free, else queues it (Pending) until one frees. */
    auto launcher = [child, effectivePrompt]() { child->runStream(effectivePrompt); };

    if (background) {
        taskSource_->start(taskId, launcher);
        QSocTask::Row row;
        const bool    queued = taskSource_->findRow(taskId, &row)
                               && row.status == QSocTask::Status::Pending;
        json          resp   = launchedResponse;
        if (queued) {
            resp["status"] = "queued";
        }
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
    auto       stopFg = [&fgLoop, &fgTerminal, &fgBody](const QString &body) {
        if (fgTerminal) {
            return;
        }
        fgTerminal = true;
        fgBody     = body;
        fgLoop.quit();
    };
    QList<QMetaObject::Connection> fgConns;
    fgConns << QObject::connect(
        child, &QSocAgent::runComplete, &fgLoop, [&stopFg](const QString &text) { stopFg(text); });
    fgConns << QObject::connect(child, &QSocAgent::runError, &fgLoop, [&stopFg](const QString &err) {
        stopFg(err);
    });
    fgConns << QObject::connect(
        child, &QSocAgent::runAborted, &fgLoop, [&stopFg](const QString &partial) {
            stopFg(partial);
        });

    QTimer autoBg;
    autoBg.setSingleShot(true);
    QObject::connect(&autoBg, &QTimer::timeout, &fgLoop, [&fgLoop, backgrounded]() {
        *backgrounded = true;
        fgLoop.quit();
    });
    if (effectiveConfig.autoBackgroundMs > 0) {
        autoBg.start(effectiveConfig.autoBackgroundMs);
    }

    taskSource_->start(taskId, launcher);
    /* start() may run the child synchronously and the child may
     * terminate before returning (e.g. a blocking user_prompt_submit
     * hook makes runStream emit runError at once). That fires stopFg ->
     * fgLoop.quit() BEFORE exec(), and Qt resets the exit flag on
     * exec() entry, so the quit would be lost and exec() would block
     * forever. Only enter the loop if the run is still pending. */
    if (!fgTerminal) {
        fgLoop.exec();
    }

    /* Detach the local-loop steering before fgLoop / autoBg leave
     * scope. No event processing happens between exec() returning and
     * here, so no steered signal can fire in the gap. */
    for (const auto &conn : fgConns) {
        QObject::disconnect(conn);
    }
    autoBg.stop();

    if (*backgrounded) {
        /* Timed out: the child keeps running on the parent's event
         * loop. Its terminal state arrives later as a task
         * notification via the persistent handlers. */
        return QString::fromUtf8(launchedResponse.dump().c_str());
    }

    /* Finished within the window: the persistent handler already
     * transitioned task state and (because the run never backgrounded)
     * skipped the notification. Return the result inline as the tool's
     * value, preserving the legacy {"status":"ok","result":...}
     * contract regardless of the child's terminal flavor. */
    return QString::fromUtf8(
        json{
            {"status", "ok"},
            {"task_id", taskId.toStdString()},
            {"subagent_type", subagentType.toStdString()},
            {"result", fgBody.toStdString()},
            {"isolation", isolation.toStdString()},
            {"worktree", worktreePath.toStdString()}}
            .dump()
            .c_str());
}

void QSocToolAgent::abort()
{
    if (taskSource_ != nullptr) {
        taskSource_->abortAll();
    }
}
