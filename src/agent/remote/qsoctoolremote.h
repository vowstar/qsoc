// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLREMOTE_H
#define QSOCTOOLREMOTE_H

#include "agent/qsoctool.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocsshexec.h"

#include <optional>

class QLLMService;
class QSocRemoteConnection;
class QSocSftpClient;
class QSocSshSession;
class QSocSshExec;
class QSocRemotePathContext;
class QSocFileHistory;

/**
 * @brief Run a remote shell escape only from the currently verified cwd.
 * @param exitCode Set when the command reported an exit status.
 */
QString runBoundRemoteShellEscape(
    QSocRemoteConnection *conn, const QString &command, std::optional<int> *exitCode = nullptr);

/**
 * @brief Remote read_file. Same schema and name as the local tool.
 * @details Streams a remote file via SFTP. Relative paths resolve against the
 *          remote working directory in @ref QSocRemotePathContext. Image files
 *          take the local tool's attachment path; text is paged by line and
 *          each read buffers at most @ref kReadBytesLimit bytes.
 */
class QSocToolRemoteFileRead : public QSocTool
{
    Q_OBJECT

public:
    static constexpr qsizetype kReadBytesLimit = 16 * 1024 * 1024;

    QSocToolRemoteFileRead(
        QObject               *parent,
        QSocRemoteConnection  *conn,
        QSocRemotePathContext *pathCtx,
        QLLMService           *llm = nullptr);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QSocRemoteConnection  *m_conn    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
    QLLMService           *m_llm     = nullptr;
};

/** @brief Remote write_file over SFTP (atomic temp+rename). */
class QSocToolRemoteFileWrite : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileWrite(
        QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    /** @brief Wire the file-history checkpoint store (for rewind). */
    void setFileHistory(QSocFileHistory *history) { m_fileHistory = history; }

private:
    QSocRemoteConnection  *m_conn        = nullptr;
    QSocRemotePathContext *m_pathCtx     = nullptr;
    QSocFileHistory       *m_fileHistory = nullptr;
};

/** @brief Remote list_files via SFTP opendir/readdir. */
class QSocToolRemoteFileList : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileList(
        QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QSocRemoteConnection  *m_conn    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

/** @brief Remote edit_file: read, replace, atomically write back. */
class QSocToolRemoteFileEdit : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileEdit(
        QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    /** @brief Wire the file-history checkpoint store (for rewind). */
    void setFileHistory(QSocFileHistory *history) { m_fileHistory = history; }

private:
    QSocRemoteConnection  *m_conn        = nullptr;
    QSocRemotePathContext *m_pathCtx     = nullptr;
    QSocFileHistory       *m_fileHistory = nullptr;
};

/** @brief One background job a launch started, or why it did not. */
struct QSocRemoteJobStart
{
    QSocRemoteJobRecord record;        /**< Its jobId is set even on failure. */
    QString             jobDir;        /**< Remote job directory. */
    QString             failure;       /**< Text to hand back; empty on success. */
    bool                noted = false; /**< The ledger recorded the job. */
};

/**
 * @brief Launch @p command as a detached job in @p cwd over @p conn.
 * @details The one launch path for bash(background=true) and the monitor.
 *          A recorded job is watched by the connection's job watcher.
 * @param monitor Whether the monitor tool launched it.
 * @param ownerId Agent identity the job's completion is reported to.
 * @param running Set to the live exec while the launch runs, for abort.
 * @param maxOutputBytes Output cap the watcher enforces; 0 for none.
 */
QSocRemoteJobStart startRemoteJob(
    QSocRemoteConnection *conn,
    const QString        &cwd,
    const QString        &command,
    bool                  monitor,
    const QString        &ownerId,
    QSocSshExec         **running        = nullptr,
    qint64                maxOutputBytes = 0);

/** @brief Output cap of a remote bash job, matching the local default. */
constexpr qint64 kDefaultJobOutputBytes = qint64{5} * 1024 * 1024;

/** @brief Remote bash: run a shell command over an SSH exec channel. */
class QSocToolRemoteShellBash : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteShellBash(
        QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

private:
    /* Host time a wait script may overrun its own count before the call gives up. */
    static constexpr int kJobWaitGraceMs = 10000;
    static constexpr int kSignalExecMs   = 5000;

    /** @brief Run over the exec channel alone, with no job behind it. */
    QString runAttached(const QString &cmd, const QString &cwd, int timeoutMs);
    /** @brief Run as a job; one that outlives @p timeoutMs keeps running and is watched. */
    QString runAsJob(
        QSocRemoteJobStart *start, const QString &cmd, const QString &cwd, int timeoutMs);
    /** @brief Kill a job this call started; true when the host delivered the signal. */
    bool stopJob(const QSocRemoteJobRecord &record);

    QSocRemoteConnection  *m_conn          = nullptr;
    QSocRemotePathContext *m_pathCtx       = nullptr;
    QSocSshExec           *m_running       = nullptr;
    bool                   m_stopRequested = false;
};

/**
 * @brief Remote bash_manage: inspect and control backgrounded remote commands.
 * @details Jobs are directories under `<workspace>/.qsoc-agent/jobs/<id>/`
 *          created by the background mode of QSocToolRemoteShellBash. State
 *          lives in `pid`, `exit_code`, `output.log`, `command`. `kill -0`
 *          via SSH exec probes liveness.
 */
class QSocToolRemoteBashManage : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteBashManage(
        QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

private:
    static constexpr int kQueryMs     = 5000;
    static constexpr int kOutputMs    = 10000;
    static constexpr int kSignalMs    = 5000;
    static constexpr int kWaitGraceMs = 10000;

    QSocSshExec::Result runScript(const QString &script, int timeoutMs);
    QString             status(const QString &jobId, const QString &dir);
    QString             output(const QString &jobId, const QString &dir, int maxLines);
    QString             waitFor(const QString &jobId, const QString &dir, int timeoutMs);
    QString sendSignal(const QString &jobId, const QString &signalArg, bool *signalled = nullptr);
    QString terminate(const QString &jobId);

    QSocRemoteConnection  *m_conn    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
    QSocSshExec           *m_running = nullptr;
};

/** @brief Remote path_context: the local actions, over the remote workspace. */
class QSocToolRemotePath : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemotePath(QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QString listing() const;

    QSocRemoteConnection  *m_conn    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

#endif // QSOCTOOLREMOTE_H
