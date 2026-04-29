// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOOPSCHEDULER_H
#define QSOCLOOPSCHEDULER_H

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

class QLockFile;

/**
 * @brief Cron-based scheduler for `/loop` and `schedule_*` tools.
 * @details Holds a list of tasks (id + cron + prompt + recurring + durable).
 *          A 1s QTimer fires due tasks by emitting promptDue(). Reschedule
 *          is in-memory right after fire so a slow consumer cannot
 *          double-fire.
 *
 *          A 5-field cron string represents both recurring schedules
 *          (`*\/5 * * * *`) and one-shot pinned dates (`30 14 27 2 *`)
 *          uniformly, eliminating the interval/runAt tagged-union special
 *          case. One-shot tasks (recurring=false) are removed from the
 *          job list immediately after firing so they cannot fire again
 *          on the next tick.
 *
 *          Two persistence modes:
 *          1. Pure in-memory (no setProjectDir call): tasks live for the
 *             session only; isOwner() is always true.
 *          2. Durable (setProjectDir called): tasks with durable=true
 *             round-trip through `<dir>/.qsoc/loops.json`;
 *             `<dir>/.qsoc/loops.lock` makes one of N sessions the owner.
 *             Only the owner fires durable tasks AND only the owner
 *             accepts add/remove/clear; non-owner mutate is refused so
 *             the on-disk task list cannot drift behind the owner's
 *             in-memory copy. Tasks with durable=false live only in this
 *             session even when a project is bound.
 */
class QSocLoopScheduler : public QObject
{
    Q_OBJECT

public:
    struct Job
    {
        QString id;          /* 8-hex-char UUID slice */
        QString prompt;      /* verbatim prompt to dispatch */
        QString cron;        /* 5-field cron expression */
        bool    recurring;   /* true=fire on every match; false=fire once then erase */
        bool    durable;     /* true=persist to loops.json; false=session-only */
        qint64  createdAt;   /* epoch ms */
        qint64  lastFiredAt; /* 0 if never fired */
    };

    static constexpr int kMaxJobs = 50;

    explicit QSocLoopScheduler(QObject *parent = nullptr);
    ~QSocLoopScheduler() override;

    /**
     * @brief Bind to a project dir for durable storage + cross-session lock.
     * @details Creates `<dir>/.qsoc/loops.json` and `<dir>/.qsoc/loops.lock`.
     *          Loads any persisted durable tasks into memory. Only the
     *          lock owner fires; non-owner sessions stay quiet to avoid
     *          double-fire. Pass an empty string to operate purely in-memory.
     */
    void setProjectDir(const QString &dir);

    /**
     * @brief Whether this instance currently owns the on-disk fire lock.
     * @details Always true in pure in-memory mode (no project dir bound).
     */
    bool isOwner() const;

    /**
     * @brief Add a task. Returns its generated id, or empty string on refusal.
     * @param cron      5-field expression (validated; invalid -> empty return).
     * @param prompt    verbatim payload to fire (must be non-empty).
     * @param recurring true=fire on every cron match; false=fire once then erase.
     * @param durable   true=persist to loops.json; false=session-only memory.
     * @details Refusal cases:
     *          - Invalid cron expression.
     *          - Empty prompt.
     *          - kMaxJobs reached.
     *          - Durable mode + durable=true and caller is not the lock owner.
     *          - Durable persist write to loops.json failed; the in-memory
     *            append is rolled back so the caller does not see the task
     *            in listJobs() either.
     */
    QString addJob(const QString &cron, const QString &prompt, bool recurring, bool durable);

    /**
     * @brief Remove one task by id.
     * @return true if found and removed, false on no-such-id, non-owner
     *         refusal (only when removing a durable task in durable mode),
     *         or durable persist failure (in-memory remove rolled back).
     */
    bool removeJob(const QString &id);

    /**
     * @brief Remove all tasks (durable + session-only).
     * @return true on success, false on non-owner refusal (when there are
     *         durable tasks to clear) or durable persist failure (in-memory
     *         clear rolled back).
     */
    bool clearJobs();

    /**
     * @brief Snapshot of current tasks in registration order.
     * @details In durable mode, a non-owner session re-reads loops.json
     *          on every call so output reflects any owner-side mutate
     *          immediately. The owner's in-memory copy stays authoritative.
     *          Session-only (durable=false) tasks live in memory only and
     *          are not visible to peer sessions.
     */
    QList<Job> listJobs();

    /**
     * @brief Count of currently scheduled tasks (durable + session-only).
     * @details O(1) snapshot; safe to call from UI hot paths (status bar
     *          pill / task overlay header). Unlike listJobs() this does
     *          not re-read disk in non-owner mode, so it can drift by up
     *          to 1 tick behind a peer session's mutation.
     */
    int activeJobCount() const;

    /**
     * @brief Whether a scheduled prompt must go through CLI dispatch.
     * @details Returns true when the (trimmed) input begins with `/`
     *          (slash command) or `!` (shell escape). Free-form text can
     *          ride agent->queueRequest as a normal mid-turn message;
     *          CLI-special inputs cannot, otherwise they land in the LLM
     *          as literal text.
     */
    static bool scheduledInputRequiresCliDispatch(const QString &input);

    /**
     * @brief Parse the `/loop` argument string into (cron, prompt).
     * @details Rules (priority order, mirroring the claude-code loop skill):
     *          1. Leading token matches `^\d+[smhd]$` and converts cleanly
     *             via QSocCron::intervalToCron -> that is the schedule.
     *          2. Trailing "every Nx" or "every N <unit-word>" -> schedule.
     *          3. Otherwise the default cron (defaultCron) is used and the
     *             entire text is the prompt.
     *          Sets outErrorIfAny to a user-facing message and clears outCron
     *          when an interval token is recognized but does not divide its
     *          unit cleanly (e.g. `7m`, `90m`); the caller should surface
     *          the error and not call addJob.
     */
    static void parseLoopArgs(
        const QString &args,
        const QString &defaultCron,
        QString       &outCron,
        QString       &outPrompt,
        QString       &outErrorIfAny);

signals:
    /** @brief A task is due. The receiver must dispatch prompt verbatim. */
    void promptDue(const QString &prompt, const QString &jobId);

    /**
     * @brief Emitted whenever the job list changes shape (add / remove /
     *        clear / fire-and-erase one-shot).
     * @details Drives task overlays / status bar pills that need to
     *          re-render when the count or composition changes. Distinct
     *          from promptDue: a recurring fire updates lastFiredAt but
     *          does not change shape, so this signal is not emitted there.
     */
    void jobsChanged();

    /**
     * @brief Durable persist write failed during tick().
     * @details Latched: emitted once when fire-time persist() returns
     *          false, then suppressed until a later persist() succeeds
     *          (e.g. the disk recovered) so the REPL gets a single
     *          warning per failure window.
     */
    void persistFailed(const QString &path);

private slots:
    void tick();

private:
    static QString allocateId();

    /**
     * @brief Atomically rewrite loops.json from durable jobs in jobs_.
     * @return true on success, including the in-memory-only case where
     *         there is nothing to write. false only on open/write/commit
     *         failure under durable mode with at least one durable job.
     */
    bool    persist();
    void    loadFromDisk();
    bool    tryAcquireLock();
    void    releaseLock();
    QString lockPath() const;
    QString tasksPath() const;

    QTimer     timer_;
    QList<Job> jobs_;

    QString                    projectDir_;
    bool                       isOwner_ = true;
    std::unique_ptr<QLockFile> lockFile_;
    bool                       persistDegraded_ = false;
};

#endif /* QSOCLOOPSCHEDULER_H */
