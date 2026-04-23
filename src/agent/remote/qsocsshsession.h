// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHSESSION_H
#define QSOCSSHSESSION_H

#include "agent/remote/qsocsshhostconfig.h"

#include <libssh2.h>

#include <QObject>
#include <QString>

/**
 * @brief libssh2 session with TCP socket, known_hosts check, and auth chain.
 * @details Never reads SSH private key contents. IdentityFile paths are
 *          handed to `libssh2_userauth_publickey_fromfile_ex` which performs
 *          the file read inside the library. ssh-agent authentication is
 *          tried first when available.
 */
class QSocSshSession : public QObject
{
    Q_OBJECT

public:
    /** @brief Coarse outcome for `connectTo`. Logs avoid mentioning secrets. */
    enum class ConnectStatus {
        Ok,
        AlreadyConnected,
        NetworkError,
        HandshakeFailed,
        HostKeyMismatch,
        HostKeyNotFound,
        AuthFailed,
        Timeout,
    };

    explicit QSocSshSession(QObject *parent = nullptr);
    ~QSocSshSession() override;

    QSocSshSession(const QSocSshSession &)            = delete;
    QSocSshSession &operator=(const QSocSshSession &) = delete;

    /**
     * @brief Connect and authenticate using resolved host settings.
     * @param host Resolved host configuration (from QSocSshConfigParser).
     * @param errorMessage Optional sink for a user-safe error description.
     *                     Never contains passphrases or private key contents.
     */
    ConnectStatus connectTo(const QSocSshHostConfig &host, QString *errorMessage = nullptr);

    /** @brief Tear down the session and close the underlying socket. */
    void disconnectFromHost();

    bool isConnected() const { return m_session != nullptr; }

    /** @brief Raw libssh2 session handle. Valid only while isConnected(). */
    LIBSSH2_SESSION *rawSession() const { return m_session; }

    /** @brief Socket file descriptor. Returns -1 when not connected. */
    int socketFd() const { return m_socket; }

    /** @brief Last error text, safe for logs. Never contains secrets. */
    QString lastError() const { return m_lastError; }

    /** @brief Per-operation network timeout in milliseconds. Default 30000. */
    void setTimeoutMs(int ms);
    int  timeoutMs() const { return m_timeoutMs; }

    /**
     * @brief Wait helper around libssh2's EAGAIN direction hint.
     * @return 1 on ready, 0 on timeout, -1 on error.
     */
    static int waitSocket(int sockFd, LIBSSH2_SESSION *session, int timeoutMs);

private:
    ConnectStatus openSocket(const QString &host, int port, QString *errorMessage);
    ConnectStatus performHandshake(QString *errorMessage);
    ConnectStatus verifyHostKey(const QSocSshHostConfig &host, QString *errorMessage);
    ConnectStatus authenticate(const QSocSshHostConfig &host, QString *errorMessage);
    bool          tryAgentAuth(const QString &user);
    bool          tryIdentityFileAuth(
        const QString &user, const QString &privateKeyPath, const QString &passphrase);

    void clearConnection();
    void setError(const QString &msg);

    LIBSSH2_SESSION *m_session   = nullptr;
    int              m_socket    = -1;
    int              m_timeoutMs = 30000;
    QString          m_lastError;
};

#endif // QSOCSSHSESSION_H
