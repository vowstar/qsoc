// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHEXEC_H
#define QSOCSSHEXEC_H

#include <libssh2.h>

#include <QByteArray>
#include <QString>

#include <atomic>

class QSocSshSession;

/**
 * @brief Runs a single shell command over an existing SSH session.
 * @details Opens a fresh channel per invocation, streams stdout and stderr
 *          into buffers, and waits for the remote process to exit or the
 *          per-call timeout to fire. `requestAbort()` is the recommended
 *          way to cancel a long-running command from another thread.
 */
class QSocSshExec
{
public:
    /** @brief Outcome of a single `run()` call. */
    struct Result
    {
        int        exitCode = -1;
        QByteArray stdoutBytes;
        QByteArray stderrBytes;
        bool       timedOut = false;
        bool       aborted  = false;
        QString    errorText;
    };

    explicit QSocSshExec(QSocSshSession &session);

    /**
     * @brief Execute a command synchronously.
     * @param command Shell command string passed straight to the remote
     *                shell. The caller owns any required escaping.
     * @param timeoutMs Per-call timeout. <=0 disables the timeout.
     */
    Result run(const QString &command, int timeoutMs = 30000);

    /** @brief Flag a running `run()` call to stop reading and close channel. */
    void requestAbort();

private:
    bool waitReady();

    QSocSshSession   &m_session;
    std::atomic<bool> m_abort{false};
};

#endif // QSOCSSHEXEC_H
