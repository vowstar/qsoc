// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoclibssh2init.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QtCore>
#include <QtTest>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#endif

#ifndef Q_OS_WIN
namespace {
volatile sig_atomic_t g_signalSeen = 0;

void recordSignal(int)
{
    g_signalSeen = 1;
}
} // namespace
#endif

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Construction without connecting must not leak and must not require
     * a running sshd. The library init singleton is exercised. */
    void testConstructAndDestruct()
    {
        QSocSshSession session;
        QCOMPARE(session.isConnected(), false);
        QCOMPARE(session.rawSession(), static_cast<LIBSSH2_SESSION *>(nullptr));
        QCOMPARE(session.socketFd(), -1);
        QVERIFY(QSocLibSsh2Init::useCount() >= 1);
    }

    void testRawHostKeyPrecedesCertificate()
    {
        QSocTestSshd fixture;
        fixture.enableHostCertificate();
        fixture.start();
        QSOC_REQUIRE_SSHD(fixture);

        QFile publicKey(fixture.root() + QStringLiteral("/host_rsa.pub"));
        QVERIFY(publicKey.open(QIODevice::ReadOnly));
        const QByteArray knownHost = QByteArrayLiteral("[127.0.0.1]:")
                                     + QByteArray::number(fixture.port()) + QByteArrayLiteral(" ")
                                     + publicKey.readAll();
        publicKey.close();

        const QString knownHostsPath = fixture.root() + QStringLiteral("/known_hosts");
        QFile         knownHosts(knownHostsPath);
        QVERIFY(knownHosts.open(QIODevice::WriteOnly));
        QCOMPARE(knownHosts.write(knownHost), knownHost.size());
        knownHosts.close();

        QSocSshHostConfig host  = fixture.hostConfig();
        host.strictHostKey      = QSocSshHostConfig::StrictHostKey::Yes;
        host.userKnownHostsFile = knownHostsPath;

        QSocSshSession session;
        QString        error;
        const auto     status = session.connectTo(host, &error);
        QVERIFY2(
            status == QSocSshSession::ConnectStatus::Ok,
            qPrintable(QStringLiteral("%1\n--- sshd ---\n%2").arg(error, fixture.log())));
    }

    /* Unresolvable hostname surfaces as NetworkError, never a crash. The
     * error string must not contain any private-key paths or secrets. */
    void testConnectToUnresolvableHost()
    {
        QSocSshSession    session;
        QSocSshHostConfig host;
        host.hostname      = QStringLiteral("qsoc.invalid.example.nxdomain.test");
        host.port          = 22;
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        session.setTimeoutMs(2000);
        QString    err;
        const auto status = session.connectTo(host, &err);
        QCOMPARE(status, QSocSshSession::ConnectStatus::NetworkError);
        QVERIFY(!session.isConnected());
        QVERIFY(err.contains(QStringLiteral("qsoc.invalid.example.nxdomain.test")));
        QVERIFY(!err.contains(QStringLiteral("id_rsa")));
        QVERIFY(!err.contains(QStringLiteral("id_ed25519")));
    }

    /* A stop asked for while the resolver is running is a cancellation, not a
     * broken resolver: the same unresolvable host, with the user's stop
     * pending, must classify as Aborted before the NetworkError branch. This
     * pins the classification order at the resolver's own failure exit. */
    void testResolverFailureUnderAStopIsAborted()
    {
        QSocSshSession    session;
        QSocSshHostConfig host;
        host.hostname      = QStringLiteral("qsoc.invalid.example.nxdomain.test");
        host.port          = 22;
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        session.setTimeoutMs(2000);
        int probeCalls = 0;
        session.setAbortProbe([&probeCalls] {
            ++probeCalls;
            return probeCalls > 1;
        });
        QString    err;
        const auto status = session.connectTo(host, &err);
        QCOMPARE(status, QSocSshSession::ConnectStatus::Aborted);
        QVERIFY2(probeCalls >= 2, "the stop probe never reached the resolver failure exit");
        QVERIFY2(
            err.contains(QStringLiteral("cancelled by user")),
            qPrintable(
                QStringLiteral("a cancelled resolve reported as a resolver failure: ") + err));
        QVERIFY2(
            !err.startsWith(QStringLiteral("TCP connect")),
            qPrintable(QStringLiteral("the test never reached the resolver failure exit: ") + err));
    }

    /* Closed TCP port on localhost: connect() returns NetworkError, not a
     * crash. Uses port 1 which is almost never served. */
    void testConnectToClosedPort()
    {
        QSocSshSession    session;
        QSocSshHostConfig host;
        host.hostname      = QStringLiteral("127.0.0.1");
        host.port          = 1;
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        session.setTimeoutMs(2000);
        QString    err;
        const auto status = session.connectTo(host, &err);
        QCOMPARE(status, QSocSshSession::ConnectStatus::NetworkError);
        QVERIFY(!session.isConnected());
    }

    /* An expired absolute deadline is stop-now, not the legacy zero value
     * that means an unlimited configured timeout. */
    void testExpiredAbsoluteDeadlineDoesNotBecomeUnlimited()
    {
        QSocSshSession    session;
        QSocSshHostConfig host;
        host.hostname      = QStringLiteral("qsoc.invalid.example.nxdomain.test");
        host.port          = 22;
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        QString    err;
        const auto status = session.connectTo(host, QDeadlineTimer(0), &err);
        QCOMPARE(status, QSocSshSession::ConnectStatus::Timeout);
        QVERIFY(!session.isConnected());
        QVERIFY(err.contains(QStringLiteral("stopped before resolving")));
    }

    /* waitSocket with an invalid fd must report Fatal rather than block,
     * segfault, or claim a timeout the caller would retry through. */
    void testWaitSocketRejectsBadArguments()
    {
        QCOMPARE(QSocSshSession::waitSocket(-1, nullptr, 100), QSocSshSession::WaitOutcome::Fatal);
    }

    /* A benign process signal does not consume a socket deadline. Returning
     * Timeout on EINTR made teardown abandon a healthy channel immediately. */
    void testWaitSocketKeepsItsBudgetAcrossEintr()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX poll interruption is not available on Windows");
