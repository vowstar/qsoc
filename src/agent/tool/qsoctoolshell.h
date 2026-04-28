// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSHELL_H
#define QSOCTOOLSHELL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

#include <QEventLoop>
#include <QMap>
#include <QProcess>
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
     * @brief Kill every background process tracked by bash_manage.
     * @details Iterates activeProcesses, sends kill + deletes QProcess objects,
     *          removes their capture files, and clears the map. Called on
     *          project switch so orphaned processes from the old project
     *          don't surface in the new session's bash_manage list.
     */
    static void killAllActive();

    /**
     * @brief Match a tail of bash output against common interactive prompts.
     * @details Public so a unit test can pin the regex without driving a
     *          real QProcess through a 45s no-progress window. Returns the
     *          matched prompt fragment (e.g. "(y/n)") or an empty string.
     */
    static QString detectInteractivePrompt(const QString &tail);

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

    static QProcess   *currentProcess;
    static QEventLoop *currentLoop;

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

private:
    static void    cleanupProcess(int processId);
    static QString collectOutput(int processId, int exitCode);
};

#endif // QSOCTOOLSHELL_H
