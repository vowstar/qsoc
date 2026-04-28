// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSHELL_H
#define QSOCTOOLSHELL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

#include <QEventLoop>
#include <QMap>
#include <QProcess>

/**
 * @brief Info for a background bash process that timed out but is still running
 */
struct QSocBashProcessInfo
{
    QProcess *process = nullptr;
    QString   outputPath;
    QString   command;
    qint64    startTime = 0;
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

signals:
    /**
     * @brief Fired when a backgrounded bash process exits.
     * @details Either explicit `background: true` or timeout-handoff. The
     *          REPL uses this to surface a dim notification line; the
     *          process and its output stay in activeProcesses for the
     *          agent to inspect via bash_manage.
     */
    void backgroundProcessFinished(int processId, int exitCode, const QString &command);

private:
    QSocProjectManager *projectManager = nullptr;

    static QProcess   *currentProcess;
    static QEventLoop *currentLoop;

    static QMap<int, QSocBashProcessInfo> activeProcesses;
    static int                            nextProcessId;
    static QString                        readLastLines(const QString &path, int count);

    friend class QSocToolBashManage;
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
