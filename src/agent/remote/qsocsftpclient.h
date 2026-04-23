// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSFTPCLIENT_H
#define QSOCSFTPCLIENT_H

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

class QSocSshSession;

/**
 * @brief SFTP helper riding on top of an established SSH session.
 * @details Opens the SFTP subsystem on first use, exposes simple file and
 *          directory operations, and reports errors as user-safe strings
 *          (no secrets, no private-key contents). Writes go through a
 *          temporary path + atomic rename where the server supports it.
 */
class QSocSftpClient
{
public:
    /** @brief Directory entry returned by `listDir`. */
    struct Entry
    {
        QString name;
        qint64  size        = 0;
        bool    isDirectory = false;
        bool    isSymlink   = false;
    };

    explicit QSocSftpClient(QSocSshSession &session);
    ~QSocSftpClient();

    QSocSftpClient(const QSocSftpClient &)            = delete;
    QSocSftpClient &operator=(const QSocSftpClient &) = delete;

    /** @brief Open the SFTP subsystem. Returns false on failure. */
    bool open(QString *errorMessage = nullptr);

    /** @brief Tear down SFTP subsystem; safe to call repeatedly. */
    void close();

    bool isOpen() const { return m_sftp != nullptr; }

    /**
     * @brief Read a remote file.
     * @param maxBytes Hard cap on returned content; 0 means unlimited.
     */
    QByteArray readFile(const QString &path, qint64 maxBytes = 0, QString *errorMessage = nullptr);

    /** @brief Write content atomically via temp-file + rename. */
    bool writeFile(const QString &path, const QByteArray &content, QString *errorMessage = nullptr);

    /** @brief Recursive mkdir. Equivalent to `mkdir -p`. */
    bool mkdirP(const QString &path, QString *errorMessage = nullptr);

    /** @brief Rename (or move) a remote path. */
    bool rename(const QString &oldPath, const QString &newPath, QString *errorMessage = nullptr);

    /** @brief List a directory; limit caps returned entries, 0 means unlimited. */
    QList<Entry> listDir(const QString &path, int limit = 0, QString *errorMessage = nullptr);

    /** @brief Check whether a remote path exists (file or directory). */
    bool exists(const QString &path, QString *errorMessage = nullptr);

    /** @brief Most recent error, user-safe for logs. */
    QString lastError() const { return m_lastError; }

private:
    bool waitReady();
    void setError(const QString &msg, QString *sink);

    QSocSshSession &m_session;
    LIBSSH2_SFTP   *m_sftp = nullptr;
    QString         m_lastError;
};

#endif // QSOCSFTPCLIENT_H
