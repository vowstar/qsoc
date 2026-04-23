// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLREMOTE_H
#define QSOCTOOLREMOTE_H

#include "agent/qsoctool.h"

class QSocSftpClient;
class QSocSshSession;
class QSocSshExec;
class QSocRemotePathContext;

/**
 * @brief Remote read_file. Same schema and name as the local tool.
 * @details Reads a remote file via SFTP. Relative paths resolve against the
 *          remote working directory in @ref QSocRemotePathContext.
 */
class QSocToolRemoteFileRead : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileRead(QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSftpClient        *m_sftp    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

/** @brief Remote write_file over SFTP (atomic temp+rename). */
class QSocToolRemoteFileWrite : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileWrite(QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSftpClient        *m_sftp    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

/** @brief Remote list_files via SFTP opendir/readdir. */
class QSocToolRemoteFileList : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileList(QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSftpClient        *m_sftp    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

/** @brief Remote edit_file: read, replace, atomically write back. */
class QSocToolRemoteFileEdit : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteFileEdit(QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSftpClient        *m_sftp    = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
};

/** @brief Remote bash: run a shell command over an SSH exec channel. */
class QSocToolRemoteShellBash : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemoteShellBash(QObject *parent, QSocSshSession *session, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

private:
    QSocSshSession        *m_session = nullptr;
    QSocRemotePathContext *m_pathCtx = nullptr;
    QSocSshExec           *m_running = nullptr;
};

/** @brief Remote path_context: report/change remote cwd, root, writable dirs. */
class QSocToolRemotePath : public QSocTool
{
    Q_OBJECT

public:
    QSocToolRemotePath(QObject *parent, QSocRemotePathContext *pathCtx);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocRemotePathContext *m_pathCtx = nullptr;
};

#endif // QSOCTOOLREMOTE_H
