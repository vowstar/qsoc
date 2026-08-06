// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoclibssh2init.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QtCore>
#include <QtTest>

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

    /* waitSocket with an invalid fd must report Fatal rather than block,
     * segfault, or claim a timeout the caller would retry through. */
    void testWaitSocketRejectsBadArguments()
    {
        QCOMPARE(QSocSshSession::waitSocket(-1, nullptr, 100), QSocSshSession::WaitOutcome::Fatal);
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
