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
/* Must follow winsock2.h: it defines SIO_KEEPALIVE_VALS. */
#include <mstcpip.h>
using socket_fd_t = SOCKET;
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
using socket_fd_t = int;
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

#ifdef Q_OS_WIN
constexpr socket_fd_t kInvalidSocket = INVALID_SOCKET;
#else
constexpr socket_fd_t kInvalidSocket = -1;
#endif

/* Teardown budget per libssh2 call. Bounded because a peer that stopped
 * answering must not park a caller in the close handshake. */
constexpr int kTeardownMs = 2000;

/* Slice length for one poll on the connect path. Short enough that a stop the
 * user asked for is honoured inside one, instead of after the whole attempt. */
constexpr int kPollSliceMs = 200;

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

int pollSocket(socket_fd_t sockFd, short events, int timeoutMs, short *revents = nullptr)
{
#ifdef Q_OS_WIN
    WSAPOLLFD pfd{};
#else
    struct pollfd pfd{};
#endif
    pfd.fd     = sockFd;
    pfd.events = events;
#ifdef Q_OS_WIN
    const int rc = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    const int rc = ::poll(&pfd, 1, timeoutMs);
#endif
    if (revents != nullptr) {
        *revents = pfd.revents;
    }
    return rc;
}

/* Ask the kernel to probe an idle connection so a peer that vanished
 * without sending FIN or RST (power cut, cable pull) surfaces as a socket
 * error instead of silence. Probes need an ACK, so this also catches a
 * black hole. Every setsockopt here is advisory: a kernel that rejects a
 * knob still gets the absolute deadlines above it. */
void enableKeepalive(socket_fd_t sockFd)
{
    constexpr int kIdleSec     = 15;
    constexpr int kIntervalSec = 5;
    constexpr int kProbeCount  = 3;

#ifdef Q_OS_WIN
    /* Windows takes the whole schedule in one ioctl and hard-codes the
     * probe count, so only idle and interval are ours to set. */
    struct tcp_keepalive vals{};
    vals.onoff             = 1;
    vals.keepalivetime     = kIdleSec * 1000;
    vals.keepaliveinterval = kIntervalSec * 1000;
    DWORD returned         = 0;
    (void) ::WSAIoctl(
        sockFd, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0, &returned, nullptr, nullptr);
#else
    constexpr int on = 1;
    (void) ::setsockopt(sockFd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
    (void) ::setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPIDLE, &kIdleSec, sizeof(kIdleSec));
#elif defined(TCP_KEEPALIVE)
    /* Apple spells the idle time TCP_KEEPALIVE. */
    (void) ::setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPALIVE, &kIdleSec, sizeof(kIdleSec));
#endif
#ifdef TCP_KEEPINTVL
    (void) ::setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPINTVL, &kIntervalSec, sizeof(kIntervalSec));
#endif
#ifdef TCP_KEEPCNT
    (void) ::setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPCNT, &kProbeCount, sizeof(kProbeCount));
#endif
#endif
}

/* Connect without parking the calling thread on the kernel's SYN retry
 * schedule, which can exceed two minutes. */
bool connectWithTimeout(
    socket_fd_t                  sockFd,
    const struct addrinfo       *addr,
    QDeadlineTimer               deadline,
    const std::function<bool()> &abortProbe)
{
    if (setNonBlocking(sockFd) != 0) {
        return false;
    }
    if (::connect(sockFd, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) {
        return true;
    }
#ifdef Q_OS_WIN
    if (::WSAGetLastError() != WSAEWOULDBLOCK) {
        return false;
    }
#else
    if (errno != EINPROGRESS) {
        return false;
    }
#endif
    /* A host that drops SYNs answers nothing here, so this poll is the other
     * place a stop has to be honoured. Sliced only when there is a probe to
     * consult, so a caller without one keeps the single bounded poll. */
    short        revents = 0;
    int          rc      = 0;
    const qint64 budget  = deadline.remainingTime();
    if (!abortProbe) {
        rc = pollSocket(sockFd, POLLOUT, budget < 0 ? -1 : static_cast<int>(budget), &revents);
    } else {
        while (true) {
            if (deadline.hasExpired() || abortProbe()) {
                return false;
            }
            const qint64 remaining = deadline.remainingTime();
            /* A negative remainder is "no deadline", not "no time left": poll
             * would read it as an infinite wait and put the probe out of
             * reach. */
            const int slice = remaining < 0
                                  ? kPollSliceMs
                                  : qMax(1, static_cast<int>(qMin<qint64>(remaining, kPollSliceMs)));
            rc = pollSocket(sockFd, POLLOUT, slice, &revents);
            if (rc != 0) {
                break;
            }
        }
    }
    if (rc <= 0) {
        return false;
    }
    /* WSAPoll does not report a failed connect in revents, so the SO_ERROR
     * read below is the authoritative check on every platform. */
    int soError = 0;
#ifdef Q_OS_WIN
    /* Winsock's getsockopt is documented as taking int* for the length and
     * char* for the value, not socklen_t*. */
    int optLen = static_cast<int>(sizeof(soError));
    if (::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &optLen)
        != 0) {
        return false;
    }
#else
    socklen_t optLen = sizeof(soError);
    if (::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &soError, &optLen) != 0) {
        return false;
    }
