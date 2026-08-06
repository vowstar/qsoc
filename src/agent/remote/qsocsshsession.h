// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHSESSION_H
#define QSOCSSHSESSION_H

#include "agent/remote/qsocsshhostconfig.h"

#include <libssh2.h>

#include <cstdint>
#include <functional>
#include <QDeadlineTimer>
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

    /** @brief Why a session may no longer be used. */
    enum class Unusable : std::uint8_t {
        No,                /**< Usable. */
        AbandonedExchange, /**< A libssh2 request was left half-finished. */
        TransportDead,     /**< The socket is gone. */
    };

    /**
     * @brief Whether the session is open and still safe to issue calls on.
     * @details Answers usability, not "does this object own a session". Two
     *          distinct states make it false, and they are not the same
     *          thing: the socket may be gone, or it may be fine while
     *          libssh2's own state machine is stranded mid-request.
     */
    bool isConnected() const { return m_session != nullptr && m_unusable == Unusable::No; }

    Unusable unusableReason() const { return m_unusable; }

    /** @brief Reason text safe for logs, empty while the session is usable. */
    QString unusableText() const;

    /**
     * @brief Record that the socket underneath this session died.
     * @details One-way: only a fresh connect clears it.
     */
    void markTransportDead() { m_unusable = Unusable::TransportDead; }

    /**
     * @brief Record that an incomplete libssh2 request was abandoned.
     * @details libssh2 requires a call that returned EAGAIN to be retried
     *          with the same arguments until it finishes. Walking away at a
     *          deadline strands the protocol mid-request, so any later call
     *          on this session would read replies belonging to the one we
     *          gave up on. The link itself may be perfectly healthy, which
     *          is why this is not folded into the transport-dead flag.
     */
    void markAbandonedExchange()
    {
        if (m_unusable == Unusable::No) {
            m_unusable = Unusable::AbandonedExchange;
        }
    }

    bool isTransportDead() const { return m_unusable == Unusable::TransportDead; }

    /**
     * @brief Whether a libssh2 return code means the transport is gone.
     * @details Only the socket-level codes qualify. An SFTP protocol error
     *          or a permission denial says the link works and the request
     *          did not.
     */
    static bool isTransportError(int rc);

    /**
     * @brief Poison the session when @p rc is a transport-level error.
     * @details Every EAGAIN loop in the codebase ends by handing its final
     *          return code here, so a dropped link is recorded once,
     *          wherever libssh2 first noticed it, instead of only when
     *          poll happens to report the failure.
     * @return True when the session was poisoned.
     */
    bool notePossibleTransportError(int rc);

    /** @brief Raw libssh2 session handle. Valid only while isConnected(). */
    LIBSSH2_SESSION *rawSession() const { return m_session; }

    /**
     * @brief Socket handle. Returns -1 when not connected.
     * @details qintptr, not int: a Win64 SOCKET is a 64-bit UINT_PTR and
     *          truncating it produces a handle that no longer names the
     *          socket.
     */
    qintptr socketFd() const { return m_socket; }

    /** @brief Last error text, safe for logs. Never contains secrets. */
    QString lastError() const { return m_lastError; }

    /** @brief Per-operation network timeout in milliseconds. Default 30000. */
    void setTimeoutMs(int ms);
    int  timeoutMs() const { return m_timeoutMs; }

    /** @brief Outcome of one bounded wait on a session socket. */
    enum class WaitOutcome {
        Ready,   /**< The socket signalled the direction libssh2 asked for. */
        Timeout, /**< Nothing happened inside the slice; the link may be fine. */
        Fatal,   /**< The socket is in error or hung up: the transport is gone. */
    };

    /**
     * @brief Wait helper around libssh2's EAGAIN direction hint.
     * @details Timeout and Fatal are distinct on purpose: a slice that
     *          expires says nothing about link health, while POLLERR or a
     *          hangup means no later retry can succeed. Collapsing the two
     *          is what let a powered-off host loop forever.
     */
    static WaitOutcome waitSocket(qintptr sockFd, LIBSSH2_SESSION *session, int timeoutMs);

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
    /** @brief Wait bounded by @p deadline rather than by a fresh timeout. */
    WaitOutcome   waitWithin(qintptr sockFd, LIBSSH2_SESSION *session, QDeadlineTimer deadline);
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
    /* Signatures must match LIBSSH2_SEND_FUNC / LIBSSH2_RECV_FUNC exactly:
     * libssh2 calls these through a generic pointer, so a mismatched socket
     * width or return type is an ABI bug the compiler cannot see. */
    static ssize_t sendOverChannel(
        libssh2_socket_t socket, const void *buffer, size_t length, int flags, void **abstract);
    static ssize_t recvOverChannel(
        libssh2_socket_t socket, void *buffer, size_t length, int flags, void **abstract);

    bool tryPassphrasePrompt(
        const QString &user, const QStringList &identityPaths, QStringList *triedKeys);
    bool tryPasswordPrompt(const QString &user, const QString &hostname, int port);

    LIBSSH2_SESSION *m_session       = nullptr;
    qintptr          m_socket        = -1;
    Unusable         m_unusable      = Unusable::No;
    int              m_timeoutMs     = 30000;
    QSocSshSession  *m_parent        = nullptr; /* non-owning */
    LIBSSH2_CHANNEL *m_parentChannel = nullptr;
    QString          m_lastError;
    SecretCallback   m_secretCallback;
};

#endif // QSOCSSHSESSION_H
