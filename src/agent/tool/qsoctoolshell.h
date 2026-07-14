// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSHELL_H
#define QSOCTOOLSHELL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

#include <QEventLoop>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QTimer>

/**
 * @brief Info for a background bash process that timed out but is still running
 */
struct QSocBashProcessInfo
{
    QProcess *process = nullptr;
    QString   outputPath;
    QString   command;
    qint64    startTime      = 0;
    qint64    maxOutputBytes = 0; /* 0 means "use default cap" */
    /* Watchdog state. lastKnownSize/lastSizeChangeAt feed both the
     * output-size kill rule and the no-progress stuck detector. */
    qint64  lastKnownSize    = 0;
    qint64  lastSizeChangeAt = 0;
    QString killReason;            /* set by watchdog when it kills the process */
    QString stuckReason;           /* set when stuck detector first matches */
    bool    stuckNotified = false; /* one-shot signal latch */
    /* Cleanup waits until the blocking bash_manage call returns. */
    int  activeWaiters  = 0;
    bool removeWhenIdle = false;
};

class QSocToolBashManage;

/**
 * @brief Tool to execute shell commands
 */
class QSocToolShellBash : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolShellBash(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolShellBash() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Stop background processes tracked by bash_manage.
     * @details Force-stops tracked QProcess objects and removes confirmed
     *          stopped entries. A process that does not stop remains tracked;
     *          entries observed by bash_manage remain valid until their active
     *          wait unwinds.
     */
    static void killAllActive();

    /**
     * @brief Match a tail of bash output against common interactive prompts.
     * @details Public so a unit test can pin the regex without driving a
     *          real QProcess through a 45s no-progress window. Returns the
     *          matched prompt fragment (e.g. "(y/n)") or an empty string.
     */
    static QString detectInteractivePrompt(const QString &tail);

    /**
     * @brief Snapshot of active background process info, sorted by id.
     * @details The task overlay reads this to render rows; consumers
     *          should not retain QProcess pointers because they belong
     *          to bashTool's static map and may be deleted on exit.
     *          Returns a list of {id, command, startedAtMs, outputPath,
     *          isStuck} tuples: a minimal projection so callers do not
     *          accidentally pin internal state.
     */
    struct BackgroundSnapshot
    {
        int     id;
        QString command;
        qint64  startedAtMs;
        QString outputPath;
        bool    isStuck;
        bool    isRunning;
    };
    static QList<BackgroundSnapshot> snapshotActive();
    static int                       activeProcessCount();
    static bool                      killActive(int processId);
    static QString                   tailActive(int processId, int maxBytes);

signals:
    /**
     * @brief Fired when a backgrounded bash process exits.
     * @details Either explicit `background: true` or timeout-handoff. The
     *          REPL uses this to surface a dim notification line; the
     *          process and its output stay in activeProcesses for the
     *          agent to inspect via bash_manage.
     */
    void backgroundProcessFinished(int processId, int exitCode, const QString &command);

    /**
     * @brief Fired once when a background process appears stuck.
     * @details "Stuck" means the capture file size has not grown for the
     *          stuck-window AND the tail of the file looks like an
     *          interactive prompt (y/n, password, "press enter"). Latched
     *          per process so the REPL gets one notification, not a flood.
     */
    void processStuckDetected(int processId, const QString &reason, const QString &tailHint);

private:
    QSocProjectManager *projectManager = nullptr;

    /* In-flight synchronous waits. A single shared slot would be
     * trampled when concurrent sub-agents each run a foreground bash
     * (one nested inside another's event loop), so abort() must reach
     * every live wait, not just the last one registered. Single-thread,
     * so a plain set is safe; entries are added before loop.exec() and
     * removed right after it returns (no event dispatch in between). */
    static QSet<QProcess *>   inFlightProcs_;
    static QSet<QEventLoop *> inFlightLoops_;

    static QMap<int, QSocBashProcessInfo> activeProcesses;
    static int                            nextProcessId;
    static QString                        readLastLines(const QString &path, int count);

    QTimer *watchdogTimer_ = nullptr;

    friend class QSocToolBashManage;

private slots:
    void tickWatchdog();
};

/**
 * @brief Tool to manage timed-out bash processes
 */
class QSocToolBashManage : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolBashManage(QObject *parent = nullptr);
    ~QSocToolBashManage() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

private:
    struct WaitContext
    {
        QEventLoop *loop    = nullptr;
        bool        aborted = false;
    };

    static QString collectOutput(const QSocBashProcessInfo &info, int exitCode);

    QSet<WaitContext *> activeWaits_;
};

#endif // QSOCTOOLSHELL_H
