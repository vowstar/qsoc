// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENT_H
#define QSOCTOOLAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"

class QLLMService;
class QSocAgent;
class QSocAgentDefinitionRegistry;
class QSocHookManager;
class QSocLoopScheduler;
class QSocMemoryManager;
class QSocSubAgentTaskSource;

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
 *          `task_id`. A single-in-flight policy is enforced
 *          because the shared `QLLMService` cannot stream two
 *          conversations at once.
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
    ~QSocToolAgent() override = default;

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

private:
    QLLMService                 *llmService_     = nullptr;
    QSocToolRegistry            *parentRegistry_ = nullptr;
    QSocAgentConfig              parentConfig_;
    QSocAgentDefinitionRegistry *defRegistry_   = nullptr;
    QSocSubAgentTaskSource      *taskSource_    = nullptr;
    QSocMemoryManager           *memoryManager_ = nullptr;
    QSocHookManager             *hookManager_   = nullptr;
    QSocLoopScheduler           *loopScheduler_ = nullptr;
    QSocAgent                   *parentAgent_   = nullptr;
};

#endif /* QSOCTOOLAGENT_H */
