// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QtCore>
#include <QtTest>

/*
 * How long a user waits after asking a connect attempt to stop.
 *
 * Connecting is a sequence of EAGAIN loops, each around one poll bounded by the
 * operation timeout. A poll of that length is a wait nothing can get out of, so
 * what matters is not whether the stop is eventually noticed but how long after
 * it was asked for. The measurement here is exactly that interval: the probe
 * records when it first answered yes, and the case reports the gap between that
 * and the call returning.
 *
 * The peer is a listening socket that never speaks. The kernel completes the
 * TCP handshake from the listen backlog without the test accepting anything, so
 * libssh2 sends its banner and then waits for a reply that never comes. That is
 * the real blocking path, with no server to install and no thread to drive.
 */

namespace {

/* The session's own budget for this case. Long enough that a wait bounded by it
 * rather than by a slice is unmistakable in the measurement, short enough that
 * the revert-check does not take a minute. */
constexpr int kSessionTimeoutMs = 6000;

/* When the probe starts saying stop, measured from the call starting. Well
 * inside the budget above, so the return can only be the abort. */
constexpr int kAbortAfterMs = 400;

/* What a stop may cost the user. One slice is 200 ms; this allows a slice, the
 * poll that was already in flight, and scheduling noise on a loaded machine. */
constexpr qint64 kAcceptableLatencyMs = 1500;

class Test : public QObject
{
    Q_OBJECT

private slots:
    /*
     * A stop asked for inside an attempt must be honoured inside the attempt.
     * Checking it only between attempts leaves the user holding a frozen
     * interface for the rest of the one in flight, which on a host that
     * accepts and then goes quiet is the whole operation budget.
     */
    void aStopDuringAConnectIsHonouredWithinASlice()
    {
        /* A peer that completes the TCP handshake and then says nothing. */
        QTcpServer silent;
        QVERIFY2(silent.listen(QHostAddress::LocalHost, 0), qPrintable(silent.errorString()));

        QSocSshSession session;
        session.setTimeoutMs(kSessionTimeoutMs);

        QElapsedTimer clock;
        qint64        askedAtMs = -1;
        /* No thread: the probe decides from the clock, so the stop arrives
         * from inside the poll loop that has to notice it. */
        session.setAbortProbe([&clock, &askedAtMs] {
            if (clock.elapsed() < kAbortAfterMs) {
                return false;
            }
            if (askedAtMs < 0) {
                askedAtMs = clock.elapsed();
            }
            return true;
        });

        QSocSshHostConfig host;
        host.hostname = QStringLiteral("127.0.0.1");
        host.port     = silent.serverPort();
        host.user     = QStringLiteral("nobody");
        /* Nothing may be read from the real user's home while this runs. */
        host.identityFiles  = {};
        host.identitiesOnly = true;
        host.strictHostKey  = QSocSshHostConfig::StrictHostKey::AcceptNew;

        QString err;
        clock.start();
        const auto   status       = session.connectTo(host, &err);
        const qint64 returnedAtMs = clock.elapsed();

        QVERIFY2(
            askedAtMs >= 0,
            qPrintable(QStringLiteral(
                           "the stop was never consulted; the call returned after %1 ms "
                           "without ever asking")
                           .arg(returnedAtMs)));
        const qint64 latencyMs = returnedAtMs - askedAtMs;
        qInfo(
            "abort latency: %lld ms (asked at %lld ms, returned at %lld ms)",
            latencyMs,
            askedAtMs,
            returnedAtMs);

        QVERIFY2(
            latencyMs <= kAcceptableLatencyMs,
            qPrintable(QStringLiteral(
                           "the user waited %1 ms after asking the connect to stop; a "
                           "slice is %2 ms and the whole attempt is %3 ms")
                           .arg(latencyMs)
                           .arg(200)
                           .arg(kSessionTimeoutMs)));
        /* And it must not report success: nothing was connected. */
        QVERIFY(status != QSocSshSession::ConnectStatus::Ok);
    }

    /*
     * The same slicing must not reach teardown. A disconnect drains libssh2
     * calls that return EAGAIN with nothing done, and giving one of those up
     * leaks the session and its channels, so a stop the user asked for during a
     * connect must not also abandon the release of what that connect built.
     */
    void aStopDoesNotAbandonTeardown()
    {
        QTcpServer silent;
        QVERIFY2(silent.listen(QHostAddress::LocalHost, 0), qPrintable(silent.errorString()));

        QSocSshSession session;
        session.setTimeoutMs(kSessionTimeoutMs);
        /* Stopping from the very first question, so any wait that honours the
         * probe returns at once. */
        session.setAbortProbe([] { return true; });

        QSocSshHostConfig host;
        host.hostname       = QStringLiteral("127.0.0.1");
        host.port           = silent.serverPort();
        host.user           = QStringLiteral("nobody");
        host.identityFiles  = {};
        host.identitiesOnly = true;
        host.strictHostKey  = QSocSshHostConfig::StrictHostKey::AcceptNew;

        QString err;
        QVERIFY(session.connectTo(host, &err) != QSocSshSession::ConnectStatus::Ok);

        /* The teardown has to run to completion even with the probe still
         * saying stop, and it has to come back. */
        QElapsedTimer clock;
        clock.start();
        session.disconnectFromHost();
        QVERIFY2(
            clock.elapsed() < 30000,
            qPrintable(QStringLiteral("teardown did not return; %1 ms").arg(clock.elapsed())));
        QVERIFY(!session.isConnected());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsshabortlatency.moc"