#endif
    if (soError != 0) {
        errno = soError;
        return false;
    }
    return true;
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

#ifndef Q_OS_WIN

/* One bounded poll on a descriptor we own, sliced so a stop the user asked for
 * is honoured inside a slice. Unlike waitSocket this takes the direction from
 * the caller: an ssh-agent link is not an SSH session and has no libssh2 block
 * hint to read. */
enum class LinkWait : std::uint8_t { Ready, Expired, Failed };

LinkWait waitFd(
    socket_fd_t                  sockFd,
    short                        events,
    QDeadlineTimer               deadline,
    const std::function<bool()> &abortProbe)
{
    while (true) {
        if (deadline.hasExpired()) {
            return LinkWait::Expired;
        }
        if (abortProbe && abortProbe()) {
            return LinkWait::Expired;
        }
        const qint64 remaining = deadline.remainingTime();
        const int    slice = remaining < 0
                                 ? kPollSliceMs
                                 : qMax(1, static_cast<int>(qMin<qint64>(remaining, kPollSliceMs)));
        short        revents = 0;
        const int    rc      = pollSocket(sockFd, events, slice, &revents);
        if (rc < 0) {
#ifndef Q_OS_WIN
            if (errno == EINTR) {
                continue;
            }
#endif
            return LinkWait::Failed;
        }
        if (rc == 0) {
            continue;
        }
        if ((revents & (POLLERR | POLLNVAL)) != 0) {
            return LinkWait::Failed;
        }
        if ((revents & POLLHUP) != 0 && (revents & POLLIN) == 0) {
            return LinkWait::Failed;
        }
        return LinkWait::Ready;
    }
}

/* ssh-agent wire protocol message numbers and signature flags, from the
 * SSH agent protocol. Only the two requests public-key auth needs are here. */
constexpr unsigned char kAgentcRequestIdentities = 11;
constexpr unsigned char kAgentIdentitiesAnswer   = 12;
constexpr unsigned char kAgentcSignRequest       = 13;
constexpr unsigned char kAgentSignResponse       = 14;
constexpr quint32       kAgentFlagRsaSha2_256    = 2;
constexpr quint32       kAgentFlagRsaSha2_512    = 4;

/* Caps on what an agent may claim, matching libssh2's own limits so a hostile
 * or confused agent cannot make us allocate without bound. */
constexpr quint32 kAgentMaxMsgLen     = 256 * 1024;
constexpr quint32 kAgentMaxIdentities = 1024;

void appendU32(QByteArray *out, quint32 value)
{
    const char bytes[4]
        = {static_cast<char>((value >> 24) & 0xFF),
           static_cast<char>((value >> 16) & 0xFF),
           static_cast<char>((value >> 8) & 0xFF),
           static_cast<char>(value & 0xFF)};
    out->append(bytes, 4);
}

void appendString(QByteArray *out, const QByteArray &value)
{
    appendU32(out, static_cast<quint32>(value.size()));
    out->append(value);
}

