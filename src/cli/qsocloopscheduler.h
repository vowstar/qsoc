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
 * @brief Interval scheduler for /loop.
 * @details Holds a list of jobs (id + intervalMs + prompt). A 1s QTimer
 *          fires due jobs by emitting promptDue(). Reschedule is in-memory
 *          right after fire so a slow consumer cannot trigger double-fire.
 *
 *          Two modes:
 *          1. Pure in-memory (default, no setProjectDir call): jobs live
 *             for the session only; isOwner() is always true.
 *          2. Durable (setProjectDir called): jobs round-trip through
 *             `<dir>/.qsoc/loops.json`; `<dir>/.qsoc/loops.lock` makes
 *             one of N sessions the owner. Only the owner fires AND only
 *             the owner accepts add/remove/clear; non-owner mutate is
 *             refused so the on-disk job list cannot drift behind the
 *             owner's in-memory copy.
 */
class QSocLoopScheduler : public QObject
{
    Q_OBJECT

public:
    struct Job
    {
        QString name;             /* short id e.g. "a1b2" */
        QString prompt;           /* verbatim prompt to dispatch */
        qint64  intervalMs;       /* fire cadence */
        qint64  createdAt;        /* epoch ms */
        qint64  lastFiredAt;      /* 0 if never fired */
        qint64  nextFireAt;       /* epoch ms */
        bool    recurring = true; /* future-proof: always true today */
        bool    enabled   = true; /* disabled jobs persist but don't fire */
    };

    explicit QSocLoopScheduler(QObject *parent = nullptr);
    ~QSocLoopScheduler() override;

    /**
     * @brief Bind to a project dir for durable storage + cross-session lock.
     * @details Creates `<dir>/.qsoc/loops.json` and `<dir>/.qsoc/loops.lock`.
     *          Loads any persisted jobs into memory. Only the lock owner
     *          fires; non-owner sessions stay quiet to avoid double-fire.
     *          Pass an empty string to operate purely in-memory (default).
     */
    void setProjectDir(const QString &dir);

    /**
     * @brief Whether this instance currently owns the on-disk fire lock.
     * @details Always true in pure in-memory mode (no project dir bound).
     */
    bool isOwner() const;

    /**
     * @brief Add a recurring job. Returns its generated name, or empty
     *        string on refusal.
     * @details Refusal cases:
     *          - Durable mode and the caller is not the lock owner.
     *          - Durable persist write to loops.json failed; the
     *            in-memory append is rolled back so the caller does not
     *            see the job in listJobs() either.
     *          The first fire happens after intervalMs; the caller is
     *          expected to dispatch the prompt once immediately on its
     *          own.
     */
    QString addJob(const QString &prompt, qint64 intervalMs);

    /**
     * @brief Remove one job by name.
     * @return true if found and removed, false on:
     *         - no such job in the list,
     *         - durable mode and the caller is not the lock owner,
     *         - durable persist failure (in-memory remove rolled back).
     *         CLI callers that need to distinguish should listJobs()
     *         first to separate "not found" from the I/O paths.
     */
    bool removeJob(const QString &name);

    /**
     * @brief Remove all jobs.
     * @return true on success, false on non-owner refusal or durable
     *         persist failure (in-memory clear rolled back).
     */
    bool clearJobs();

    /**
     * @brief Snapshot of current jobs in registration order.
     * @details In durable mode, a non-owner session re-reads loops.json
     *          on every call so /loop list reflects any owner-side
     *          add/remove/clear immediately, including same-second
     *          edits that an mtime gate would miss. The owner's
     *          in-memory copy stays authoritative.
     */
    QList<Job> listJobs();

    /**
     * @brief Parse "5m", "30m", "2h", "1d", "30s" into milliseconds.
     * @details Seconds round up to whole minutes (minimum 60_000 ms).
     *          Returns 0 on parse error.
     */
    static qint64 parseInterval(const QString &token);

    /**
     * @brief Render an interval back as a short human token (e.g. "10m").
     */
    static QString formatInterval(qint64 intervalMs);

    /**
     * @brief Whether a scheduled prompt must go through CLI dispatch.
     * @details Returns true when the (trimmed) input begins with `/`
     *          (slash command) or `!` (shell escape). Free-form text
     *          can ride agent->queueRequest as a normal mid-turn
     *          message; CLI-special inputs cannot, otherwise they land
     *          in the LLM as literal text. Lifted to a static helper so
     *          a single source of truth covers REPL plumbing and tests.
     */
    static bool scheduledInputRequiresCliDispatch(const QString &input);

    /**
     * @brief Parse the /loop argument string into (intervalMs, prompt).
     * @details Rules (priority order, mirroring claude-code's loop skill):
     *          1. Leading token matches ^\d+[smhd]$ -> that is the interval.
     *          2. Trailing "every Nx" or "every N <unit-word>" -> interval.
     *          3. Otherwise default interval (defaultMs), entire text is prompt.
     *          On rule 1 the leading token is stripped; on rule 2 the trailing
     *          clause is stripped. Returns intervalMs == 0 if parsing failed.
     */
    static void parseLoopArgs(
        const QString &args, qint64 defaultMs, qint64 &outIntervalMs, QString &outPrompt);

signals:
    /** @brief A job is due. The lambda must dispatch prompt verbatim. */
    void promptDue(const QString &prompt, const QString &jobName);

    /**
     * @brief Durable persist write failed during tick().
     * @details Latched: emitted once when fire-time persist() returns
     *          false, then suppressed until a later persist() succeeds
     *          (e.g. the disk recovered) so the REPL gets a single
     *          warning per failure window instead of one per tick.
     *          Mutation paths (addJob/removeJob/clearJobs) surface
     *          their own persist failures via return values; this
     *          signal exists for the scheduler-driven path the user
     *          isn't actively driving.
     */
    void persistFailed(const QString &path);

private slots:
    void tick();

private:
    QString allocateName();
    /**
     * @brief Atomically rewrite loops.json from jobs_.
     * @return true on success, including the in-memory-only case where
     *         there is nothing to write. false only on open/write/commit
     *         failure under durable mode.
     * @details Mutate paths (addJob/removeJob/clearJobs) propagate
     *          this failure so the CLI cannot tell a user "Scheduled L2"
     *          when the file actually didn't change.
     */
    bool    persist();
    void    loadFromDisk();
    bool    tryAcquireLock();
    void    releaseLock();
    QString lockPath() const;
    QString tasksPath() const;

    QTimer     timer_;
    QList<Job> jobs_;
    int        nameCounter_ = 0;

    QString                    projectDir_;
    bool                       isOwner_ = true;
    std::unique_ptr<QLockFile> lockFile_;
    /* Latched between fire-time persist() failure and the next success
     * so persistFailed() emits once per outage, not once per tick. */
    bool persistDegraded_ = false;
};

#endif /* QSOCLOOPSCHEDULER_H */
