// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <unistd.h>
#endif

/*
 * Real-server regression for the SFTP write/edit path. A loopback OpenSSH
 * sshd is started over internal-sftp and qsoc's own SSH/SFTP stack writes
 * to it. The key case is overwriting an EXISTING file many times: the
 * non-blocking unlink-before-rename bug made that fail intermittently
 * while writing a brand-new file did not.
 *
 * The test QSKIPs (never fails) when the environment cannot host a
 * user-level sshd: no sshd / ssh-keygen, key generation fails, sshd will
 * not start, or the SSH handshake does not complete. On a CI runner with
 * openssh-server installed it runs for real and catches the regression.
 */

namespace {

QString findExe(const QStringList &candidates)
{
    for (const QString &path : candidates) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

/* The login name of the process owner. $USER is unset in CI non-login
 * shells (e.g. GitHub Actions), so resolve via the password database and
 * fall back to the env var only off-POSIX. Without this the loopback
 * test would QSKIP on CI instead of actually exercising the SFTP path. */
QString currentUser()
{
#ifndef Q_OS_WIN
    if (const struct passwd *pw = getpwuid(getuid())) {
        if (pw->pw_name != nullptr && pw->pw_name[0] != '\0') {
            return QString::fromLocal8Bit(pw->pw_name);
        }
    }
#endif
    return qEnvironmentVariable("USER");
}

int pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = probe.serverPort();
    probe.close();
    return port;
}

bool runKeygen(const QString &keygen, const QString &keyPath)
{
    /* RSA, classic PEM (-m PEM). qsoc pins userauth to the rsa-sha2
     * family (see QSocSshSession) and every real server offers an RSA
     * host key, so the fixture mirrors that. PEM matters for the client
     * key: this libssh2 build signs RSA pubkey auth reliably only from a
     * classic "BEGIN RSA PRIVATE KEY" file; a modern OpenSSH-format key
     * aborts the signature step after the server's PK_OK. */
    QProcess proc;
    proc.start(
        keygen,
        {QStringLiteral("-t"),
         QStringLiteral("rsa"),
         QStringLiteral("-b"),
         QStringLiteral("3072"),
         QStringLiteral("-m"),
         QStringLiteral("PEM"),
         QStringLiteral("-N"),
         QString(),
         QStringLiteral("-q"),
         QStringLiteral("-f"),
         keyPath});
    return proc.waitForStarted(5000) && proc.waitForFinished(15000)
           && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0
           && QFile::exists(keyPath);
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void releasesAuthenticationCallbackAfterConnect();
    void overwriteExistingFileRepeatedly();

private:
    QTemporaryDir m_dir;
    QProcess      m_sshd;
    int           m_port  = 0;
    bool          m_ready = false;
    QString       m_keyPath;
    QString       m_user;
    QString       m_workDir;
    QString       m_sshdErr;
};

void Test::initTestCase()
{
    const QString sshd = findExe(
        {QStringLiteral("/usr/sbin/sshd"), QStringLiteral("/usr/bin/sshd")});
    const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
    m_user               = currentUser();
    if (sshd.isEmpty() || keygen.isEmpty() || m_user.isEmpty() || !m_dir.isValid()) {
        return; /* m_ready stays false -> test QSKIPs */
    }

    const QString root     = m_dir.path();
    const QString hostKey  = root + QStringLiteral("/host_rsa");
    m_keyPath              = root + QStringLiteral("/client_rsa");
    const QString authKeys = root + QStringLiteral("/authorized_keys");
    const QString cfgPath  = root + QStringLiteral("/sshd_config");
    m_workDir              = root + QStringLiteral("/work");
    QDir().mkpath(m_workDir);

    if (!runKeygen(keygen, hostKey) || !runKeygen(keygen, m_keyPath)) {
        return;
    }
    /* authorized_keys = the client public key. */
    QFile pub(m_keyPath + QStringLiteral(".pub"));
    if (!pub.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray pubKey = pub.readAll();
    pub.close();
    QFile ak(authKeys);
    if (!ak.open(QIODevice::WriteOnly)) {
        return;
    }
    ak.write(pubKey);
    ak.close();

    m_port = pickFreePort();
    if (m_port == 0) {
        return;
    }

    /* internal-sftp keeps the test independent of the sftp-server path;
     * StrictModes/UsePAM off so a user-level sshd in a temp dir is happy. */
    const QString cfg = QStringLiteral(
                            "Port %1\n"
                            "ListenAddress 127.0.0.1\n"
                            "HostKey %2\n"
                            "PidFile %3/sshd.pid\n"
                            "AuthorizedKeysFile %4\n"
                            "UsePAM no\n"
                            "StrictModes no\n"
                            "PasswordAuthentication no\n"
                            "KbdInteractiveAuthentication no\n"
                            "PubkeyAuthentication yes\n"
                            "Subsystem sftp internal-sftp\n"
                            "LogLevel ERROR\n")
                            .arg(m_port)
                            .arg(hostKey)
                            .arg(root)
                            .arg(authKeys);
    QFile         cf(cfgPath);
    if (!cf.open(QIODevice::WriteOnly)) {
        return;
    }
    cf.write(cfg.toUtf8());
    cf.close();

    /* -D foreground, -e log to stderr; absolute sshd path satisfies the
     * re-exec requirement. Capture the server log for failure diagnosis. */
    m_sshdErr = root + QStringLiteral("/sshd.err");
    m_sshd.setStandardErrorFile(m_sshdErr);
    m_sshd.start(sshd, {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), cfgPath});
    if (!m_sshd.waitForStarted(5000)) {
        return;
    }

    /* Wait until the port accepts connections (sshd is up). */
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (m_sshd.state() != QProcess::Running) {
            return; /* sshd died (likely refuses to run here) -> QSKIP */
        }
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_port));
        if (probe.waitForConnected(200)) {
            probe.disconnectFromHost();
            m_ready = true;
            return;
        }
        QTest::qWait(100);
    }
}

