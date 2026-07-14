// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLONGTASKMONITOR_H
#define QLONGTASKMONITOR_H

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <atomic>

class QTimer;

/**
 * @brief Watchdog that pairs an external event source with a polling timer.
 *
 * One-task lifecycle:
 *   construct -> start() -> notifyProgress()... -> finish() or cancel()
 *
 * The owning code drives the event source (a stream, signal, or RPC).
 * Every progress chunk calls notifyProgress(); finish() runs on normal
 * completion; cancel() runs on user abort or internal error. A QTimer
 * child ticks at Config::tickIntervalMs and emits stalled() when no
 * progress has arrived for stallThresholdMs (debounced by N consecutive
 * idle ticks), and wallClockExceeded() when the total run hits
 * wallClockMs. Both terminal calls are idempotent: a single
 * compare-and-swap on terminated_ guarantees one outcome wins and any
 * already-queued tick is a no-op afterwards.
 *
 * Threading: the monitor must live on a thread with a Qt event loop
 * (the timer needs one). start() asserts thread affinity.
 *
 * Signal handlers may destroy the monitor. cancel() tears down before emitting,
 * and timer callbacks access no members after emitting a signal.
 */
class QLongTaskMonitor : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Tuning knobs. Defaults mirror a long-running LLM stream.
     * @details Set wallClockMs to 0 to disable the hard cap;
     *          set stallThresholdMs to 0 to disable stall detection;
     *          consecutiveIdleTicks debounces transient idle so a
     *          single unlucky tick gap does not raise stalled().
     */
    struct Config
    {
        int tickIntervalMs       = 1000;   /* 1 s tick */
        int stallThresholdMs     = 30000;  /* 30 s of silence -> stall */
        int wallClockMs          = 300000; /* 5 min hard cap; 0 = off */
        int consecutiveIdleTicks = 3;      /* ticks of stall before emit */
    };

    /**
     * @brief Construct a monitor parented to @p parent.
     * @details The monitor is not running until start() is called.
     *          Destruction at any time is safe: the QTimer child is
     *          auto-stopped and deleted by Qt's object tree.
     */
    explicit QLongTaskMonitor(QObject *parent, const Config &cfg);

    /** Start the watchdog. Must be called from the owning thread. */
    void start();

    /** Record a fresh progress event. Resets stall accounting. */
    void notifyProgress();

    /** Normal completion path. Idempotent. Emits no terminal signal. */
    void finish();

    /**
     * @brief Cancellation path. Idempotent.
     * @details Emits cancelled(@p reason) exactly once (the first
     *          caller wins; later callers are no-ops).
     */
    void cancel(const QString &reason);

    /** True if cancel() ever latched. Stays true after teardown. */
    bool isCancelled() const noexcept;

    /** Wall-clock milliseconds since start(); 0 before start(). */
    qint64 elapsedMs() const;

signals:
    /**
     * @brief No progress for at least stallThresholdMs (debounced).
     * @param silentMs   ms since the last notifyProgress() / start()
     * @param idleStreak how many consecutive ticks saw the stall
     *
     * Emitted at most once per stall episode. A subsequent
     * notifyProgress() rearms the detector for the next episode.
     */
    void stalled(int silentMs, int idleStreak);

    /** Run exceeded wallClockMs. Emitted at most once. */
    void wallClockExceeded(int elapsedMs);

    /** Cancel was called. Emitted at most once. */
    void cancelled(QString reason);

private slots:
    void onTick();

private:
    void teardown();

    QTimer             *tick_;
    QAtomicInt          terminated_{0}; /* 1 once finish() or cancel() ran */
    QAtomicInt          cancelled_{0};  /* 1 if cancel() was the path */
    QAtomicInt          wallClockFired_{0};
    QAtomicInt          stalledFiredThisGap_{0};
    std::atomic<qint64> lastProgressMs_{0};
    QElapsedTimer       wall_;
    int                 idleStreak_ = 0;
    Config              cfg_;
};

#endif /* QLONGTASKMONITOR_H */