quint32 readU32(const unsigned char *data)
{
    return (static_cast<quint32>(data[0]) << 24) | (static_cast<quint32>(data[1]) << 16)
           | (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

/* Cursor over an SSH wire-format buffer. Every take* returns false rather than
 * reading past the end, so a truncated or lying agent reply fails the parse
 * instead of the process. */
class WireReader
{
public:
    explicit WireReader(const QByteArray &buffer)
        : m_buffer(buffer)
    {}

    bool takeByte(unsigned char *value)
    {
        if (m_pos + 1 > m_buffer.size()) {
            return false;
        }
        *value = static_cast<unsigned char>(m_buffer.at(m_pos));
        m_pos += 1;
        return true;
    }

    bool takeU32(quint32 *value)
    {
        if (m_pos + 4 > m_buffer.size()) {
            return false;
        }
        *value = readU32(reinterpret_cast<const unsigned char *>(m_buffer.constData()) + m_pos);
        m_pos += 4;
        return true;
    }

    bool takeString(QByteArray *value)
    {
        quint32 length = 0;
        if (!takeU32(&length)) {
            return false;
        }
        if (static_cast<qsizetype>(length) > m_buffer.size() - m_pos) {
            return false;
        }
        *value = m_buffer.mid(m_pos, static_cast<qsizetype>(length));
        m_pos += static_cast<qsizetype>(length);
        return true;
    }

    bool skipString()
    {
        QByteArray ignored;
        return takeString(&ignored);
    }

private:
    QByteArray m_buffer;
    qsizetype  m_pos = 0;
};

struct AgentIdentity
{
    QByteArray blob;
};

/**
 * @brief An ssh-agent connection bounded by a deadline and a stop.
 * @details libssh2 ships its own agent client, and it cannot be used here:
 *          `libssh2_session_set_blocking` is documented as leaving the socket
 *          alone, the agent backend creates its AF_UNIX socket in the default
 *          blocking mode, and the descriptor is not reachable through any
 *          public API. An agent that accepts the connection and then stops
 *          answering therefore parks the caller in `recv` with no deadline and
 *          nothing to interrupt it. Speaking the protocol on a socket we own is
 *          what puts the agent stage under the same budget as the handshake.
 */
class AgentLink
{
public:
    AgentLink(QDeadlineTimer deadline, std::function<bool()> abortProbe)
        : m_deadline(deadline)
        , m_abortProbe(std::move(abortProbe))
    {}

    ~AgentLink() { closeLink(); }

    AgentLink(const AgentLink &)            = delete;
    AgentLink &operator=(const AgentLink &) = delete;

    /** @brief Connect to $SSH_AUTH_SOCK. False when there is no agent to use. */
    bool open()
    {
        const QByteArray path = qgetenv("SSH_AUTH_SOCK");
        if (path.isEmpty()) {
            return false;
        }
        struct sockaddr_un addr{};
        if (static_cast<size_t>(path.size()) >= sizeof(addr.sun_path)) {
            return false;
        }
        m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return false;
        }
        if (setNonBlocking(m_fd) != 0) {
            closeLink();
            return false;
        }
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.constData(), static_cast<size_t>(path.size()));
        if (::connect(m_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            if (errno != EINPROGRESS) {
                closeLink();
                return false;
            }
            if (waitFd(m_fd, POLLOUT, m_deadline, m_abortProbe) != LinkWait::Ready) {
                closeLink();
                return false;
            }
            int       soError = 0;
            socklen_t optLen  = sizeof(soError);
            if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &optLen) != 0 || soError != 0) {
                closeLink();
                return false;
            }
        }
        return true;
    }

    /** @brief Every public key the agent holds, in the agent's own order. */
    bool listIdentities(QList<AgentIdentity> *out)
    {
        QByteArray request;
        request.append(static_cast<char>(kAgentcRequestIdentities));
        QByteArray response;
        if (!transact(request, &response)) {
            return false;
        }
        WireReader    reader(response);
        unsigned char kind  = 0;
        quint32       count = 0;
        if (!reader.takeByte(&kind) || kind != kAgentIdentitiesAnswer || !reader.takeU32(&count)) {
            return false;
        }
        if (count > kAgentMaxIdentities) {
            return false;
        }
        for (quint32 index = 0; index < count; ++index) {
            AgentIdentity identity;
            if (!reader.takeString(&identity.blob) || !reader.skipString()) {
                return false;
            }
            out->append(identity);
        }
        return true;
    }

    /**
     * @brief Have the agent sign @p data with the key @p blob names.
     * @details @p algorithm comes back as the agent named it, for the caller to
     *          hold against what libssh2 is about to write into the packet.
     */
    bool sign(
        const QByteArray &blob,
        const QByteArray &data,
        quint32           flags,
        QByteArray       *algorithm,
        QByteArray       *signature)
    {
        QByteArray request;
        request.append(static_cast<char>(kAgentcSignRequest));
        appendString(&request, blob);
        appendString(&request, data);
        appendU32(&request, flags);
        QByteArray response;
        if (!transact(request, &response)) {
            return false;
        }
        WireReader    reader(response);
        unsigned char kind = 0;
        if (!reader.takeByte(&kind) || kind != kAgentSignResponse) {
            return false;
        }
        /* The payload is one string holding the algorithm name and the
         * signature, so the outer length is skipped to read the two inside. */
        quint32 blobLen = 0;
        return reader.takeU32(&blobLen) && reader.takeString(algorithm)
               && reader.takeString(signature);
    }

