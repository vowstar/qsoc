// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHSESSION_H
#define QSOCSSHSESSION_H

#include "agent/remote/qsocsshhostconfig.h"

#include <libssh2.h>

#include <functional>
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

    /**
     * @brief Connect through a parent session's direct-tcpip channel.
     * @details Implements one hop of ProxyJump: opens a direct-tcpip channel
     *          from @p parent to @p host, then runs the SSH handshake over
     *          that channel via libssh2 send/recv callbacks. @p parent must
     *          outlive this session because its TCP socket is the real
     *          transport underneath. Nest calls to chain multiple hops.
     */
    ConnectStatus connectToVia(
        const QSocSshHostConfig &host, QSocSshSession *parent, QString *errorMessage = nullptr);

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

    /**
     * @brief Callback invoked when auth needs an interactive secret
     *        (encrypted private-key passphrase or `password` method).
     * @details The session never logs, stores, or echoes the returned
     *          value. The callback's @p prompt is a UI-safe label like
     *          "Passphrase for id_ed25519:" or "Password for user@host:".
     *          Return an empty string to skip this auth attempt.
     */
    using SecretCallback = std::function<QString(const QString &prompt)>;

    /**
     * @brief Install an interactive-secret callback. When unset, the
     *        session never prompts: ssh-agent and empty-passphrase
     *        identity files are the only auth routes attempted. The
     *        sub-agent dispatch path leaves this unset so an
     *        in-flight LLM turn never blocks on user input.
     */
    void setSecretCallback(SecretCallback callback);

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

    /* libssh2 transport callbacks for ProxyJump tunneling. When a parent
     * channel is set, the child session's bytes ride on top of it instead
     * of the TCP socket. */
    static long long sendOverChannel(
        int sockFd, const void *buf, size_t len, int flags, void **abstract);
    static long long recvOverChannel(int sockFd, void *buf, size_t len, int flags, void **abstract);

    bool tryPassphrasePrompt(
        const QString &user, const QStringList &identityPaths, QStringList *triedKeys);
    bool tryPasswordPrompt(const QString &user, const QString &hostname, int port);

    LIBSSH2_SESSION *m_session       = nullptr;
    int              m_socket        = -1;
    int              m_timeoutMs     = 30000;
    QSocSshSession  *m_parent        = nullptr; /* non-owning */
    LIBSSH2_CHANNEL *m_parentChannel = nullptr;
    QString          m_lastError;
    SecretCallback   m_secretCallback;
};

#endif // QSOCSSHSESSION_H
