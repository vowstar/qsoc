// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENT_H
#define QSOCTOOLAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"

#include <memory>
#include <QMap>

class QLLMService;
class QSocAgent;
class QSocAgentDefinitionRegistry;
class QSocHookManager;
class QSocLoopScheduler;
class QSocMemoryManager;
class QSocSubAgentTaskSource;
class QSocHostCatalog;
class QSocSshConfigParser;

/**
 * @brief LLM-facing `agent` tool that spawns a child sub-agent.
 * @details Builds a fresh `QSocAgent` configured by the chosen
 *          `subagent_type` (loaded from
 *          `QSocAgentDefinitionRegistry`), shares the parent's
 *          `QLLMService`, `QSocToolRegistry`, hook / loop / memory
 *          managers, and routes the child through the
 *          `QSocSubAgentTaskSource` so it shows up in the Ctrl+B
 *          task overlay. Synchronous execution blocks until the
 *          child returns; asynchronous returns immediately with a
 *          `task_id` and pushes a `<task-notification>` into the
 *          parent's request queue when the child reaches a terminal
 *          state. Each child clones the parent's `QLLMService` for
 *          independent streaming state, so concurrent children are
 *          bounded only by `maxConcurrentSubagents`.
 */
class QSocToolAgent : public QSocTool
{
    Q_OBJECT

public:
    QSocToolAgent(
        QObject                     *parent,
        QLLMService                 *llmService,
        QSocToolRegistry            *parentRegistry,
        QSocAgentConfig              parentConfig,
        QSocAgentDefinitionRegistry *defRegistry,
        QSocSubAgentTaskSource      *taskSource);

    /**
     * @brief Inject the host catalog so `host` parameter resolves
     *        named SSH targets. When null, the tool only allows
     *        `host: "local"` (or the parent's current active host).
     */
    void setHostCatalog(QSocHostCatalog *catalog) { hostCatalog_ = catalog; }

    /**
     * @brief Inject a shared parsed `~/.ssh/config`. Used to mark
     *        which catalog aliases come from ssh-config in the
     *        host-list description.
     */
    void setSshConfigParser(QSocSshConfigParser *parser) { sshConfigParser_ = parser; }

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

    /* Optional managers to forward into the child agent so it sees
     * the same hook config / scheduler / memory the parent uses. */
    void setMemoryManager(QSocMemoryManager *manager) { memoryManager_ = manager; }
    void setHookManager(QSocHookManager *manager) { hookManager_ = manager; }
    void setLoopScheduler(QSocLoopScheduler *scheduler) { loopScheduler_ = scheduler; }

    /**
     * @brief Bind a live parent QSocAgent. When set, the spawn tool
     *        pulls the parent's CURRENT toolRegistry and config at
     *        execute() time instead of using the constructor-captured
     *        snapshot. Critical for remote-mode correctness: the
     *        parent's registry is swapped to a SSH-backed registry on
     *        `/remote`, and the child must inherit that swap rather
     *        than a stale local pointer.
     */
    void setParentAgent(QSocAgent *agent) { parentAgent_ = agent; }

    /**
     * @brief Accessor for the underlying definition registry. Used by
     *        the /agents slash command to enumerate definitions
     *        without plumbing a separate pointer through the REPL.
     */
    QSocAgentDefinitionRegistry *definitionRegistry() const { return defRegistry_; }

    /**
     * @brief Accessor for the sub-agent task source. The
     *        /agents-history slash uses this to read disk-backed
     *        meta sidecars without a separate handle.
     */
    QSocSubAgentTaskSource *taskSource() const { return taskSource_; }

    /**
     * @brief Sweep orphan sub-agent worktrees left behind by crashed
     *        runs. Walks `<TempLocation>/qsoc-worktrees/qsoc_wt_*`,
     *        removes any dir whose mtime is older than maxAgeSec.
     *        Returns the number of dirs removed. Safe to call from
     *        multiple processes (best-effort; git worktree remove
     *        is idempotent).
     */
    static int sweepStaleWorktrees(int maxAgeSec = 24 * 60 * 60);

    /**
     * @brief Build the model-visible `<task-notification>` envelope
     *        pushed into the parent's request queue when a background
     *        sub-agent reaches a terminal state. Carries the status,
     *        a capped result/error body, and the transcript path so
     *        the parent reads the full run on demand rather than
     *        inlining a large transcript. Stateless; exposed for
     *        format testing.
     * @param status One of "completed", "failed", "aborted".
     */
    static QString buildTaskNotification(
        const QString &taskId,
        const QString &subagentType,
        const QString &status,
        const QString &body,
        const QString &transcriptPath);

