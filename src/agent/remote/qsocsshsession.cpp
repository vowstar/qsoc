// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshsession.h"

#include "agent/remote/qsoclibssh2init.h"
#include "agent/remote/qsocsshpubderive.h"

#include <libssh2.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_fd_t = SOCKET;
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_fd_t = int;
#endif

#include <cerrno>
#include <cstring>

namespace {

#ifdef Q_OS_WIN
constexpr socket_fd_t kInvalidSocket = INVALID_SOCKET;
#else
constexpr socket_fd_t kInvalidSocket = -1;
#endif

void closeSocketFd(socket_fd_t sockFd)
{
#ifdef Q_OS_WIN
    ::closesocket(sockFd);
#else
    ::close(sockFd);
#endif
}

int setNonBlocking(socket_fd_t sockFd)
{
#ifdef Q_OS_WIN
    u_long nonblocking = 1;
    return ::ioctlsocket(sockFd, FIONBIO, &nonblocking) == 0 ? 0 : -1;
#else
    const int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int pollSocket(socket_fd_t sockFd, short events, int timeoutMs)
{
#ifdef Q_OS_WIN
    WSAPOLLFD pfd{};
    pfd.fd     = sockFd;
    pfd.events = events;
    return ::WSAPoll(&pfd, 1, timeoutMs);
#else
    struct pollfd pfd{};
    pfd.fd     = sockFd;
    pfd.events = events;
    return ::poll(&pfd, 1, timeoutMs);
#endif
}

/* Materialize the public half of a private key alongside the private
 * file so libssh2 (on the mbedTLS backend, which cannot derive one in
 * memory) has a concrete path to feed to the server. The derivation
 * itself happens in QSocSshPubDerive via mbedTLS; we only write the
 * resulting line. Returns the .pub path on success, empty on failure. */
QString derivePubkeyPath(const QString &privateKeyPath)
{
    const QString pubPath = privateKeyPath + QStringLiteral(".pub");
    if (QFileInfo::exists(pubPath)) {
        return pubPath;
    }
    const QString line = QSocSshPubDerive::fromPrivateKeyFile(privateKeyPath);
    if (line.isEmpty()) {
        return {};
    }
    QFile file(pubPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write(line.toUtf8());
    file.write("\n");
    file.close();
    QFile::setPermissions(
        pubPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
            | QFileDevice::ReadOther);
    return pubPath;
}

QString libssh2ErrorString(LIBSSH2_SESSION *session)
{
    if (session == nullptr) {
        return {};
    }
    char *errMsg = nullptr;
    int   len    = 0;
    libssh2_session_last_error(session, &errMsg, &len, 0);
    if (errMsg != nullptr && len > 0) {
        return QString::fromLocal8Bit(errMsg, len);
    }
    return {};
}

} // namespace

QSocSshSession::QSocSshSession(QObject *parent)
    : QObject(parent)
{
    QSocLibSsh2Init::ensure();
}

QSocSshSession::~QSocSshSession()
{
    disconnectFromHost();
}

void QSocSshSession::setTimeoutMs(int ms)
{
    m_timeoutMs = ms;
}

int QSocSshSession::waitSocket(int sockFd, LIBSSH2_SESSION *session, int timeoutMs)
{
    if (sockFd < 0 || session == nullptr) {
        return -1;
    }
    const int dir    = libssh2_session_block_directions(session);
    short     events = 0;
    if ((dir & LIBSSH2_SESSION_BLOCK_INBOUND) != 0) {
        events |= POLLIN;
    }
    if ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0) {
        events |= POLLOUT;
    }
    if (events == 0) {
        events = POLLIN | POLLOUT;
    }
    const int rc
        = pollSocket(static_cast<socket_fd_t>(sockFd), events, timeoutMs <= 0 ? -1 : timeoutMs);
    if (rc > 0) {
        return 1;
    }
    if (rc == 0) {
        return 0;
    }
    return -1;
}

void QSocSshSession::setError(const QString &msg)
{
    m_lastError = msg;
}

void QSocSshSession::clearConnection()
{
    if (m_session != nullptr) {
        libssh2_session_disconnect(m_session, "QSoC shutting down session");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_parentChannel != nullptr) {
        /* Free the direct-tcpip channel on the parent session. The parent
         * owns its own socket and stays alive until the caller destroys
         * it; we only release the hop we opened. */
        libssh2_channel_free(m_parentChannel);
        m_parentChannel = nullptr;
    }
    /* Only close the socket when it is ours. Tunneled sessions borrow the
     * parent's fd for polling and must not close it. */
    if (m_parent == nullptr && m_socket >= 0) {
        closeSocketFd(static_cast<socket_fd_t>(m_socket));
    }
    m_socket = -1;
    m_parent = nullptr;
}

QSocSshSession::ConnectStatus QSocSshSession::openSocket(
    const QString &host, int port, QString *errorMessage)
{
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *result    = nullptr;
    const QByteArray hostBytes = host.toUtf8();
    const QByteArray portBytes = QByteArray::number(port);
    const int rc = getaddrinfo(hostBytes.constData(), portBytes.constData(), &hints, &result);
    if (rc != 0 || result == nullptr) {
#ifdef Q_OS_WIN
        const QString errText = QString::fromWCharArray(gai_strerror(rc));
#else
        const QString errText = QString::fromLocal8Bit(gai_strerror(rc));
#endif
        const QString msg = QStringLiteral("Failed to resolve %1: %2").arg(host, errText);
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::NetworkError;
    }

    socket_fd_t sockFd = kInvalidSocket;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        sockFd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sockFd == kInvalidSocket) {
            continue;
        }
        if (::connect(sockFd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        closeSocketFd(sockFd);
        sockFd = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (sockFd == kInvalidSocket) {
        const QString msg = QStringLiteral("TCP connect to %1:%2 failed: %3")
                                .arg(host)
                                .arg(port)
                                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::NetworkError;
    }

    setNonBlocking(sockFd);
    m_socket = static_cast<int>(sockFd);
    return ConnectStatus::Ok;
}

QSocSshSession::ConnectStatus QSocSshSession::performHandshake(QString *errorMessage)
{
    m_session = libssh2_session_init();
    if (m_session == nullptr) {
        const QString msg = QStringLiteral("libssh2 session init failed");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::HandshakeFailed;
    }

    libssh2_session_set_blocking(m_session, 0);

    int rc = 0;
    while ((rc = libssh2_session_handshake(m_session, m_socket)) == LIBSSH2_ERROR_EAGAIN) {
        if (waitSocket(m_socket, m_session, m_timeoutMs) <= 0) {
            const QString msg = QStringLiteral("SSH handshake timeout");
            setError(msg);
            if (errorMessage != nullptr) {
                *errorMessage = msg;
            }
            return ConnectStatus::Timeout;
        }
    }
    if (rc != 0) {
        const QString msg
            = QStringLiteral("SSH handshake failed: %1").arg(libssh2ErrorString(m_session));
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::HandshakeFailed;
    }

    /* Enable keepalive: libssh2 sends a ping if idle for 60s; remote drops
     * connection if idle too long otherwise. Non-fatal on failure. */
    libssh2_keepalive_config(m_session, 1, 60);
    return ConnectStatus::Ok;
}

QSocSshSession::ConnectStatus QSocSshSession::verifyHostKey(
    const QSocSshHostConfig &host, QString *errorMessage)
{
    if (host.strictHostKey == QSocSshHostConfig::StrictHostKey::No) {
        return ConnectStatus::Ok;
    }

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(m_session);
    if (kh == nullptr) {
        const QString msg = QStringLiteral("knownhost init failed");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::HostKeyNotFound;
    }

    QString khPath = host.userKnownHostsFile;
    if (khPath.isEmpty()) {
        khPath = QFileInfo(QStringLiteral("~/.ssh/known_hosts")).filePath();
        khPath = QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
    }
    libssh2_knownhost_readfile(kh, khPath.toLocal8Bit().constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t      keyLen  = 0;
    int         keyType = 0;
    const char *key     = libssh2_session_hostkey(m_session, &keyLen, &keyType);
    if (key == nullptr) {
        libssh2_knownhost_free(kh);
        const QString msg = QStringLiteral("Server did not present a host key");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::HostKeyNotFound;
    }

    int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (keyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        break;
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        typeMask |= LIBSSH2_KNOWNHOST_KEY_ED25519;
        break;
    default:
        break;
    }

    const QByteArray hostName = host.hostname.toUtf8();
    const int        check    = libssh2_knownhost_checkp(
        kh, hostName.constData(), host.port, key, keyLen, typeMask, nullptr);
    libssh2_knownhost_free(kh);

    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        return ConnectStatus::Ok;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH: {
        const QString msg
            = QStringLiteral("Host key mismatch for %1:%2").arg(host.hostname).arg(host.port);
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::HostKeyMismatch;
    }
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        if (host.strictHostKey == QSocSshHostConfig::StrictHostKey::AcceptNew) {
            return ConnectStatus::Ok;
        }
        setError(QStringLiteral("Host key not found in known_hosts for %1").arg(host.hostname));
        if (errorMessage != nullptr) {
            *errorMessage = m_lastError;
        }
        return ConnectStatus::HostKeyNotFound;
    default:
        setError(QStringLiteral("Host key check failed for %1").arg(host.hostname));
        if (errorMessage != nullptr) {
            *errorMessage = m_lastError;
        }
        return ConnectStatus::HostKeyNotFound;
    }
}

bool QSocSshSession::tryAgentAuth(const QString &user)
{
    /* libssh2's agent API is documented as blocking-only. When our session
     * runs in non-blocking mode (needed for waitSocket polling elsewhere)
     * the agent calls return EAGAIN and auth never completes. Flip to
     * blocking for the agent exchange and restore afterwards. */
    libssh2_session_set_blocking(m_session, 1);
    LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
    if (agent == nullptr) {
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }
    if (libssh2_agent_connect(agent) != 0) {
        libssh2_agent_free(agent);
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }
    if (libssh2_agent_list_identities(agent) != 0) {
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    struct libssh2_agent_publickey *identity  = nullptr;
    struct libssh2_agent_publickey *prev      = nullptr;
    bool                            authOk    = false;
    const QByteArray                userBytes = user.toUtf8();
    while (true) {
        const int next = libssh2_agent_get_identity(agent, &identity, prev);
        if (next != 0 || identity == nullptr) {
            break;
        }
        const int rc = libssh2_agent_userauth(agent, userBytes.constData(), identity);
        if (rc == 0) {
            authOk = true;
            break;
        }
        prev = identity;
    }

    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    libssh2_session_set_blocking(m_session, 0);
    return authOk;
}

bool QSocSshSession::tryIdentityFileAuth(
    const QString &user, const QString &privateKeyPath, const QString &passphrase)
{
    /* libssh2 reads the private key file internally; QSoC only supplies its
     * path. The passphrase is passed straight through without being copied
     * into any log or error message. */
    const QByteArray userBytes = user.toUtf8();
    const QByteArray keyBytes  = privateKeyPath.toUtf8();
    /* The mbedTLS crypto backend cannot derive a public key from an EC or
     * Ed25519 private-key file (NULL pubkey works only for classic PEM
     * RSA), so we ask ssh-keygen to emit the sibling .pub when missing.
     * ssh-keygen, not QSoC, is the one that reads the private-key bytes. */
    const QString    pubPath      = derivePubkeyPath(privateKeyPath);
    const QByteArray pubPathBytes = pubPath.toUtf8();
    const char      *pubArg       = pubPath.isEmpty() ? nullptr : pubPathBytes.constData();
    const QByteArray phBytes      = passphrase.toUtf8();
    int              rc           = 0;
    while ((rc = libssh2_userauth_publickey_fromfile_ex(
                m_session,
                userBytes.constData(),
                static_cast<unsigned int>(userBytes.size()),
                pubArg,
                keyBytes.constData(),
                phBytes.isEmpty() ? nullptr : phBytes.constData()))
           == LIBSSH2_ERROR_EAGAIN) {
        if (waitSocket(m_socket, m_session, m_timeoutMs) <= 0) {
            return false;
        }
    }
    return rc == 0;
}

QSocSshSession::ConnectStatus QSocSshSession::authenticate(
    const QSocSshHostConfig &host, QString *errorMessage)
{
    const QString user = host.user.isEmpty() ? QString::fromLocal8Bit(qgetenv("USER")) : host.user;
    if (user.isEmpty()) {
        const QString msg = QStringLiteral("No username available for SSH authentication");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::AuthFailed;
    }

    /* Try ssh-agent first whenever the config does not explicitly forbid it
     * via IdentitiesOnly + a concrete IdentityFile list. An agent may hold
     * keys that are absent on disk, and skipping it means losing that
     * route even when the user intended the agent to sign. */
    const bool restrictToFiles = host.identitiesOnly && !host.identityFiles.isEmpty();
    if (!restrictToFiles && tryAgentAuth(user)) {
        return ConnectStatus::Ok;
    }

    /* Identity file fallback: honour the config-supplied paths when present
     * (the user's explicit choice wins); otherwise enumerate the common
     * ~/.ssh/id_* names the way OpenSSH would, so first connections work
     * without bespoke config. Only paths are touched here; libssh2 reads
     * the key material internally during auth. */
    QStringList identityPaths = host.identityFiles;
    if (identityPaths.isEmpty()) {
        const QDir        sshDir(QDir::homePath() + QStringLiteral("/.ssh"));
        const QStringList entries
            = sshDir.entryList({QStringLiteral("id_*")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QString &name : entries) {
            if (name.endsWith(QStringLiteral(".pub"))) {
                continue;
            }
            identityPaths.push_back(sshDir.absoluteFilePath(name));
        }
    }

    QStringList triedKeys;
    for (const QString &identity : identityPaths) {
        if (tryIdentityFileAuth(user, identity, QString())) {
            return ConnectStatus::Ok;
        }
        triedKeys.append(QFileInfo(identity).fileName());
    }

    const QString hint = triedKeys.isEmpty()
                             ? QStringLiteral(" (no identity keys found in ~/.ssh)")
                             : QStringLiteral(" (tried: ") + triedKeys.join(QStringLiteral(", "))
                                   + QStringLiteral(")");
    const QString msg  = QStringLiteral("Authentication failed as %1@%2:%3%4")
                             .arg(user, host.hostname)
                             .arg(host.port)
                             .arg(hint);
    setError(msg);
    if (errorMessage != nullptr) {
        *errorMessage = msg;
    }
    return ConnectStatus::AuthFailed;
}

QSocSshSession::ConnectStatus QSocSshSession::connectTo(
    const QSocSshHostConfig &host, QString *errorMessage)
{
    if (isConnected()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Session is already connected");
        }
        return ConnectStatus::AlreadyConnected;
    }

    ConnectStatus status = openSocket(host.hostname, host.port, errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }

    status = performHandshake(errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }

    status = verifyHostKey(host, errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }

    status = authenticate(host, errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }

    return ConnectStatus::Ok;
}

void QSocSshSession::disconnectFromHost()
{
    clearConnection();
}

QSocSshSession::ConnectStatus QSocSshSession::connectToVia(
    const QSocSshHostConfig &host, QSocSshSession *parent, QString *errorMessage)
{
    if (isConnected()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Session is already connected");
        }
        return ConnectStatus::AlreadyConnected;
    }
    if (parent == nullptr || !parent->isConnected()) {
        const QString msg = QStringLiteral("Parent ProxyJump session is not connected");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::NetworkError;
    }

    m_parent = parent;
    /* Borrow the parent's socket for waitSocket polling; we never close it. */
    m_socket = parent->socketFd();

    const QByteArray hostBytes = host.hostname.toUtf8();
    LIBSSH2_CHANNEL *channel   = nullptr;
    LIBSSH2_SESSION *parentSes = parent->rawSession();
    while ((channel = libssh2_channel_direct_tcpip_ex(
                parentSes, hostBytes.constData(), host.port, "127.0.0.1", 0))
           == nullptr) {
        const int err = libssh2_session_last_errno(parentSes);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            const QString msg = QStringLiteral("Failed to open ProxyJump channel to %1:%2: %3")
                                    .arg(host.hostname)
                                    .arg(host.port)
                                    .arg(libssh2ErrorString(parentSes));
            setError(msg);
            if (errorMessage != nullptr) {
                *errorMessage = msg;
            }
            m_parent = nullptr;
            m_socket = -1;
            return ConnectStatus::NetworkError;
        }
        if (waitSocket(parent->socketFd(), parentSes, m_timeoutMs) <= 0) {
            const QString msg = QStringLiteral("Timed out opening ProxyJump channel");
            setError(msg);
            if (errorMessage != nullptr) {
                *errorMessage = msg;
            }
            m_parent = nullptr;
            m_socket = -1;
            return ConnectStatus::Timeout;
        }
    }
    m_parentChannel = channel;

    /* Build the child session and wire the send/recv callbacks before the
     * handshake so every byte flows through the jump channel. */
    m_session = libssh2_session_init();
    if (m_session == nullptr) {
        const QString msg = QStringLiteral("libssh2 session init failed");
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        clearConnection();
        return ConnectStatus::HandshakeFailed;
    }
    libssh2_session_set_blocking(m_session, 0);
    libssh2_session_callback_set2(
        m_session,
        LIBSSH2_CALLBACK_SEND,
        reinterpret_cast<libssh2_cb_generic *>(&QSocSshSession::sendOverChannel));
    libssh2_session_callback_set2(
        m_session,
        LIBSSH2_CALLBACK_RECV,
        reinterpret_cast<libssh2_cb_generic *>(&QSocSshSession::recvOverChannel));
    *libssh2_session_abstract(m_session) = this;

    int rc = 0;
    while ((rc = libssh2_session_handshake(m_session, m_socket)) == LIBSSH2_ERROR_EAGAIN) {
        if (waitSocket(m_socket, m_session, m_timeoutMs) <= 0) {
            const QString msg = QStringLiteral("SSH handshake over ProxyJump timed out");
            setError(msg);
            if (errorMessage != nullptr) {
                *errorMessage = msg;
            }
            clearConnection();
            return ConnectStatus::Timeout;
        }
    }
    if (rc != 0) {
        const QString msg = QStringLiteral("SSH handshake over ProxyJump failed: %1")
                                .arg(libssh2ErrorString(m_session));
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        clearConnection();
        return ConnectStatus::HandshakeFailed;
    }

    auto status = verifyHostKey(host, errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }
    status = authenticate(host, errorMessage);
    if (status != ConnectStatus::Ok) {
        clearConnection();
        return status;
    }
    return ConnectStatus::Ok;
}

long long QSocSshSession::sendOverChannel(
    int sockFd, const void *buf, size_t len, int flags, void **abstract)
{
    (void) sockFd;
    (void) flags;
    auto *self = static_cast<QSocSshSession *>(*abstract);
    if (self == nullptr || self->m_parentChannel == nullptr) {
        return -1;
    }
    const ssize_t n
        = libssh2_channel_write_ex(self->m_parentChannel, 0, static_cast<const char *>(buf), len);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return -EAGAIN;
    }
    if (n < 0) {
        return -1;
    }
    return n;
}

long long QSocSshSession::recvOverChannel(
    int sockFd, void *buf, size_t len, int flags, void **abstract)
{
    (void) sockFd;
    (void) flags;
    auto *self = static_cast<QSocSshSession *>(*abstract);
    if (self == nullptr || self->m_parentChannel == nullptr) {
        return -1;
    }
    const ssize_t n
        = libssh2_channel_read_ex(self->m_parentChannel, 0, static_cast<char *>(buf), len);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return -EAGAIN;
    }
    if (n < 0) {
        return -1;
    }
    return n;
}