void Test::cleanupTestCase()
{
    if (m_sshd.state() != QProcess::NotRunning) {
        m_sshd.terminate();
        if (!m_sshd.waitForFinished(3000)) {
            m_sshd.kill();
            m_sshd.waitForFinished(2000);
        }
    }
}

void Test::releasesAuthenticationCallbackAfterConnect()
{
    if (!m_ready) {
        QSKIP("loopback sshd unavailable in this environment");
    }

    const QString homeDir = m_dir.path() + QStringLiteral("/client_home");
    QVERIFY(QDir().mkpath(homeDir + QStringLiteral("/.ssh")));
    QFile config(homeDir + QStringLiteral("/.ssh/config"));
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write(QStringLiteral(
                     "Host callback-release\n"
                     "  HostName 127.0.0.1\n"
                     "  Port %1\n"
                     "  User %2\n"
                     "  IdentityFile %3\n"
                     "  IdentitiesOnly yes\n")
                     .arg(m_port)
                     .arg(m_user, m_keyPath)
                     .toUtf8());
    config.close();

    const bool       hadHome     = qEnvironmentVariableIsSet("HOME");
    const QByteArray oldHome     = qgetenv("HOME");
    const auto       restoreHome = qScopeGuard([hadHome, oldHome]() {
        if (hadHome) {
            qputenv("HOME", oldHome);
        } else {
            qunsetenv("HOME");
        }
    });
    QVERIFY(qputenv("HOME", homeDir.toUtf8()));
    QCOMPARE(QDir::homePath(), homeDir);

    QObject          parent;
    AgentRemoteState state;
    const auto       cleanupState = qScopeGuard([&state]() {
        if (state.sftp != nullptr) {
            state.sftp->close();
            delete state.sftp;
        }
        if (state.session != nullptr) {
            state.session->disconnectFromHost();
            delete state.session;
        }
        for (auto it = state.jumps.rbegin(); it != state.jumps.rend(); ++it) {
            (*it)->disconnectFromHost();
            delete *it;
        }
    });

    auto                           token = std::make_shared<int>(1);
    const std::weak_ptr<int>       weakToken(token);
    QSocSshSession::SecretCallback callback = [token](const QString &) { return QString(); };
    token.reset();

    QString error;
    QVERIFY2(
        connectAgentSshSession(QStringLiteral("callback-release"), &parent, &state, &error, callback),
        qPrintable(error));
    callback = {};
    QVERIFY(weakToken.expired());
}

void Test::overwriteExistingFileRepeatedly()
{
    if (!m_ready) {
        QSKIP("loopback sshd unavailable in this environment");
    }

    QSocSshHostConfig host;
    host.hostname           = QStringLiteral("127.0.0.1");
    host.port               = m_port;
    host.user               = m_user;
    host.identityFiles      = {m_keyPath};
    host.identitiesOnly     = true;
    host.strictHostKey      = QSocSshHostConfig::StrictHostKey::No;
    host.userKnownHostsFile = QStringLiteral("/dev/null");

    QSocSshSession session;
    QString        err;
    if (session.connectTo(host, &err) != QSocSshSession::ConnectStatus::Ok) {
        QString log;
        QFile   lf(m_sshdErr);
        if (lf.open(QIODevice::ReadOnly)) {
            log = QString::fromUtf8(lf.readAll()).section(QLatin1Char('\n'), -30);
        }
        QSKIP(qPrintable(QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, log)));
    }

    QSocSftpClient sftp(session);
    const QString  path = m_workDir + QStringLiteral("/edit_target.sv");

    /* New file: write_file path (target absent, unlink no-ops). */
    QVERIFY2(sftp.writeFile(path, QByteArray("version-0\n"), &err), qPrintable(err));
    QCOMPARE(sftp.readFile(path), QByteArray("version-0\n"));

    /* Overwrite an EXISTING file repeatedly: this is the edit_file path
     * that previously failed with "SFTP rename failed" intermittently. */
    for (int i = 1; i <= 8; ++i) {
        const QByteArray content
            = QStringLiteral("version-%1 with a longer payload line\n").arg(i).toUtf8();
        QVERIFY2(
            sftp.writeFile(path, content, &err),
            qPrintable(QStringLiteral("overwrite %1 failed: %2").arg(i).arg(err)));
        QCOMPARE(sftp.readFile(path), content);
    }
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsftp_loopback.moc"
