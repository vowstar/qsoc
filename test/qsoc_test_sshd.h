// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_SSHD_H
#define QSOC_TEST_SSHD_H

#include "agent/remote/qsocsshhostconfig.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/**
 * @brief A user-level OpenSSH sshd over internal-sftp, for real-server tests.
 * @details Keys are generated per run, so nothing key-shaped is ever
 *          committed. POSIX only: the sshd lookup, the password database and
 *          /bin/bash on the target all assume it.
 *
 *          Readiness is three-state on purpose. A dependency the fixture
 *          cannot install (sshd, ssh-keygen, a login name) may skip; anything
 *          after that is a fixture that had what it needed and still would not
 *          start, which must fail. InitFailed is the default so that every
 *          early return in start() is already a failure and none has to be
 *          remembered.
 */
class QSocTestSshd
{
public:
    enum class State : std::uint8_t {
        Ready,             /**< sshd is up and accepting connections. */
        DependencyMissing, /**< Something the fixture cannot supply is absent. */
        InitFailed,        /**< Dependencies present, fixture still did not start. */
    };

    ~QSocTestSshd() { stop(); }

    QSocTestSshd()                                = default;
    QSocTestSshd(const QSocTestSshd &)            = delete;
    QSocTestSshd &operator=(const QSocTestSshd &) = delete;

    /**
     * @brief Append directives to the generated sshd_config. Call before
     *        start(); a case that needs server behaviour the default config
     *        does not have gets its own fixture instance rather than changing
     *        what every other case runs against.
     */
    void setExtraConfig(const QStringList &directives) { m_extraConfig = directives; }

    /** @brief Bring up sshd. False unless state() becomes Ready. */
    bool start()
    {
        QStringList absent;
        /* An override lets one run prove both readiness branches without
         * uninstalling anything. CI never sets it. */
        QString sshd = qEnvironmentVariable("QSOC_TEST_SSHD");
        if (sshd.isEmpty()) {
            sshd = findExe({QStringLiteral("/usr/sbin/sshd"), QStringLiteral("/usr/bin/sshd")});
        } else if (!QFile::exists(sshd)) {
            sshd.clear();
        }
        const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        m_user               = loginName();
        if (sshd.isEmpty()) {
            absent << QStringLiteral("sshd");
        }
        if (keygen.isEmpty()) {
            absent << QStringLiteral("ssh-keygen");
        }
        if (m_user.isEmpty()) {
            absent << QStringLiteral("a login name");
        }
        if (!absent.isEmpty()) {
            m_missing = absent.join(QStringLiteral(", "));
            m_state   = State::DependencyMissing;
            return false;
        }

        if (!m_dir.isValid()) {
            return fail(QStringLiteral("temporary directory: %1").arg(m_dir.errorString()));
        }
        const QString root     = m_dir.path();
        const QString hostKey  = root + QStringLiteral("/host_rsa");
        const QString authKeys = root + QStringLiteral("/authorized_keys");
        const QString cfgPath  = root + QStringLiteral("/sshd_config");
        m_keyPath              = root + QStringLiteral("/client_rsa");
        m_workDir              = root + QStringLiteral("/work");
        QDir().mkpath(m_workDir);

        if (!runKeygen(keygen, hostKey)) {
            return fail(QStringLiteral("could not generate the host key"));
        }
        if (!runKeygen(keygen, m_keyPath)) {
            return fail(QStringLiteral("could not generate the client key"));
        }
        QFile pub(m_keyPath + QStringLiteral(".pub"));
        if (!pub.open(QIODevice::ReadOnly)) {
            return fail(QStringLiteral("could not read the client public key"));
        }
        const QByteArray pubKey = pub.readAll();
        pub.close();
        QFile authorized(authKeys);
        if (!authorized.open(QIODevice::WriteOnly)) {
            return fail(QStringLiteral("could not write authorized_keys"));
        }
        authorized.write(pubKey);
        authorized.close();

        m_port = pickFreePort();
        if (m_port == 0) {
            return fail(QStringLiteral("no free loopback port"));
        }

        /* internal-sftp keeps this independent of the sftp-server path;
         * StrictModes and UsePAM off so a user-level sshd in a temp dir is
         * happy. */
        QString cfg = QStringLiteral(
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
        for (const QString &directive : m_extraConfig) {
            cfg += directive + QLatin1Char('\n');
        }
        QFile configFile(cfgPath);
        if (!configFile.open(QIODevice::WriteOnly)) {
            return fail(QStringLiteral("could not write sshd_config"));
        }
        configFile.write(cfg.toUtf8());
        configFile.close();

        /* -D foreground, -e log to stderr; an absolute sshd path satisfies
         * the re-exec requirement. */
        m_logPath = root + QStringLiteral("/sshd.err");
        m_sshd.setStandardErrorFile(m_logPath);
        m_sshd
            .start(sshd, {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), cfgPath});
        if (!m_sshd.waitForStarted(5000)) {
            return fail(QStringLiteral("sshd did not start: %1").arg(m_sshd.errorString()));
        }

        for (int attempt = 0; attempt < 50; ++attempt) {
            if (m_sshd.state() != QProcess::Running) {
                return fail(QStringLiteral("sshd exited during startup:\n%1").arg(log()));
            }
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_port));
            if (probe.waitForConnected(200)) {
                probe.disconnectFromHost();
                m_state = State::Ready;
                return true;
            }
            QTest::qWait(100);
        }
        return fail(QStringLiteral("sshd never accepted a connection:\n%1").arg(log()));
    }

    /** @brief Terminate sshd. Safe to call more than once. */
    void stop()
    {
        if (m_sshd.state() != QProcess::NotRunning) {
            m_sshd.terminate();
            if (!m_sshd.waitForFinished(3000)) {
                m_sshd.kill();
                m_sshd.waitForFinished(2000);
            }
        }
    }

    /**
     * @brief Remove the fixture root.
     * @details QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor
     *          never runs and the keys, work tree and backup siblings would
     *          survive every run. Callers do this from cleanupTestCase.
     */
    bool removeRoot() { return m_dir.remove(); }

    State   state() const { return m_state; }
    QString missing() const { return m_missing; }
    QString failure() const { return m_failure; }
    int     port() const { return m_port; }
    QString keyPath() const { return m_keyPath; }
    QString user() const { return m_user; }
    QString workDir() const { return m_workDir; }
    QString root() const { return m_dir.path(); }

    /** @brief The sshd log, for diagnosing a handshake that failed. */
    QString log() const
    {
        QFile logFile(m_logPath);
        if (!logFile.open(QIODevice::ReadOnly)) {
            return QStringLiteral("(no log)");
        }
        return QString::fromUtf8(logFile.readAll());
    }

    /** @brief Client config pointing at @p port on loopback. */
    QSocSshHostConfig hostConfig(quint16 port) const
    {
        QSocSshHostConfig host;
        host.hostname           = QStringLiteral("127.0.0.1");
        host.port               = port;
        host.user               = m_user;
        host.identityFiles      = {m_keyPath};
        host.identitiesOnly     = true;
        host.strictHostKey      = QSocSshHostConfig::StrictHostKey::No;
        host.userKnownHostsFile = QStringLiteral("/dev/null");
        return host;
    }

    /** @brief Config pointing at the fixture's own port. */
    QSocSshHostConfig hostConfig() const { return hostConfig(static_cast<quint16>(m_port)); }

    /** @brief First existing path among @p candidates, empty when none is. */
    static QString findExe(const QStringList &candidates)
    {
        for (const QString &path : candidates) {
            if (QFile::exists(path)) {
                return path;
            }
        }
        return {};
    }

