// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOOPSCHEDULER_H
#define QSOCLOOPSCHEDULER_H

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

/**
 * @brief Session-only interval scheduler for /loop.
 * @details Holds a list of jobs (id + intervalMs + prompt). A 1s QTimer
 *          fires due jobs by emitting promptDue(). Reschedule is in-memory
 *          right after fire so a slow consumer cannot trigger double-fire.
 *          No persistence, no cross-session lock.
 */
class QSocLoopScheduler : public QObject
{
    Q_OBJECT

public:
    struct Job
    {
        QString name;        /* short id e.g. "a1b2" */
        QString prompt;      /* verbatim prompt to dispatch */
        qint64  intervalMs;  /* fire cadence */
        qint64  createdAt;   /* epoch ms */
        qint64  lastFiredAt; /* 0 if never fired */
        qint64  nextFireAt;  /* epoch ms */
    };

    explicit QSocLoopScheduler(QObject *parent = nullptr);
    ~QSocLoopScheduler() override = default;

    /**
     * @brief Add a recurring job. Returns its generated name.
     * @details The first fire happens after intervalMs; the caller is
     *          expected to dispatch the prompt once immediately on its own.
     */
    QString addJob(const QString &prompt, qint64 intervalMs);

    /** @brief Remove one job by name. Returns true if found and removed. */
    bool removeJob(const QString &name);

    /** @brief Remove all jobs. */
    void clearJobs();

    /** @brief Snapshot of current jobs in registration order. */
    QList<Job> listJobs() const;

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

private slots:
    void tick();

private:
    QString allocateName();

    QTimer     timer_;
    QList<Job> jobs_;
    int        nameCounter_ = 0;
};

#endif /* QSOCLOOPSCHEDULER_H */