#else
        int sockets[2] = {-1, -1};
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        const auto closeSockets = qScopeGuard([&] {
            ::close(sockets[0]);
            ::close(sockets[1]);
        });

        struct sigaction action    = {};
        struct sigaction oldAction = {};
        action.sa_handler          = recordSignal;
        QVERIFY(sigemptyset(&action.sa_mask) == 0);
        QVERIFY(::sigaction(SIGUSR1, &action, &oldAction) == 0);
        const auto restoreSignal = qScopeGuard(
            [&] { (void) ::sigaction(SIGUSR1, &oldAction, nullptr); });

        QSocLibSsh2Init::ensure();
        LIBSSH2_SESSION *session = libssh2_session_init();
        QVERIFY(session != nullptr);
        const auto freeSession = qScopeGuard([&] { (void) libssh2_session_free(session); });
        const int  flags       = ::fcntl(sockets[0], F_GETFL, 0);
        QVERIFY(flags >= 0);
        QVERIFY(::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
        libssh2_session_set_blocking(session, 0);
        QCOMPARE(
            libssh2_session_handshake(session, sockets[0]), static_cast<int>(LIBSSH2_ERROR_EAGAIN));
        QVERIFY((libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_INBOUND) != 0);

        g_signalSeen             = 0;
        const pthread_t   target = ::pthread_self();
        std::atomic<bool> keepInterrupting{true};
        std::atomic<int>  signalsSent{0};
        std::thread       interrupter([target, &keepInterrupting, &signalsSent] {
            while (keepInterrupting.load(std::memory_order_acquire)) {
                if (::pthread_kill(target, SIGUSR1) == 0) {
                    signalsSent.fetch_add(1, std::memory_order_relaxed);
                }
                QThread::msleep(5);
            }
        });
        const auto        stopThread = qScopeGuard([&] {
            keepInterrupting.store(false, std::memory_order_release);
            if (interrupter.joinable()) {
                interrupter.join();
            }
        });

        QElapsedTimer elapsed;
        elapsed.start();
        const auto outcome = QSocSshSession::waitSocket(sockets[0], session, 500);
        keepInterrupting.store(false, std::memory_order_release);
        interrupter.join();

        QCOMPARE(outcome, QSocSshSession::WaitOutcome::Timeout);
        QVERIFY(g_signalSeen != 0);
        QVERIFY2(signalsSent.load() > 5, "the wait was not interrupted repeatedly");
        QVERIFY2(elapsed.elapsed() >= 400, "EINTR was mistaken for the end of the deadline");
#endif
    }

    /* Only socket-level codes may poison a session. An SFTP protocol error
     * or a permission denial means the link works and the request did not,
     * and treating those as a dead link would tear down a healthy session. */
    void testOnlySocketErrorsCountAsTransportFailures()
    {
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_SOCKET_SEND));
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_SOCKET_RECV));
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_SOCKET_DISCONNECT));
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_SOCKET_TIMEOUT));
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_TIMEOUT));
        QVERIFY(QSocSshSession::isTransportError(LIBSSH2_ERROR_BAD_SOCKET));

        QVERIFY(!QSocSshSession::isTransportError(0));
        QVERIFY(!QSocSshSession::isTransportError(LIBSSH2_ERROR_EAGAIN));
        QVERIFY(!QSocSshSession::isTransportError(LIBSSH2_ERROR_SFTP_PROTOCOL));
        QVERIFY(!QSocSshSession::isTransportError(LIBSSH2_ERROR_AUTHENTICATION_FAILED));
        QVERIFY(!QSocSshSession::isTransportError(LIBSSH2_ERROR_CHANNEL_FAILURE));

        QSocSshSession session;
        QVERIFY(!session.notePossibleTransportError(LIBSSH2_ERROR_SFTP_PROTOCOL));
        QVERIFY(!session.isTransportDead());
        QVERIFY(session.notePossibleTransportError(LIBSSH2_ERROR_SOCKET_RECV));
        QVERIFY(session.isTransportDead());
    }

    /* A session whose transport died must stop reporting itself connected,
     * so the next tool call fails closed instead of hanging on a socket
     * that will never answer. */
    void testTransportDeathIsVisible()
    {
        QSocSshSession session;
        QVERIFY(!session.isTransportDead());
        session.markTransportDead();
        QVERIFY(session.isTransportDead());
        QVERIFY(!session.isConnected());
        /* Teardown of an already-dead session must still be safe. */
        session.disconnectFromHost();
        QVERIFY(!session.isTransportDead());
    }

    /* A peer that accepts the TCP connection and then says nothing is what
     * a firewalled or wedged host looks like. connectTo must give up on its
     * own timeout instead of parking the caller in the handshake loop. */
    void testHandshakeAgainstSilentPeerTimesOut()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        QSocSshSession    session;
        QSocSshHostConfig host;
        host.hostname      = QStringLiteral("127.0.0.1");
        host.port          = server.serverPort();
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        session.setTimeoutMs(1000);
        QElapsedTimer clock;
        clock.start();
        QString      err;
        const auto   status  = session.connectTo(host, &err);
        const qint64 elapsed = clock.elapsed();

        QCOMPARE(status, QSocSshSession::ConnectStatus::Timeout);
        QVERIFY(!session.isConnected());
        QVERIFY2(elapsed < 10000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
    }

    /* A reserved address that swallows the SYN must not park the caller on
     * the kernel's retry schedule, which runs well past two minutes. Hosts
     * that answer with ICMP unreachable fail even faster; either way the
     * call has to come back bounded. */
    void testConnectToBlackholeAddressIsBounded()
    {
        QSocSshSession    session;
        QSocSshHostConfig host;
        /* RFC 5737 TEST-NET-1: reserved for documentation, never routed. */
        host.hostname      = QStringLiteral("192.0.2.1");
        host.port          = 22;
        host.user          = QStringLiteral("nobody");
        host.strictHostKey = QSocSshHostConfig::StrictHostKey::No;

        session.setTimeoutMs(1500);
        QElapsedTimer clock;
        clock.start();
        QString      err;
        const auto   status  = session.connectTo(host, &err);
        const qint64 elapsed = clock.elapsed();

        QVERIFY(status != QSocSshSession::ConnectStatus::Ok);
        QVERIFY(!session.isConnected());
        QVERIFY2(elapsed < 15000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocsshsession.moc"