private:
    /* $USER is unset in CI non-login shells, so resolve through the password
     * database and fall back to the environment only off-POSIX. */
    static QString loginName()
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

    static int pickFreePort()
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            return 0;
        }
        const int port = probe.serverPort();
        probe.close();
        return port;
    }

    /* RSA in classic PEM. qsoc pins userauth to the rsa-sha2 family and every
     * real server offers an RSA host key. PEM matters for the client key: this
     * libssh2 build signs RSA pubkey auth reliably only from a classic
     * "BEGIN RSA PRIVATE KEY" file; an OpenSSH-format key aborts the
     * signature step after the server's PK_OK. */
    static bool runKeygen(const QString &keygen, const QString &keyPath)
    {
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

    bool fail(const QString &detail)
    {
        m_failure = detail;
        m_state   = State::InitFailed;
        return false;
    }

    QTemporaryDir m_dir;
    QProcess      m_sshd;
    State         m_state = State::InitFailed;
    QString       m_missing;
    QString       m_failure;
    QString       m_keyPath;
    QString       m_user;
    QString       m_workDir;
    QString       m_logPath;
    QStringList   m_extraConfig;
    int           m_port = 0;
};

/**
 * @brief First line of every case that needs the sshd fixture.
 * @details Skips only for a dependency the fixture cannot install, and only
 *          while QSOC_TEST_DEPS_REQUIRED is unset. Everything else fails.
 */
#define QSOC_REQUIRE_SSHD(fixture) \
    do { \
        if ((fixture).state() == QSocTestSshd::State::DependencyMissing) { \
            QSOC_TEST_MISSING_DEPENDENCY((fixture).missing()); \
        } \
        if ((fixture).state() != QSocTestSshd::State::Ready) { \
            QSOC_TEST_FIXTURE_FAILED((fixture).failure()); \
        } \
    } while (false)

#endif // QSOC_TEST_SSHD_H
