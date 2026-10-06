// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEJOBWATCHER_H
#define QSOCREMOTEJOBWATCHER_H

#include "agent/remote/qsocremotejobs.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <cstdint>

class QSocRemoteConnection;

/** @brief Host form of the directory that holds @p jobId on @p conn. */
QString remoteJobDir(const QSocRemoteConnection *conn, const QString &jobId);

/**
 * @brief Watches the background jobs of one remote binding.
 * @details One timer, running only while the ledger holds a live job. Each
 *          tick is one exec that reports the status of every live job and the
 *          new log bytes of every followed job. A tick is skipped while any
 *          blocking operation runs on the connection, and on a host with no
 *          shell nothing is ever polled.
 */
class QSocRemoteJobWatcher : public QObject
{
    Q_OBJECT

public:
    /** @brief What one @ref pollOnce did. */
    enum class Poll : std::uint8_t {
        Idle,    /**< Nothing to watch, or watching is off. */
        Skipped, /**< The connection was busy or unusable. */
        Failed,  /**< The poll exec did not complete. */
        Polled,  /**< The host answered for every live job. */
    };

    /** @brief One job a poll asks about. */
    struct Probe
    {
        QString             jobDir; /**< Host form of the job directory. */
        QSocRemoteJobRecord record;
        qint64              offset    = -1; /**< stdout offset of a followed job; -1 otherwise. */
        qint64              errOffset = -1; /**< stderr offset of a followed job. */
    };

    /** @brief What the host said about one job. */
    struct Report
    {
        QString    jobId;
        QString    status; /**< Status script output. */
        QByteArray log;    /**< New stdout bytes of a followed job. */
        QByteArray err;    /**< New stderr bytes of a followed job. */
        QByteArray tail;   /**< Last lines of an unfollowed job. */
    };

    static constexpr int    kFollowIntervalMs = 1000;
    static constexpr int    kIntervalMs       = 5000;
    static constexpr int    kBackoffMs        = 30000;
    static constexpr int    kExecTimeoutMs    = 3000;
    static constexpr qint64 kLogChunk         = 64 * 1024;
    static constexpr int    kTailLines        = 40;
    static constexpr int    kTailBytes        = 4000;

    explicit QSocRemoteJobWatcher(QSocRemoteConnection *conn, QObject *parent = nullptr);

    /** @brief Turn watching on or off; off by default. */
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /** @brief Stream the stdout and stderr of @p jobId through @ref jobOutput. */
    void follow(const QString &jobId);
    /** @brief Stop watching @p jobId; no further signal names it. */
    void forget(const QString &jobId);

    /** @brief Re-evaluate the timer after the ledger or the transport changed. */
    void kick();

    /** @brief Whether the timer is running. */
    bool isActive() const { return m_timer.isActive(); }
    int  intervalMs() const { return m_timer.interval(); }

    /** @brief Run one tick now; the timer calls this, and so may a test. */
    Poll pollOnce();

    /** @brief Last lines of an unfollowed job as of the latest poll. */
    QByteArray savedTail(const QString &jobId) const { return m_tails.value(jobId); }

    /** @brief Ticks skipped because the connection was busy or unusable. */
    int skippedPolls() const { return m_skipped; }

    /** @brief The one exec script a tick runs for @p probes. */
    static QString pollScript(const QList<Probe> &probes);
    /** @brief Split a poll's stdout into one report per complete section. */
    static QList<Report> parsePoll(const QByteArray &output);

signals:
    void jobOutput(const QString &jobId, const QByteArray &bytes, bool stderrStream);
    /** @brief A job ended; @p exitCode is -1 when the host lost it. */
    void jobSettled(const QString &jobId, int exitCode, const QByteArray &tail);

private:
    QList<Probe> probes() const;
    void         apply(const Report &report);
    void         settle(const QString &jobId, int exitCode, const QByteArray &tail);
    void         rearm();

    QSocRemoteConnection      *m_conn = nullptr;
    QTimer                     m_timer;
    QHash<QString, qint64>     m_offsets;    /* followed jobs, stdout */
    QHash<QString, qint64>     m_errOffsets; /* followed jobs, stderr */
    QHash<QString, QByteArray> m_tails;      /* unfollowed jobs */
    QHash<QString, int>        m_quiet;      /* ticks a job read "not running" with no exit code */
    QSet<QString>              m_done;       /* reported, never polled again */
    int                        m_failures = 0;
    int                        m_skipped  = 0;
    bool                       m_enabled  = false;
};

#endif // QSOCREMOTEJOBWATCHER_H
