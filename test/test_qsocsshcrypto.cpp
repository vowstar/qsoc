// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

/*
 * Host key, key exchange and identity file types against a loopback OpenSSH
 * sshd, and Qt's own TLS in the same process as the bundled libcrypto. Every
 * key and certificate is generated per run.
 *
 * A dependency this fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing openssh-server.
 */

namespace {

const QStringList kEd25519 = {QStringLiteral("-t"), QStringLiteral("ed25519")};

/* Connect with only @p key offered. Empty on success, else the reason. */
QString connectWith(const QSocTestSshd &fixture, const QString &key)
{
    QSocSshHostConfig host = fixture.hostConfig();
    host.identityFiles     = {key};
    QSocSshSession session;
    QString        error;
    if (session.connectTo(host, &error) == QSocSshSession::ConnectStatus::Ok) {
        return {};
    }
    return QStringLiteral("%1\n--- sshd ---\n%2").arg(error, fixture.log());
}

/* A self-signed certificate and key from the openssl tool, in @p dir. */
bool makeTlsIdentity(const QString &openssl, const QString &dir)
{
    QProcess proc;
    proc.start(
        openssl,
        {QStringLiteral("req"),
         QStringLiteral("-x509"),
         QStringLiteral("-newkey"),
         QStringLiteral("rsa:2048"),
         QStringLiteral("-nodes"),
         QStringLiteral("-subj"),
         QStringLiteral("/CN=127.0.0.1"),
         QStringLiteral("-days"),
         QStringLiteral("1"),
         QStringLiteral("-keyout"),
         dir + QStringLiteral("/tls.key"),
         QStringLiteral("-out"),
         dir + QStringLiteral("/tls.crt")});
    return proc.waitForStarted(5000) && proc.waitForFinished(30000)
           && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

QByteArray slurp(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void anEd25519OnlyHostConnects();
    void aCurve25519OnlyKexConnects();
    void anIdentityFileAuthenticates_data();
    void anIdentityFileAuthenticates();
#ifdef Q_OS_LINUX
    void qtTlsWorksBesideAnSshSession();
#endif
};

/* Counterexample: ssh-ed25519 was missing from the host key list and from the
 * crypto backend, so a server with only an ed25519 host key failed KEX. */
void Test::anEd25519OnlyHostConnects()
{
    QSocTestSshd fixture;
    fixture.setHostKeyArgs(kEd25519);
    fixture.start();
    QSOC_REQUIRE_SSHD(fixture);
    const QString error = connectWith(fixture, fixture.keyPath());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    fixture.stop();
    QVERIFY(fixture.removeRoot());
}

/* Counterexample: curve25519 KEX was compiled out, so a server hardened to it
 * shared no key exchange method with qsoc. */
void Test::aCurve25519OnlyKexConnects()
{
    QSocTestSshd fixture;
    fixture.setExtraConfig(
        {QStringLiteral("KexAlgorithms curve25519-sha256,curve25519-sha256@libssh.org")});
    fixture.start();
    QSOC_REQUIRE_SSHD(fixture);
    const QString error = connectWith(fixture, fixture.keyPath());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    fixture.stop();
    QVERIFY(fixture.removeRoot());
}

void Test::anIdentityFileAuthenticates_data()
{
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<bool>("withPub");

    const QStringList rsa
        = {QStringLiteral("-t"),
           QStringLiteral("rsa"),
           QStringLiteral("-b"),
           QStringLiteral("3072")};
    const QStringList ecdsa = {QStringLiteral("-t"), QStringLiteral("ecdsa")};
    const QStringList pem   = {QStringLiteral("-m"), QStringLiteral("PEM")};
    const struct Row
    {
        const char *name;
        QStringList args;
    } rows[] = {
        {"ed25519", kEd25519},
        {"rsa-openssh", rsa},
        {"rsa-pem", rsa + pem},
        {"ecdsa-openssh", ecdsa},
        {"ecdsa-pem", ecdsa + pem},
    };
    for (const Row &row : rows) {
        QTest::addRow("%s", row.name) << row.args << true;
        QTest::addRow("%s-no-pub", row.name) << row.args << false;
    }
}

/* Counterexample: only classic PEM RSA and ECDSA files were readable, and a
 * key file without its .pub sibling worked for PEM RSA alone. A server per
 * row, so one rejected key cannot penalize the next row's source address. */
void Test::anIdentityFileAuthenticates()
{
    QFETCH(QStringList, args);
    QFETCH(bool, withPub);
    QSocTestSshd fixture;
    fixture.start();
    QSOC_REQUIRE_SSHD(fixture);

    const QString key = fixture.addClientKey(QStringLiteral("id_under_test"), args);
    QVERIFY2(!key.isEmpty(), "ssh-keygen could not generate the client key");
    if (!withPub) {
        QVERIFY(QFile::remove(key + QStringLiteral(".pub")));
    }
    const QString error = connectWith(fixture, key);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    fixture.stop();
    QVERIFY(fixture.removeRoot());
}

#ifdef Q_OS_LINUX
/* Counterexample: the executable exported the bundled libcrypto's symbols, so
 * the system libssl that Qt loads bound to them and crashed in the handshake.
 * ELF only: that is where an executable's symbols interpose a library's. */
void Test::qtTlsWorksBesideAnSshSession()
{
    QSocTestSshd fixture;
    fixture.start();
    QSOC_REQUIRE_SSHD(fixture);
    QSocSshSession session;
    QString        error;
    QVERIFY2(
        session.connectTo(fixture.hostConfig(), &error) == QSocSshSession::ConnectStatus::Ok,
        qPrintable(error));

    const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (!QSslSocket::supportsSsl() || openssl.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a Qt TLS backend and the openssl tool"));
    }
    QVERIFY2(makeTlsIdentity(openssl, fixture.root()), "openssl could not make a certificate");
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(QSslCertificate(slurp(fixture.root() + QStringLiteral("/tls.crt"))));
    config.setPrivateKey(QSslKey(slurp(fixture.root() + QStringLiteral("/tls.key")), QSsl::Rsa));
    QSslServer server;
    server.setSslConfiguration(config);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&server] {
        server.nextPendingConnection()->write("ping");
    });

    QSslSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    client.setPeerVerifyMode(QSslSocket::VerifyNone);
    client.connectToHostEncrypted(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 10000);
    QCOMPARE(client.readAll(), QByteArray("ping"));
    QVERIFY(session.isConnected());
    fixture.stop();
    QVERIFY(fixture.removeRoot());
}
#endif

QSOC_TEST_MAIN(Test)
#include "test_qsocsshcrypto.moc"
