// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSUBAGENTTASKSOURCE_H
#define QSOCSUBAGENTTASKSOURCE_H

#include "agent/qsoctasksource.h"

#include <QList>
#include <QString>

class QSocAgent;

/**
 * @brief Task source backing in-process sub-agent runs.
 * @details The spawn-agent tool calls registerRun() before invoking
 *          the child, then feeds the child's progress signals
 *          through appendTranscript() and finalizes via
 *          markCompleted() or markFailed(). The overlay listing
 *          updates automatically through tasksChanged().
 *          The source owns each registered child agent and disposes
 *          of it during eviction.
 */
class QSocSubAgentTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocSubAgentTaskSource(QObject *parent = nullptr);
    ~QSocSubAgentTaskSource() override = default;

    QString              sourceTag() const override { return QStringLiteral("agent"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

    /**
     * @brief Register a new sub-agent run. The source takes ownership
     *        of the agent (reparents it). Status starts as Running.
     * @return Stable rolling id ("a1", "a2", ...) used by the spawn
     *         tool to feed progress.
     */
    QString registerRun(const QString &label, const QString &subagentType, QSocAgent *agent);

    /**
     * @brief Append text to the run's transcript buffer (kept ~64 KiB
     *        rolling); also bumps lastActivityMs.
     */
    void appendTranscript(const QString &id, const QString &chunk);

    /**
     * @brief Mark a run completed and stash the final assistant text.
     */
    void markCompleted(const QString &id, const QString &finalResult);

    /**
     * @brief Mark a run failed and stash the error message.
     */
    void markFailed(const QString &id, const QString &errorText);

    /**
     * @brief True when any registered run is currently Running.
     *        Kept for backward compat; new code should prefer
     *        countRunning() for capacity decisions.
     */
    bool hasActiveRun() const;

    /**
     * @brief How many runs are currently Running. Used by the
     *        spawn tool to enforce the maxConcurrentSubagents cap
     *        once per-child LLM service cloning has lifted the
     *        legacy single-flight constraint.
     */
    int countRunning() const;

    /**
     * @brief Number of runs currently tracked (any status).
     */
    int runCount() const;

    /**
     * @brief Forward abort() to every Running child. Used by the
     *        spawn-agent tool's abort() so a parent ESC cascades.
     */
    void abortAll();

    /**
     * @brief Find a row by id; returns false if no run with that id
     *        is currently tracked (either never registered, or
     *        already evicted).
     */
    bool findRow(const QString &id, QSocTask::Row *out) const;

    /**
     * @brief Elapsed seconds since a run started. Returns 0 when the
     *        id is unknown or has no startedAtMs.
     */
    qint64 elapsedSecondsFor(const QString &id) const;

    /**
     * @brief Sub-agent type label for an id; empty when unknown.
     */
    QString subagentTypeFor(const QString &id) const;

private:
    struct RunState
    {
        QString          id;
        QString          label;
        QString          subagentType;
        QSocAgent       *agent          = nullptr;
        QSocTask::Status status         = QSocTask::Status::Running;
        qint64           startedAtMs    = 0;
        qint64           lastActivityMs = 0;
        QString          transcript;  /* rolling, capped */
        QString          finalResult; /* on Completed */
        QString          errorText;   /* on Failed */
    };

    /* Drop Completed / Failed runs older than completionTtlMs_,
     * deleteLater()'ing their agents. Called whenever a new run is
     * registered, so the panel doesn't grow without bound. */
    void evictStaleCompleted();

    QList<RunState> runs_; /* preserves registration order; small N */
    int             nextSerial_      = 1;
    qint64          completionTtlMs_ = qint64{60} * 1000; /* 60 s lingering window */
    int             transcriptCap_   = 64 * 1024;
};

#endif /* QSOCSUBAGENTTASKSOURCE_H */