private:
    bool transact(const QByteArray &request, QByteArray *response)
    {
        QByteArray framed;
        appendU32(&framed, static_cast<quint32>(request.size()));
        framed.append(request);
        if (!sendAll(framed.constData(), framed.size())) {
            return false;
        }
        unsigned char header[4] = {0, 0, 0, 0};
        if (!recvAll(reinterpret_cast<char *>(header), 4)) {
            return false;
        }
        const quint32 length = readU32(header);
        if (length == 0 || length > kAgentMaxMsgLen) {
            return false;
        }
        response->resize(static_cast<qsizetype>(length));
        return recvAll(response->data(), static_cast<qsizetype>(length));
    }

    bool sendAll(const char *data, qsizetype length)
    {
        qsizetype done = 0;
        while (done < length) {
            const ssize_t written = ::send(m_fd, data + done, static_cast<size_t>(length - done), 0);
            if (written > 0) {
                done += written;
                continue;
            }
            if (written == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
            if (waitFd(m_fd, POLLOUT, m_deadline, m_abortProbe) != LinkWait::Ready) {
                return false;
            }
        }
        return true;
    }

    bool recvAll(char *data, qsizetype length)
    {
        qsizetype done = 0;
        while (done < length) {
            const ssize_t read = ::recv(m_fd, data + done, static_cast<size_t>(length - done), 0);
            if (read > 0) {
                done += read;
                continue;
            }
            /* Zero is the agent hanging up mid-message: no later retry can
             * finish this reply, and treating it as "try again" is how a closed
             * socket becomes a spin. */
            if (read == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
            if (waitFd(m_fd, POLLIN, m_deadline, m_abortProbe) != LinkWait::Ready) {
                return false;
            }
        }
        return true;
    }

    void closeLink()
    {
        if (m_fd >= 0) {
            closeSocketFd(m_fd);
        }
        m_fd = kInvalidSocket;
    }

    socket_fd_t           m_fd = kInvalidSocket;
    QDeadlineTimer        m_deadline;
    std::function<bool()> m_abortProbe;
};

/* What the sign callback needs: the link to ask, and which key to ask about. */
struct AgentSignContext
{
    AgentLink *link = nullptr;
    QByteArray blob;
};

/**
 * @brief The algorithm libssh2 settled on, read out of the bytes to be signed.
 * @details The signature has to name the same algorithm libssh2 is about to
 *          write into the userauth packet, and that choice depends on the
 *          server's advertised signature algorithms, which no public API
 *          exposes. It does not have to be guessed: the blob handed to the
 *          callback is the session id followed by the userauth request itself,
 *          and the request carries the algorithm name as its sixth field.
 */
QByteArray methodFromSignData(const unsigned char *data, size_t length)
{
    const QByteArray view = QByteArray::fromRawData(
        reinterpret_cast<const char *>(data), static_cast<qsizetype>(length));
    WireReader    reader(view);
    unsigned char ignored = 0;
    QByteArray    method;
    if (!reader.skipString()          /* session id */
        || !reader.takeByte(&ignored) /* SSH_MSG_USERAUTH_REQUEST */
        || !reader.skipString()       /* user name */
        || !reader.skipString()       /* "ssh-connection" */
        || !reader.skipString()       /* "publickey" */
        || !reader.takeByte(&ignored) /* signature-follows flag */
        || !reader.takeString(&method)) {
        return {};
    }
    return method;
}

/* Signature fixed by LIBSSH2_USERAUTH_PUBLICKEY_SIGN_FUNC: libssh2 calls this
 * through a function pointer, so the shape has to match exactly. */
int agentSignCallback(
    LIBSSH2_SESSION     *session,
    unsigned char      **sig,
    size_t              *sigLen,
    const unsigned char *data,
    size_t               dataLen,
    void               **abstract)
{
    (void) session;
    if (abstract == nullptr || *abstract == nullptr) {
        return -1;
    }
    auto *context = static_cast<AgentSignContext *>(*abstract);
    if (context->link == nullptr) {
        return -1;
    }
    const QByteArray method = methodFromSignData(data, dataLen);
    if (method.isEmpty()) {
        return -1;
    }
    quint32 flags = 0;
    if (method == "rsa-sha2-512") {
        flags = kAgentFlagRsaSha2_512;
    } else if (method == "rsa-sha2-256") {
        flags = kAgentFlagRsaSha2_256;
    }
    QByteArray       algorithm;
    QByteArray       signature;
    const QByteArray payload
        = QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(dataLen));
    if (!context->link->sign(context->blob, payload, flags, &algorithm, &signature)) {
        return -1;
    }
    /* An agent that signed with something else than libssh2 asked for is not a
     * failure: libssh2 answers this code by retrying the key with its default
     * algorithm. */
    if (algorithm != method) {
        return LIBSSH2_ERROR_ALGO_UNSUPPORTED;
    }
    /* libssh2 releases this with the session allocator, which is the default
     * malloc/free pair because the session was made by libssh2_session_init. */
    void *owned = std::malloc(static_cast<size_t>(signature.size()));
    if (owned == nullptr) {
        return -1;
    }
    std::memcpy(owned, signature.constData(), static_cast<size_t>(signature.size()));
    *sig    = static_cast<unsigned char *>(owned);
    *sigLen = static_cast<size_t>(signature.size());
    return 0;
}

#endif // !Q_OS_WIN

/* libssh2's own socket type: int on POSIX, SOCKET on Windows. m_socket is
 * stored wide enough for both. */
