// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"

#include "agent/remote/qsocsshsession.h"

#include <QElapsedTimer>

QSocSshExec::QSocSshExec(QSocSshSession &session)
    : m_session(session)
{}

void QSocSshExec::requestAbort()
{
    m_abort.store(true, std::memory_order_relaxed);
}

bool QSocSshExec::waitReady()
{
    return QSocSshSession::waitSocket(m_session.socketFd(), m_session.rawSession(), 200) >= 0;
}

QSocSshExec::Result QSocSshExec::run(const QString &command, int timeoutMs)
{
    Result result;
    m_abort.store(false, std::memory_order_relaxed);

    LIBSSH2_SESSION *session = m_session.rawSession();
    const int        sockFd  = m_session.socketFd();
    if (session == nullptr || sockFd < 0) {
        result.errorText = QStringLiteral("SSH session is not connected");
        return result;
    }

    LIBSSH2_CHANNEL *channel = nullptr;
    while ((channel = libssh2_channel_open_session(session)) == nullptr) {
        const int err = libssh2_session_last_errno(session);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            result.errorText = QStringLiteral("Failed to open exec channel");
            return result;
        }
        if (!waitReady()) {
            result.errorText = QStringLiteral("Timed out waiting to open channel");
            return result;
        }
    }

    const QByteArray cmdBytes = command.toUtf8();
    int              rc       = 0;
    while ((rc = libssh2_channel_exec(channel, cmdBytes.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            libssh2_channel_free(channel);
            result.errorText = QStringLiteral("Timed out sending exec request");
            return result;
        }
    }
    if (rc != 0) {
        libssh2_channel_free(channel);
        result.errorText = QStringLiteral("Remote exec failed to start");
        return result;
    }

    QElapsedTimer deadline;
    deadline.start();

    char buffer[4096];
    while (true) {
        if (m_abort.load(std::memory_order_relaxed)) {
            result.aborted = true;
            break;
        }
        if (timeoutMs > 0 && deadline.elapsed() > timeoutMs) {
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
                break;
            }
            waitReady();
            continue;
        }

        if (nout == LIBSSH2_ERROR_EAGAIN || nerr == LIBSSH2_ERROR_EAGAIN) {
            waitReady();
            continue;
        }

        /* Genuine read error. */
        result.errorText = QStringLiteral("Remote read error during exec");
        break;
    }

    while (libssh2_channel_close(channel) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            break;
        }
    }
    result.exitCode = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);
    return result;
}
