// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshsession.h"

#include "agent/remote/qsoclibssh2init.h"

#include <libssh2.h>

#include <QDir>
#include <QFileInfo>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace {

int setNonBlocking(int sockFd)
{
    const int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);
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
    const int     dir = libssh2_session_block_directions(session);
    struct pollfd pfd{};
    pfd.fd = sockFd;
    if ((dir & LIBSSH2_SESSION_BLOCK_INBOUND) != 0) {
        pfd.events |= POLLIN;
    }
    if ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0) {
        pfd.events |= POLLOUT;
    }
    if (pfd.events == 0) {
        /* libssh2 did not give a direction hint yet; wait for either. */
        pfd.events = POLLIN | POLLOUT;
    }
    const int rc = poll(&pfd, 1, timeoutMs <= 0 ? -1 : timeoutMs);
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
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
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
        const QString msg = QStringLiteral("Failed to resolve %1: %2")
                                .arg(host, QString::fromLocal8Bit(gai_strerror(rc)));
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::NetworkError;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
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

    setNonBlocking(fd);
    m_socket = fd;
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
    LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
    if (agent == nullptr) {
        return false;
    }
    if (libssh2_agent_connect(agent) != 0) {
        libssh2_agent_free(agent);
        return false;
    }
    if (libssh2_agent_list_identities(agent) != 0) {
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        return false;
    }

    struct libssh2_agent_publickey *identity  = nullptr;
    struct libssh2_agent_publickey *prev      = nullptr;
    bool                            authOk    = false;
    const QByteArray                userBytes = user.toUtf8();
    while (true) {
        const int rc = libssh2_agent_get_identity(agent, &identity, prev);
        if (rc != 0 || identity == nullptr) {
            break;
        }
        if (libssh2_agent_userauth(agent, userBytes.constData(), identity) == 0) {
            authOk = true;
            break;
        }
        prev = identity;
    }

    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
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
    const QByteArray phBytes   = passphrase.toUtf8();
    int              rc        = 0;
    while ((rc = libssh2_userauth_publickey_fromfile_ex(
                m_session,
                userBytes.constData(),
                static_cast<unsigned int>(userBytes.size()),
                nullptr, /* pubkey: let libssh2 derive */
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

    if (!host.identitiesOnly && tryAgentAuth(user)) {
        return ConnectStatus::Ok;
    }
    for (const QString &identity : host.identityFiles) {
        if (tryIdentityFileAuth(user, identity, QString())) {
            return ConnectStatus::Ok;
        }
    }

    const QString msg = QStringLiteral(
        "Authentication failed using ssh-agent and configured IdentityFile");
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
