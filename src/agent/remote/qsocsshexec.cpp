// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"

#include "agent/remote/qsocsshsession.h"

namespace {

/* Slice length for one poll. Short enough that requestAbort() from
 * another thread takes effect promptly. */
constexpr int kPollSliceMs = 200;

/* Separate budget for closing the channel once the main budget is spent,
 * so cleanup is bounded rather than skipped. */
constexpr int kCleanupMs = 2000;

const auto kTransportDeadText = QStringLiteral(
    "SSH transport is dead: the remote host stopped responding");

} // namespace

QSocSshExec::QSocSshExec(QSocSshSession &session)
    : m_session(session)
{}

void QSocSshExec::requestAbort()
{
    m_abort.store(true, std::memory_order_relaxed);
}

bool QSocSshExec::wait()
{
    return waitInternal(true);
}

bool QSocSshExec::waitAbandonable()
{
    return waitInternal(false);
}

bool QSocSshExec::waitInternal(bool requestInFlight)
{
    if (m_deadline.hasExpired()) {
        if (requestInFlight) {
            m_session.markAbandonedExchange();
        }
        return false;
    }
    const qint64 remaining = m_deadline.remainingTime();
    const int    slice     = remaining < 0
                                 ? kPollSliceMs
                                 : qMax(1, static_cast<int>(qMin<qint64>(remaining, kPollSliceMs)));
    const auto   outcome
        = QSocSshSession::waitSocket(m_session.socketFd(), m_session.rawSession(), slice);
    if (outcome == QSocSshSession::WaitOutcome::Fatal) {
        m_transportDead = true;
        m_session.markTransportDead();
        return false;
    }
    if (m_deadline.hasExpired()) {
        if (requestInFlight) {
            m_session.markAbandonedExchange();
        }
        return false;
    }
    return true;
}

QSocSshExec::Result QSocSshExec::run(const QString &command, int timeoutMs)
{
    Result result;
    m_abort.store(false, std::memory_order_relaxed);
    m_transportDead = false;
    /* One deadline for the whole call. Starting it here rather than at the
     * first read is what stops a vanished host from parking the caller in
     * the channel-open loop forever. */
    m_deadline = timeoutMs > 0 ? QDeadlineTimer(timeoutMs)
                               : QDeadlineTimer(QDeadlineTimer::Forever);

    LIBSSH2_SESSION *session = m_session.rawSession();
    const qintptr    sockFd  = m_session.socketFd();
    if (session == nullptr || sockFd < 0) {
        result.errorText = QStringLiteral("SSH session is not connected");
        return result;
    }
    if (!m_session.isConnected()) {
        result.errorText     = m_session.unusableText();
        result.transportDead = m_session.isTransportDead();
        return result;
    }

    LIBSSH2_CHANNEL *channel = nullptr;
    while ((channel = libssh2_channel_open_session(session)) == nullptr) {
        const int err = libssh2_session_last_errno(session);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            m_transportDead      = m_session.notePossibleTransportError(err);
            result.transportDead = m_transportDead;
            result.errorText     = m_transportDead ? kTransportDeadText
                                                   : QStringLiteral("Failed to open exec channel");
            return result;
        }
        if (!wait()) {
            result.transportDead = m_transportDead;
            result.errorText     = m_transportDead
                                       ? kTransportDeadText
                                       : QStringLiteral("Timed out waiting to open channel");
            result.timedOut      = !m_transportDead;
            return result;
        }
    }

    const QByteArray cmdBytes = command.toUtf8();
    int              rc       = 0;
    while ((rc = libssh2_channel_exec(channel, cmdBytes.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            libssh2_channel_free(channel);
            result.transportDead = m_transportDead;
            result.errorText = m_transportDead ? kTransportDeadText
                                               : QStringLiteral("Timed out sending exec request");
            result.timedOut  = !m_transportDead;
            return result;
        }
    }
    if (rc != 0) {
        libssh2_channel_free(channel);
        m_transportDead      = m_session.notePossibleTransportError(rc);
        result.transportDead = m_transportDead;
        result.errorText     = m_transportDead ? kTransportDeadText
                                               : QStringLiteral("Remote exec failed to start");
        return result;
    }

    /* Set only when the channel reached EOF, which is the one state where
     * libssh2 has a real exit status to report. */
    bool cleanEof = false;

    char buffer[4096];
    while (true) {
        if (m_abort.load(std::memory_order_relaxed)) {
            result.aborted = true;
            break;
        }
        if (m_deadline.hasExpired()) {
            result.timedOut = true;
            break;
        }

        const ssize_t nout = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (nout > 0) {
            result.stdoutBytes.append(buffer, static_cast<int>(nout));
            continue;
        }

        const ssize_t nerr = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
        if (nerr > 0) {
            result.stderrBytes.append(buffer, static_cast<int>(nerr));
            continue;
        }

        if (nout == 0 && nerr == 0) {
            if (libssh2_channel_eof(channel) != 0) {
                cleanEof = true;
                break;
            }
            if (!waitAbandonable()) {
                result.timedOut = !m_transportDead;
                break;
            }
            continue;
        }

        if (nout == LIBSSH2_ERROR_EAGAIN || nerr == LIBSSH2_ERROR_EAGAIN) {
            if (!waitAbandonable()) {
                result.timedOut = !m_transportDead;
                break;
            }
            continue;
        }

        /* Genuine read error. Whether it kills the session depends on the
         * code: a socket failure poisons it, a channel-level refusal does
         * not. */
        const int readErr = static_cast<int>(nout < 0 ? nout : nerr);
        m_transportDead   = m_session.notePossibleTransportError(readErr) || m_transportDead;
        result.errorText  = m_transportDead ? kTransportDeadText
                                            : QStringLiteral("Remote read error during exec");
        break;
    }

    /* Cleanup gets its own window: the main budget is usually spent by the
     * time we get here, and skipping the close leaks the remote handle on
     * a session that is still alive. */
    m_deadline  = QDeadlineTimer(kCleanupMs);
    int closeRc = 0;
    while ((closeRc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitAbandonable()) {
            break;
        }
    }
    if (closeRc != 0) {
        m_transportDead = m_session.notePossibleTransportError(closeRc) || m_transportDead;
    }
    /* libssh2 reports EOF from a received CHANNEL_EOF, so a socket death
     * does not fake one; anything short of EOF has no status to read. */
    if (cleanEof) {
        /* A process killed by a signal sends exit-signal instead of
         * exit-status, and get_exit_status then returns its zero-initialized
         * value: a SIGKILLed build would read as a clean exit 0. Ask for the
         * signal first and leave exitCode at -1 when there is one. */
        char  *signalName = nullptr;
        size_t signalLen  = 0;
        if (libssh2_channel_get_exit_signal(
                channel, &signalName, &signalLen, nullptr, nullptr, nullptr, nullptr)
            == 0) {
            if (signalName != nullptr && signalLen > 0) {
                result.exitSignal = QString::fromUtf8(signalName, static_cast<int>(signalLen));
            }
            /* libssh2 hands back a freshly allocated buffer for each of the
             * out-parameters it filled; the caller owns them. */
            if (signalName != nullptr) {
                libssh2_free(session, signalName);
            }
        }
        if (result.exitSignal.isEmpty()) {
            result.exitCode = libssh2_channel_get_exit_status(channel);
        }
    }
    libssh2_channel_free(channel);

    result.transportDead = m_transportDead;
    if (m_transportDead && result.errorText.isEmpty()) {
        result.errorText = kTransportDeadText;
    }
    return result;
}