libssh2_socket_t nativeSocket(qintptr sockFd)
{
    return static_cast<libssh2_socket_t>(sockFd);
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

bool QSocSshSession::isTransportError(int rc)
{
    switch (rc) {
    case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_TIMEOUT:
    case LIBSSH2_ERROR_BAD_SOCKET:
        return true;
    default:
        return false;
    }
}

bool QSocSshSession::notePossibleTransportError(int rc)
{
    if (!isTransportError(rc)) {
        return false;
    }
    markTransportDead();
    return true;
}

void QSocSshSession::armConnectDeadline()
{
    m_connectDeadline = m_timeoutMs <= 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                                         : QDeadlineTimer(m_timeoutMs);
}

bool QSocSshSession::connectGaveUp() const
{
    return m_connectDeadline.hasExpired() || (m_abortProbe && m_abortProbe());
}

QString QSocSshSession::promptSecret(const QString &prompt)
{
    if (!m_secretCallback) {
        return {};
    }
    /* Frozen, not extended: remainingTime() reports zero once expired, so
     * adding the time on screen to it would hand a spent budget a fresh one. */
    const qint64  before = m_connectDeadline.remainingTime();
    const QString secret = m_secretCallback(prompt);
    if (before > 0) {
        m_connectDeadline = QDeadlineTimer(before);
    }
    return secret;
}

void QSocSshSession::noteStrandedChannel(LIBSSH2_CHANNEL *channel)
{
    if (channel != nullptr) {
        m_stranded.append(channel);
    }
}

void QSocSshSession::releaseStrandedChannels()
{
    if (m_session == nullptr) {
        m_stranded.clear();
        return;
    }
    QList<LIBSSH2_CHANNEL *> keep;
    for (LIBSSH2_CHANNEL *channel : m_stranded) {
        if (libssh2_channel_free(channel) == LIBSSH2_ERROR_EAGAIN) {
            keep.append(channel);
        }
    }
    m_stranded = keep;
}

QString QSocSshSession::unusableText() const
{
    switch (m_unusable) {
    case Unusable::No:
        return {};
    case Unusable::AbandonedExchange:
        return QStringLiteral(
            "SSH session was left mid-request after a timeout and cannot be reused");
    case Unusable::TransportDead:
        return QStringLiteral("SSH transport is dead: the remote host stopped responding");
    }
    return {};
}

QSocSshSession::WaitOutcome QSocSshSession::waitWithin(
    qintptr sockFd, LIBSSH2_SESSION *session, QDeadlineTimer deadline, Interruptible interruptible)
{
    const bool abortable = interruptible == Interruptible::Yes && static_cast<bool>(m_abortProbe);
    while (true) {
        if (deadline.hasExpired()) {
            return WaitOutcome::Timeout;
        }
        if (abortable && m_abortProbe()) {
            return WaitOutcome::Timeout;
        }
        const qint64 remaining = deadline.remainingTime();
        /* A negative remainder is "never expires", not "no time left". Reading
         * it as a poll timeout would wait forever and put the probe out of
         * reach; reading it as a length would poll in 1 ms slices. */
        const int slice
            = abortable ? (remaining < 0
                               ? kPollSliceMs
                               : qMax(1, static_cast<int>(qMin<qint64>(remaining, kPollSliceMs))))
                        : (remaining < 0 ? -1 : qMax(1, static_cast<int>(remaining)));
        const auto outcome = waitSocket(sockFd, session, slice);
        if (outcome == WaitOutcome::Ready) {
            return deadline.hasExpired() ? WaitOutcome::Timeout : WaitOutcome::Ready;
        }
        if (outcome == WaitOutcome::Fatal) {
            return WaitOutcome::Fatal;
        }
        /* A slice expired. Without a probe to consult that is the whole
         * budget, because the slice was the budget. */
        if (!abortable) {
            return WaitOutcome::Timeout;
        }
    }
}

QSocSshSession::WaitOutcome QSocSshSession::waitSocket(
    qintptr sockFd, LIBSSH2_SESSION *session, int timeoutMs)
{
    if (sockFd < 0 || session == nullptr) {
        return WaitOutcome::Fatal;
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
    short     revents = 0;
    const int rc      = pollSocket(
        static_cast<socket_fd_t>(sockFd), events, timeoutMs <= 0 ? -1 : timeoutMs, &revents);
    if (rc < 0) {
#ifndef Q_OS_WIN
        /* A signal cut the wait short. The socket is untouched, so poisoning
         * the session here would kill a healthy connection whenever a
         * timer or SIGWINCH lands mid-poll. */
        if (errno == EINTR) {
            return WaitOutcome::Timeout;
        }
#endif
        return WaitOutcome::Fatal;
    }
    if (rc == 0) {
        return WaitOutcome::Timeout;
    }
    /* A hangup that arrives with readable data still has bytes to drain,
     * and libssh2 reports the real EOF once they are consumed. Only a
     * hangup with nothing left, or an outright socket error, is fatal. */
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        return WaitOutcome::Fatal;
    }
    if ((revents & POLLHUP) != 0 && (revents & POLLIN) == 0) {
        return WaitOutcome::Fatal;
    }
    return WaitOutcome::Ready;
}

void QSocSshSession::setError(const QString &msg)
{
    m_lastError = msg;
}

bool QSocSshSession::drainCall(
    qintptr sockFd, LIBSSH2_SESSION *session, const std::function<int()> &call)
{
    const QDeadlineTimer deadline(kTeardownMs);
    while (call() == LIBSSH2_ERROR_EAGAIN) {
        if (waitWithin(sockFd, session, deadline, Interruptible::No) != WaitOutcome::Ready) {
            return false;
        }
    }
    return true;
}

void QSocSshSession::closeOwnedSocket()
{
    /* Only close the socket when it is ours. Tunneled sessions borrow the
     * parent's fd for polling and must not close it. */
    if (m_parent == nullptr && m_socket >= 0) {
        closeSocketFd(static_cast<socket_fd_t>(m_socket));
    }
    m_socket = -1;
}

void QSocSshSession::clearConnection()
{
    if (m_session != nullptr) {
        LIBSSH2_SESSION *session        = m_session;
        const qintptr    sockFd         = m_socket;
        const bool       sentDisconnect = drainCall(sockFd, session, [session] {
            return libssh2_session_disconnect(session, "QSoC shutting down session");
        });
        /* The free is what closes and releases every channel, the SFTP one
         * included, and it waits for the peer to confirm each close. */
        const bool freedOnTheLink = drainCall(sockFd, session, [session] {
            return libssh2_session_free(session);
        });
        bool       freed          = freedOnTheLink;
        if (!freed) {
            /* With the fd gone the channel free stops waiting for a peer that
             * is not answering: it ignores every close error but EAGAIN. The
             * one shape this cannot rescue is a packet left half-sent, which
             * refuses before it ever touches the fd. */
            closeOwnedSocket();
            freed = libssh2_session_free(session) != LIBSSH2_ERROR_EAGAIN;
        }
        m_session = nullptr;
        /* Whatever survived is owned by the session, and the session is gone:
         * either its free released them or nothing ever will. */
        m_stranded.clear();
        m_teardownComplete = sentDisconnect && freedOnTheLink;
        m_teardownReleased = freed;
        if (!freed) {
            qWarning(
                "SSH teardown could not release the libssh2 session; its state is left "
                "allocated");
        } else if (!m_teardownComplete) {
            qWarning(
                "SSH teardown closed the socket to release a session the peer never "
                "confirmed");
        }
    }
    if (m_parentChannel != nullptr) {
        /* Free the direct-tcpip channel on the parent session. The parent
         * owns its own socket and stays alive until the caller destroys it;
         * we only release the hop we opened. Abandoning this one strands the
         * parent's transport, which other hops are still using, so the parent
         * has to be told. */
        LIBSSH2_CHANNEL *hop           = m_parentChannel;
        m_parentChannel                = nullptr;
        LIBSSH2_SESSION *parentSession = m_parent == nullptr ? nullptr : m_parent->rawSession();
        if (parentSession != nullptr && !drainCall(m_parent->socketFd(), parentSession, [hop] {
                return libssh2_channel_free(hop);
            })) {
            m_parent->markAbandonedExchange();
            m_teardownComplete = false;
            qWarning("SSH teardown left a ProxyJump channel unreleased on its parent session");
        }
    }
    closeOwnedSocket();
    m_parent   = nullptr;
    m_unusable = Unusable::No;
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
    /* getaddrinfo has no non-blocking form in POSIX, so the deadline cannot
     * reach inside it: a resolver that stops answering is bounded only by the
     * resolver's own retry schedule. Asking before and after is what keeps a
     * stop from being swallowed by the stage around it. */
    if (connectGaveUp()) {
        const QString msg = QStringLiteral("Connect to %1 stopped before resolving").arg(host);
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::Timeout;
    }
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
        if (connectGaveUp()) {
            break;
        }
        sockFd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sockFd == kInvalidSocket) {
            continue;
        }
        if (connectWithTimeout(sockFd, ai, m_connectDeadline, m_abortProbe)) {
            break;
        }
        closeSocketFd(sockFd);
        sockFd = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (sockFd == kInvalidSocket && connectGaveUp()) {
        const QString msg
            = QStringLiteral("TCP connect to %1:%2 did not finish in time").arg(host).arg(port);
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::Timeout;
    }
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

    /* connectWithTimeout already put the socket in non-blocking mode. */
    enableKeepalive(sockFd);
    m_socket = static_cast<qintptr>(sockFd);
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

    /* Modern OpenSSH (>= 8.8) drops ssh-rsa (SHA-1) from its default
     * PubkeyAcceptedAlgorithms. Explicitly order rsa-sha2-512 ahead of
     * rsa-sha2-256 (and the legacy ssh-rsa as a last resort) so RSA
     * userauth negotiates a SHA-2 signature the server still accepts. */
    libssh2_session_method_pref(
        m_session, LIBSSH2_METHOD_SIGN_ALGO, "rsa-sha2-512,rsa-sha2-256,ssh-rsa");

    int rc = 0;
    while ((rc = libssh2_session_handshake(m_session, nativeSocket(m_socket)))
           == LIBSSH2_ERROR_EAGAIN) {
        if (waitWithin(m_socket, m_session, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
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

    /* Arm SSH-level keepalive so an idle session is not reaped by the
     * server. libssh2 only emits these when the application calls
     * libssh2_keepalive_send(), and it never tracks whether a reply came
     * back, so this is liveness for the server's benefit, not ours:
     * detecting a dead peer is the socket keepalive's job. */
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

bool QSocSshSession::tryAgentAuth(const QString &user, QDeadlineTimer deadline)
{
#ifdef Q_OS_WIN
    /* Windows agents are a Pageant window or a named pipe, neither of which the
     * AF_UNIX client below can speak, so libssh2's own agent client stays in
     * charge here and the bound this function promises elsewhere does not hold
     * on this platform. Its agent traffic also rides the session's send/recv
     * callbacks, which for a ProxyJump child lead into the remote channel
     * instead of the local agent, so a tunneled child skips the agent here. */
    (void) deadline;
    if (m_parent != nullptr) {
        return false;
    }
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
#else
    /* The session stays non-blocking throughout: the agent exchange runs on our
     * own socket inside AgentLink, and the userauth packets ride the same sliced
     * waits as the handshake. */
    AgentLink link(deadline, m_abortProbe);
    if (!link.open()) {
        return false;
    }
    QList<AgentIdentity> identities;
    if (!link.listIdentities(&identities)) {
        return false;
    }

    const QByteArray userBytes = user.toUtf8();
    for (const AgentIdentity &identity : identities) {
        if (deadline.hasExpired() || (m_abortProbe && m_abortProbe())) {
            return false;
        }
        AgentSignContext context{&link, identity.blob};
        void            *abstract = &context;
        int              rc       = 0;
        while ((rc = libssh2_userauth_publickey(
                    m_session,
                    userBytes.constData(),
                    reinterpret_cast<const unsigned char *>(identity.blob.constData()),
                    static_cast<size_t>(identity.blob.size()),
                    &agentSignCallback,
                    &abstract))
               == LIBSSH2_ERROR_EAGAIN) {
            if (waitWithin(m_socket, m_session, deadline, Interruptible::Yes)
                != WaitOutcome::Ready) {
                return false;
            }
        }
        if (rc == 0) {
            return true;
        }
        if (notePossibleTransportError(rc)) {
            return false;
        }
    }
    return false;
#endif
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
        if (waitWithin(m_socket, m_session, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
            return false;
        }
    }
    notePossibleTransportError(rc);
    return rc == 0;
}

void QSocSshSession::setSecretCallback(SecretCallback callback)
{
    m_secretCallback = std::move(callback);
}

void QSocSshSession::setAbortProbe(std::function<bool()> probe)
{
    m_abortProbe = std::move(probe);
}

bool QSocSshSession::tryPassphrasePrompt(
    const QString &user, const QStringList &identityPaths, QStringList *triedKeys)
{
    if (!m_secretCallback) {
        return false;
    }
    for (const QString &identity : identityPaths) {
        if (connectGaveUp()) {
            return false;
        }
        const QString prompt
            = QStringLiteral("Passphrase for %1: ").arg(QFileInfo(identity).fileName());
        const QString phrase = promptSecret(prompt);
        if (phrase.isEmpty()) {
            continue;
        }
        if (tryIdentityFileAuth(user, identity, phrase)) {
            return true;
        }
        triedKeys->append(QFileInfo(identity).fileName() + QStringLiteral(" (passphrase)"));
    }
    return false;
}

bool QSocSshSession::tryPasswordPrompt(const QString &user, const QString &hostname, int port)
{
    if (!m_secretCallback || m_session == nullptr) {
        return false;
    }
    const QByteArray userBytes = user.toUtf8();
    /* Query which methods the server accepts so we only prompt for a
     * password when the server actually has the `password` method.
     * The call also primes libssh2's userauth state. Driven from the
     * non-blocking session rather than flipped to blocking: this is ordinary
     * session traffic, so the same sliced wait as the handshake applies and a
     * server that goes quiet mid-query stays interruptible. */
    char *methods = nullptr;
    while ((methods = libssh2_userauth_list(
                m_session, userBytes.constData(), static_cast<unsigned int>(userBytes.size())))
           == nullptr) {
        const int err = libssh2_session_last_errno(m_session);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            notePossibleTransportError(err);
            return false;
        }
        if (waitWithin(m_socket, m_session, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
            return false;
        }
    }
    const QByteArray methodList(methods);
    if (!methodList.contains("password")) {
        return false;
    }
    const QString prompt = QStringLiteral("Password for %1@%2:%3: ").arg(user, hostname).arg(port);
    const QString pwd    = promptSecret(prompt);
    if (pwd.isEmpty()) {
        return false;
    }
    const QByteArray pwdBytes = pwd.toUtf8();
    int              status   = 0;
    while (
        (status = libssh2_userauth_password(m_session, userBytes.constData(), pwdBytes.constData()))
        == LIBSSH2_ERROR_EAGAIN) {
        if (waitWithin(m_socket, m_session, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
            return false;
        }
    }
    notePossibleTransportError(status);
    return status == 0;
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
     * route even when the user intended the agent to sign.
     *
     * The agent is also the only route that depends on a third process, and one
     * that accepts a connection and then never answers is an ordinary way to
     * find a forwarded agent whose upstream hop died. It therefore gets a share
     * of what is left of the connect budget rather than all of it, so the file
     * routes behind it still have time to work. */
    const bool restrictToFiles = host.identitiesOnly && !host.identityFiles.isEmpty();
    if (!restrictToFiles) {
        const qint64         share         = m_connectDeadline.remainingTime();
        const QDeadlineTimer agentDeadline = share < 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                                                       : QDeadlineTimer(share / 2);
        if (tryAgentAuth(user, agentDeadline)) {
            return ConnectStatus::Ok;
        }
    }
    if (connectGaveUp()) {
        const QString msg = QStringLiteral("Authentication for %1@%2:%3 did not finish in time")
                                .arg(user, host.hostname)
                                .arg(host.port);
        setError(msg);
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return ConnectStatus::Timeout;
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
        /* A key that failed because the wait ran out left libssh2 in the middle
         * of a userauth request, and the next key would send its request into
         * that half-finished one. There is nothing left to spend anyway. */
        if (connectGaveUp()) {
            break;
        }
    }

    /* Interactive fallback (only when a secret callback is wired by the
     * caller). First retry each identity file with a callback-supplied
     * passphrase, then attempt password auth if the server advertises
     * it. Sub-agent dispatch leaves the callback unset so a child run
     * can never block on user input mid-LLM-turn. */
    if (m_secretCallback) {
        if (tryPassphrasePrompt(user, identityPaths, &triedKeys)) {
            return ConnectStatus::Ok;
        }
        if (tryPasswordPrompt(user, host.hostname, host.port)) {
            return ConnectStatus::Ok;
        }
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
    /* Occupancy, not health: a session whose transport died still owns a
     * libssh2 handle, and overwriting it here would leak it. */
    if (m_session != nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Session is already connected");
        }
        return ConnectStatus::AlreadyConnected;
    }

    armConnectDeadline();
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
    /* Occupancy, not health: a session whose transport died still owns a
     * libssh2 handle, and overwriting it here would leak it. */
    if (m_session != nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Session is already connected");
        }
        return ConnectStatus::AlreadyConnected;
    }
    armConnectDeadline();
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
        if (waitWithin(parent->socketFd(), parentSes, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
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
    /* Same SHA-2 ordering as the direct path so tunneled userauth also
     * works against servers that dropped ssh-rsa (SHA-1). */
    libssh2_session_method_pref(
        m_session, LIBSSH2_METHOD_SIGN_ALGO, "rsa-sha2-512,rsa-sha2-256,ssh-rsa");
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
    while ((rc = libssh2_session_handshake(m_session, nativeSocket(m_socket)))
           == LIBSSH2_ERROR_EAGAIN) {
        if (waitWithin(m_socket, m_session, m_connectDeadline, Interruptible::Yes)
            != WaitOutcome::Ready) {
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

ssize_t QSocSshSession::sendOverChannel(
    libssh2_socket_t socket, const void *buffer, size_t length, int flags, void **abstract)
{
    (void) socket;
    (void) flags;
    auto *self = static_cast<QSocSshSession *>(*abstract);
    if (self == nullptr || self->m_parentChannel == nullptr) {
        return -1;
    }
    const ssize_t n = libssh2_channel_write_ex(
        self->m_parentChannel, 0, static_cast<const char *>(buffer), length);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return -EAGAIN;
    }
    /* libssh2_channel_write_ex returns 0 when the remote window is closed,
     * not EOF. Returning 0 to libssh2's send path would surface the same
     * cascade as a half-open socket, so we normalize to EAGAIN and let
     * the retry loop pump the parent. */
    if (n == 0) {
        return -EAGAIN;
    }
    if (n < 0) {
        return -1;
    }
    return n;
}

ssize_t QSocSshSession::recvOverChannel(
    libssh2_socket_t socket, void *buffer, size_t length, int flags, void **abstract)
{
    (void) socket;
    (void) flags;
    auto *self = static_cast<QSocSshSession *>(*abstract);
    if (self == nullptr || self->m_parentChannel == nullptr) {
        return -1;
    }
    /* A zero-length read on the parent forces libssh2 to pump the parent
     * session's transport: that is what processes any pending
     * SSH_MSG_CHANNEL_WINDOW_ADJUST or keepalive before we try to pull
     * real bytes for the child. Without this the child can deadlock
     * waiting for a window grant that has already arrived but never
     * been drained. */
    char drain = 0;
    (void) libssh2_channel_read_ex(self->m_parentChannel, 0, &drain, 0);

    const ssize_t n
        = libssh2_channel_read_ex(self->m_parentChannel, 0, static_cast<char *>(buffer), length);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return -EAGAIN;
    }
    /* A zero count here is "no data available yet" unless the parent
     * channel is actually at EOF. Returning 0 to libssh2 without a real
     * EOF makes the child misread the state as a half-open socket and
     * fail userauth with LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED. */
    if (n == 0) {
        if (libssh2_channel_eof(self->m_parentChannel) != 0) {
            return 0;
        }
        return -EAGAIN;
    }
    if (n < 0) {
        return -1;
    }
    return n;
}
