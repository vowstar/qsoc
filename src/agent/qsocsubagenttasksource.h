// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSUBAGENTTASKSOURCE_H
#define QSOCSUBAGENTTASKSOURCE_H

#include "agent/qsoctasksource.h"

#include <functional>
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
     *        of the agent (reparents it). Status starts as Pending;
     *        the run does not begin until start() supplies a launcher
     *        and a concurrency slot is free.
     * @return Stable rolling id ("a1", "a2", ...) used by the spawn
     *         tool to feed progress.
     */
    QString registerRun(const QString &label, const QString &subagentType, QSocAgent *agent);

    /**
     * @brief Attach the launcher that actually starts a registered
     *        run, then pump the queue. The run begins immediately when
     *        fewer than maxConcurrent() runs are active; otherwise it
     *        stays Pending (FIFO) until a slot frees. The launcher is
     *        invoked at most once, after the run is flipped to Running.
     *        No-op for unknown ids.
     */
    void start(const QString &id, std::function<void()> launcher);

    /**
     * @brief Set the sliding-window concurrency cap. Runs past the cap
     *        queue rather than fail. Values <= 0 mean unbounded (every
     *        queued run is admitted at once; flow control is left to the
     *        provider's rate limiting plus the agent loop's backoff).
     */
    void setMaxConcurrent(int maxConcurrent);

    /** @brief Current concurrency cap. */
    int maxConcurrent() const { return maxConcurrent_; }

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
     * @brief Queue a follow-up user message into a Running child's
     *        request queue. Returns true on success; false when the
     *        id is unknown or the run is already finished.
     * @details Used by the `send_message` tool. The child consumes
     *          the queued message at its next iteration boundary
     *          via `QSocAgent::queueRequest`.
     */
    bool queueRequestFor(const QString &id, const QString &message);

    /**
     * @brief Override the directory used to persist transcripts.
     *        Empty (default) routes to
     *        `<XDG_RUNTIME_DIR>/qsoc/agents/` with a temp-path
     *        fallback for non-Linux. Test-only.
     */
    void setTranscriptDir(const QString &dir) { transcriptDir_ = dir; }

    /**
     * @brief Resolve the JSONL transcript file path for a given run
     *        id (independent of whether the run is still tracked
     *        in-memory). Used by `tailFor` for evicted-run fallback.
     */
    QString transcriptPathFor(const QString &id) const;

    /**
     * @brief Resolve the metadata sidecar file path for a given run
     *        id. Stores `{task_id, label, subagent_type,
     *        started_at_ms, status, isolation, worktree, ...}`.
     */
    QString metaPathFor(const QString &id) const;

    /**
     * @brief Stash isolation + worktree path on a registered run so
     *        the meta sidecar reflects them. Spawn tool calls this
     *        right after registerRun(). No-op for unknown ids.
     */
    void setIsolationMetadata(
        const QString &id, const QString &isolation, const QString &worktreePath);

    /**
     * @brief One historical run reconstructed from disk meta sidecar.
     *        Distinct from in-memory RunState: no live agent pointer,
     *        meant for read-only listing and tail fallback.
     */
    struct HistoricalRun
    {
        QString id;
        QString label;
        QString subagentType;
        QString status; /* running / completed / failed / ... */
        qint64  startedAtMs  = 0;
        qint64  finishedAtMs = 0;
        QString isolation;
        QString worktreePath;
        QString error;
        QString finalPreview;
    };

    /**
     * @brief Scan the transcript directory for .meta.json sidecars
     *        and rebuild a list of past runs. Any meta with
     *        status="running" older than @p staleAgeSec is rewritten
     *        as failed (with reason "process restart") so a
     *        process-killed run never reappears as live.
     *        Returns the loaded list (also cached for
     *        historicalRuns()).
     */
    QList<HistoricalRun> loadHistoricalRuns(int staleAgeSec = 60 * 60);

    /**
     * @brief Cached historical runs from the most recent
     *        loadHistoricalRuns() call. Returns an empty list if
     *        loadHistoricalRuns has never been invoked.
     */
    QList<HistoricalRun> historicalRuns() const { return historical_; }

    /**
     * @brief Look up one historical run by id from the cache (or
     *        rescan disk if cache empty / id missing). Used by the
     *        agent_resume tool to find a prior run's metadata.
     *        Returns true on hit; populates `out`.
     */
    bool findHistoricalRun(const QString &id, HistoricalRun *out);

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
        QString               id;
        QString               label;
        QString               subagentType;
        QSocAgent            *agent          = nullptr;
        QSocTask::Status      status         = QSocTask::Status::Pending;
        qint64                queuedAtMs     = 0;
        qint64                startedAtMs    = 0;
        qint64                lastActivityMs = 0;
        QString               transcript;  /* rolling, capped */
        QString               finalResult; /* on Completed */
        QString               errorText;   /* on Failed */
        QString               isolation;   /* "none" | "worktree" */
        QString               worktreePath;
        std::function<void()> launcher; /* set by start(); fired by pumpQueue */
    };

    /* Promote Pending runs to Running (firing their launcher) while a
     * concurrency slot is free, in FIFO order. Called on start() and
     * whenever a run reaches a terminal state and frees a slot. */
    void pumpQueue();

    /* Drop Completed / Failed runs older than completionTtlMs_,
     * deleteLater()'ing their agents. Called whenever a new run is
     * registered, so the panel doesn't grow without bound. */
    void evictStaleCompleted();

    /** Resolve the on-disk directory transcripts are written to. */
    QString transcriptDir() const;

    /** Append a JSONL event {ts, kind, data} to the transcript
     *  file (best effort; failures are silent). */
    void appendDiskEvent(const QString &id, const QString &kind, const QString &data) const;

    /** Write the meta sidecar JSON file for a given run. */
    void writeMeta(const RunState &run) const;

    QList<RunState>      runs_; /* preserves registration order; small N */
    QList<HistoricalRun> historical_;
    int                  nextSerial_      = 1;
    int                  maxConcurrent_   = 0;                 /* sliding-window cap; 0=unbounded */
    bool                 pumping_         = false;             /* pumpQueue re-entry guard */
    qint64               completionTtlMs_ = qint64{60} * 1000; /* 60 s lingering window */
    int                  transcriptCap_   = 64 * 1024;
    QString              transcriptDir_; /* empty = compute from QStandardPaths */
};

#endif /* QSOCSUBAGENTTASKSOURCE_H */
