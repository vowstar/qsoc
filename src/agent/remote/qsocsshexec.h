// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHEXEC_H
#define QSOCSSHEXEC_H

#include <libssh2.h>

#include <QByteArray>
#include <QDeadlineTimer>
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
        /** Remote exit status, or -1 when the command's fate is unknown. */
        int        exitCode = -1;
        QByteArray stdoutBytes;
        QByteArray stderrBytes;
        bool       timedOut = false;
        bool       aborted  = false;
        /** The transport died mid-call: the command may have run anyway. */
        bool transportDead = false;
        /**
         * Signal name when the remote process was killed, e.g. "KILL".
         * Non-empty means the command did not exit on its own, and
         * `exitCode` stays -1: a signalled process sends no exit-status.
         */
        QString exitSignal;
        QString errorText;
    };

    explicit QSocSshExec(QSocSshSession &session);

    /**
     * @brief Execute a command synchronously.
     * @param command Shell command string passed straight to the remote
     *                shell. The caller owns any required escaping.
     * @param timeoutMs Per-call timeout, measured from entry and covering
     *                  channel open, exec request, reads and the close
     *                  handshake. The handshake is where the remote reports
     *                  its exit status, which can be long after the last
     *                  byte of output, so a budget too small to reach it
     *                  yields `timedOut` with `exitCode == -1`. <=0 disables
     *                  the timeout, leaving a dead transport as the only way
     *                  out.
     */
    Result run(const QString &command, int timeoutMs = 30000);

    /** @brief Flag a running `run()` call to stop reading and close channel. */
    void requestAbort();

private:
    /**
     * @brief Wait during a request exchange, bounded by the call deadline.
     * @details Giving up strands libssh2 mid-request, so the session is
     *          marked unusable and later calls fail closed.
     */
    bool wait();

    /**
     * @brief Wait during work whose abandonment the caller can absorb.
     * @details Reads are resumable, and a teardown is already unwinding: in
     *          neither case does a later call depend on the reply we stop
     *          waiting for. Crucially a server does not confirm a channel
     *          close until the remote process exits, so insisting on that
     *          confirmation would make every command that outruns its
     *          timeout cost the user their whole workspace. A transport left
     *          stranded mid-send is not reported as such: `send_existing`
     *          returns EAGAIN, so the next call EAGAIN-loops to its own
     *          deadline and comes back `timedOut`.
     */
    bool waitAbandonable();

    bool waitInternal(bool requestInFlight);

    /**
     * @brief Release a channel, driving the non-blocking free to completion.
     * @details An EAGAIN return released nothing and left the channel
     *          registered, and it cannot succeed while the remote process
     *          outlives the call. The channel is then left to the session
     *          teardown, which frees it: condemning the session over an
     *          ordinary timeout would cost the caller its workspace.
     */
    void freeChannel(LIBSSH2_CHANNEL *channel);

    QSocSshSession   &m_session;
    std::atomic<bool> m_abort{false};
    QDeadlineTimer    m_deadline;
    bool              m_transportDead = false;
};

#endif // QSOCSSHEXEC_H