    /**
     * @brief Give a dispatched host's connection a way to re-establish itself.
     * @details Installs the rebuilder `reconnect()` needs, built from the same
     *          connect helpers `resolveHostBinding` used for the first bind.
     *          Without it a dropped host B connection can only answer
     *          `Refused`. Exposed for testing the wiring against a fake
     *          transport.
     */
    static void installBindingRecovery(QSocRemoteConnection *conn);

    /**
     * @brief Report a dispatched host's link health, recovering it if it can.
     * @details Empty when the host still answers, else a reason the workspace
     *          cannot serve calls. Spends one bounded round trip first, because
     *          a child waiting on the model has run no call to flip the
     *          session's liveness flag, so a silent host would otherwise read
     *          as live. A dropped link is reconnected in the same step.
     *          Exposed so the child's probe and its test share one path.
     */
    static QString probeBindingHealth(QSocRemoteConnection *conn);

    /**
     * @brief Route a binding's working-directory changes into a child's config.
     * @details The working directory lives on the binding; the child's config
     *          carries a copy of it into the system prompt and hook envelope.
     *          This observer is the only path between them, so a reconnect that
     *          rewinds the directory cannot leave the copy stale. Installed per
     *          child because the binding is shared across siblings.
     */
    static void bindChildCwd(QSocRemoteConnection *conn, QSocAgent *child);

private:
    /**
     * @brief One remote binding for a host alias, and everything built on it.
     * @details Shared, not cached: the cache holds one reference so sibling
     *          spawns to the same alias reuse the SSH session, and every child
     *          dispatched onto it holds another. Nothing here is in the
     *          QSocToolAgent's Qt tree, so the binding is free to outlive both
     *          the cache entry and the tool that opened it.
     */
    struct HostBinding
    {
        /* Staging only: adopt() drains it, so the transport has exactly one
         * owner and the two field sets can never both point at it. */
        AgentRemoteState state;
        /* Sole owner of the transport and the path context the binding's tools
         * resolve on every call, so the transport can be replaced without
         * rebuilding the registry. Lives here because it must outlive every
         * tool built from it. */
        QSocRemoteConnection conn;
        /* Parents the registry and every tool in it. Declared before registry
         * so the whole tool tree is gone before the connection those tools
         * resolve through. */
        std::unique_ptr<QObject> owner;
        /* Handle into owner's tree, not a second owner. */
        QSocToolRegistry *registry = nullptr;
    };

    /**
     * @brief One binding reference, held for exactly as long as one child.
     * @details Parented to the child, so the reference is released when the
     *          child object is destroyed. A dispatched child resolves its
     *          tools through the binding's registry on every call, so eviction
     *          may stop publishing a binding but must not free it.
     */
    class HostBindingHold : public QObject
    {
    public:
        HostBindingHold(QObject *parent, std::shared_ptr<HostBinding> binding)
            : QObject(parent)
            , binding_(std::move(binding))
        {}

    private:
        std::shared_ptr<HostBinding> binding_;
    };

    /**
     * @brief Resolve a host alias to the binding a child runs on. Opens an
     *        SSH session + remote registry on first use, publishes it for
     *        subsequent siblings, returns null on failure with a populated
     *        @p errorMessage.
     * @details The whole binding rather than its registry, because a child's
     *          tools, its config and its workspace health all have to name
     *          the same (target, workspace) pair.
     */
    std::shared_ptr<HostBinding> resolveHostBinding(const QString &host, QString *errorMessage);

    QLLMService                 *llmService_     = nullptr;
    QSocToolRegistry            *parentRegistry_ = nullptr;
    QSocAgentConfig              parentConfig_;
    QSocAgentDefinitionRegistry *defRegistry_     = nullptr;
    QSocSubAgentTaskSource      *taskSource_      = nullptr;
    QSocMemoryManager           *memoryManager_   = nullptr;
    QSocHookManager             *hookManager_     = nullptr;
    QSocLoopScheduler           *loopScheduler_   = nullptr;
    QSocAgent                   *parentAgent_     = nullptr;
    QSocHostCatalog             *hostCatalog_     = nullptr;
    QSocSshConfigParser         *sshConfigParser_ = nullptr;

    QMap<QString, std::shared_ptr<HostBinding>> hostCache_;
};

#endif /* QSOCTOOLAGENT_H */
