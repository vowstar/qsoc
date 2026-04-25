// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoclibssh2init.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

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

    /* waitSocket with an invalid fd must return -1 (error) rather than
     * block or segfault. */
    void testWaitSocketRejectsBadArguments()
    {
        QCOMPARE(QSocSshSession::waitSocket(-1, nullptr, 100), -1);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocsshsession.moc"
