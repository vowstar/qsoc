// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHSESSION_H
#define QSOCSSHSESSION_H

#include "agent/remote/qsocsshhostconfig.h"

#include <libssh2.h>

#include <cstdint>
#include <functional>
#include <QDeadlineTimer>
#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief libssh2 session with TCP socket, known_hosts check, and auth chain.
 * @details Never reads SSH private key contents. IdentityFile paths are
 *          handed to `libssh2_userauth_publickey_fromfile_ex` which performs
 *          the file read inside the library. ssh-agent authentication is
 *          tried first when available, over an agent connection this class
 *          owns rather than libssh2's, which is what makes it interruptible;
 *          only public key blobs and finished signatures cross that socket.
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
        Aborted, /**< The user stopped the connect; it did not run out of time. */
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
     * @brief Take ownership of a channel whose release could not finish.
     * @details libssh2 cannot free a channel the peer has not confirmed closed,
     *          and the peer does not confirm until the remote process exits, so
     *          a command that outlives its budget leaves one behind. Dropping it
     *          is not an option: it stays registered, so it keeps taking
     *          delivery of packets nobody reads until the session is freed, and
     *          nothing is left to retry the release with. Handing it here means
     *          the next command retries, by which time the confirmation has
     *          usually arrived.
     */
    void noteStrandedChannel(LIBSSH2_CHANNEL *channel);

    /** @brief How many channels are still waiting to be released. */
    int strandedChannelCount() const { return static_cast<int>(m_stranded.size()); }

    /**
     * @brief Retry the release of every channel handed to noteStrandedChannel.
     * @details One non-blocking attempt each, no wait: the free either finds
     *          the peer's close already delivered and completes, or says EAGAIN
     *          and keeps its place in the queue. Cheap enough to run before
     *          every command.
     */
    void releaseStrandedChannels();

    /**
     * @brief Whether the last teardown released everything on the live link.
     * @details False means libssh2 refused to free the session or a ProxyJump
     *          channel. A later teardown of the parent may reclaim a refused
     *          jump channel; this result reports only the attempt that ran.
     */
    bool lastTeardownReleasedState() const { return m_teardownReleased; }

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

    /**
     * @brief Install the predicate a connect-path wait consults every slice.
     * @details Connecting is a sequence of EAGAIN loops around one poll each,
     *          and a poll bounded only by the operation timeout is a wait the
     *          user cannot get out of. With a probe installed those polls run
     *          in slices and a stop is honoured within one of them.
     */
    void setAbortProbe(std::function<bool()> probe);

private:
    /** @brief Whether a stop the user asked for may cut a wait short. */
    enum class Interruptible : std::uint8_t {
        No,  /**< Teardown: giving up here leaks what it is releasing. */
        Yes, /**< Connecting: nothing is owned yet, so the caller can go. */
    };

    ConnectStatus openSocket(const QString &host, int port, QString *errorMessage);
    /**
     * @brief Wait bounded by @p deadline rather than by a fresh timeout.
     * @details @p interruptible has no default so every call site has to say
     *          which it is. A teardown wait that honoured an abort would
     *          abandon the disconnect or free it was draining.
     */
    WaitOutcome waitWithin(
        qintptr          sockFd,
        LIBSSH2_SESSION *session,
        QDeadlineTimer   deadline,
        Interruptible    interruptible);
    /** @brief Wait for authentication and record a fatal transport event. */
    bool waitForAuthentication(QDeadlineTimer deadline);
    /**
     * @brief Arm the one deadline every connect stage answers to.
     * @details A non-positive timeout means no deadline, matching the socket
     *          connect. Without the distinction `QDeadlineTimer(0)` would be
     *          born expired and "no timeout" would mean "fail immediately".
     */
    void armConnectDeadline();
    /** @brief Whether the connect budget is spent or the user asked to stop. */
    bool connectGaveUp() const;
    /** @brief Whether the connect stopped because the user asked, not the clock.
     *  @details Read live off the same probe connectGaveUp() consults, so a
     *           timeout and a cancellation report under distinct statuses. */
    bool connectAborted() const;
    /**
     * @brief Ask the caller for a secret without charging the wait to the link.
     * @details Typing time is not network time. Pushing the deadline out by
     *          however long the prompt was on screen is what lets one absolute
     *          deadline cover the whole connect and still allow a human to
     *          answer a passphrase prompt.
     */
    QString       promptSecret(const QString &prompt);
    ConnectStatus performHandshake(QString *errorMessage);
    ConnectStatus verifyHostKey(const QSocSshHostConfig &host, QString *errorMessage);
    ConnectStatus authenticate(const QSocSshHostConfig &host, QString *errorMessage);
    bool          tryAgentAuth(const QString &user, QDeadlineTimer deadline);
    bool          tryIdentityFileAuth(
        const QString &user, const QString &privateKeyPath, const QString &passphrase);

    void clearConnection();
    /**
     * @brief Drive one libssh2 teardown call to completion, bounded.
     * @details A non-blocking session or channel free can return EAGAIN with
     *          nothing done.
     * @return True only when the call returns zero before the deadline.
     */
    bool drainCall(qintptr sockFd, LIBSSH2_SESSION *session, const std::function<int()> &call);
    /** @brief Close the socket when it is ours; idempotent. */
    void closeOwnedSocket();
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

    LIBSSH2_SESSION      *m_session          = nullptr;
    qintptr               m_socket           = -1;
    Unusable              m_unusable         = Unusable::No;
    bool                  m_teardownReleased = true;
    int                   m_timeoutMs        = 30000;
    QSocSshSession       *m_parent           = nullptr; /* non-owning */
    LIBSSH2_CHANNEL      *m_parentChannel    = nullptr;
    QString               m_lastError;
    SecretCallback        m_secretCallback;
    std::function<bool()> m_abortProbe;
    /* The connect deadline starts before the blocking resolver; later stages
     * consume only the time it leaves. */
    QDeadlineTimer m_connectDeadline;
    /* Channels libssh2 would not release yet, oldest first. */
    QList<LIBSSH2_CHANNEL *> m_stranded;
};

#endif // QSOCSSHSESSION_H
