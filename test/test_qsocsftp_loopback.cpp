// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#include "qsoc_test_sshd.h"

#include "agent/remote/qsocsshexec.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#ifndef Q_OS_WIN
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/*
 * Real-server regression for the SFTP write/edit path. A loopback OpenSSH
 * sshd is started over internal-sftp and qsoc's own SSH/SFTP stack writes
 * to it. The key case is overwriting an EXISTING file many times: the
 * non-blocking unlink-before-rename bug made that fail intermittently
 * while writing a brand-new file did not.
 *
 * A dependency this fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing openssh-server. Anything else, a fixture that has its
 * dependencies and still will not start or handshake, always fails: QtTest
 * exits 0 on a skip, so a silent skip is indistinguishable from coverage. On
 * a host that forbids a user-level sshd, use
 * `ctest -E test_qsocsftp_loopback`.
 */

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void releasesAuthenticationCallbackAfterConnect();
    void overwriteExistingFileRepeatedly();
    void connectedSocketCarriesTheDocumentedKeepalive();
    void aLiveWorkspaceAnswersTheProbe();
    void execReportsRealExitStatus();
    void execReportsSignalledCommandsAsSignalled();
    void anOrdinaryCommandTimeoutKeepsTheSessionUsable();
    void execInventsNoExitStatusWhenTheLinkDiesMidCommand();
    void execAndSftpGiveUpWhenTheLinkIsAlreadyDead();
    void overwriteKeepsACopyWhenThePublishIsCutOff();
    void publishCutAfterAsideLeavesBothNamesAlone();
    void publishFailureThatCannotBeRestoredSaysSo();
    void aTimedOutRequestCondemnsTheSession();
    void aResetConnectionPoisonsTheSessionAndItsSftp();
    void aGracefulCloseInventsNoExitStatus();

private:
    /** @brief Client config pointing at @p port on loopback. */
    QSocSshHostConfig hostConfig(quint16 port) const { return m_fixture.hostConfig(port); }

    QSocTestSshd m_fixture;
};

void Test::initTestCase()
{
    m_fixture.start();
}

void Test::cleanupTestCase()
{
    m_fixture.stop();
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

void Test::releasesAuthenticationCallbackAfterConnect()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const QString homeDir = m_fixture.root() + QStringLiteral("/client_home");
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
                     .arg(m_fixture.port())
                     .arg(m_fixture.user(), m_fixture.keyPath())
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
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshHostConfig host;
    host.hostname           = QStringLiteral("127.0.0.1");
    host.port               = m_fixture.port();
    host.user               = m_fixture.user();
    host.identityFiles      = {m_fixture.keyPath()};
    host.identitiesOnly     = true;
    host.strictHostKey      = QSocSshHostConfig::StrictHostKey::No;
    host.userKnownHostsFile = QStringLiteral("/dev/null");

    QSocSshSession session;
    QString        err;
    if (session.connectTo(host, &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }

    QSocSftpClient sftp(session);
    const QString  path = m_fixture.workDir() + QStringLiteral("/edit_target.sv");

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

/*
 * The keepalive schedule is a documented promise, so assert what was
 * actually asked of the kernel. This does not prove the kernel declares a
 * silent peer dead on that schedule: the deadlines are what bound an
 * operation, and keepalive is the earlier signal. What it does catch is the
 * setsockopt calls going missing, or drifting away from the manual.
 */
/* The positive half of the liveness probe: a healthy host answers well
 * inside the short budget the callers give it, so the probe cannot turn a
 * working workspace into a refusal. */
void Test::aLiveWorkspaceAnswersTheProbe()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }
    QSocSftpClient sftp(session);
    QElapsedTimer  clock;
    clock.start();
    QVERIFY2(remoteHostAnswers(&sftp, m_fixture.workDir(), 2000, &err), qPrintable(err));
    QVERIFY2(
        clock.elapsed() < 2000, qPrintable(QStringLiteral("probe took %1 ms").arg(clock.elapsed())));
}

void Test::connectedSocketCarriesTheDocumentedKeepalive()
{
    QSOC_REQUIRE_SSHD(m_fixture);
#ifdef Q_OS_WIN
    QSKIP("Windows sets the schedule through SIO_KEEPALIVE_VALS, which is write-only");
#else
    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }

    const auto sockFd = static_cast<int>(session.socketFd());
    QVERIFY(sockFd >= 0);

    auto readOpt = [sockFd](int level, int name) {
        int       value = -1;
        socklen_t len   = sizeof(value);
        if (::getsockopt(sockFd, level, name, &value, &len) != 0) {
            return -1;
        }
        return value;
    };

    QVERIFY2(readOpt(SOL_SOCKET, SO_KEEPALIVE) > 0, "SO_KEEPALIVE was not enabled");
#ifdef TCP_KEEPIDLE
    QCOMPARE(readOpt(IPPROTO_TCP, TCP_KEEPIDLE), 15);
#endif
#ifdef TCP_KEEPINTVL
    QCOMPARE(readOpt(IPPROTO_TCP, TCP_KEEPINTVL), 5);
#endif
#ifdef TCP_KEEPCNT
    QCOMPARE(readOpt(IPPROTO_TCP, TCP_KEEPCNT), 3);
#endif
#endif
}

void Test::execReportsRealExitStatus()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(m_fixture.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }

    QSocSshExec exec(session);

    const auto ok = exec.run(QStringLiteral("printf ready"), 10000);
    QVERIFY2(ok.errorText.isEmpty(), qPrintable(ok.errorText));
    QCOMPARE(ok.exitCode, 0);
    QCOMPARE(ok.stdoutBytes, QByteArray("ready"));

    const auto failed = exec.run(QStringLiteral("exit 7"), 10000);
    QCOMPARE(failed.exitCode, 7);
    QVERIFY(!failed.timedOut);
}

/*
 * The command is running when the host dies, so the read loop is what hits
 * the deadline. That is the only path where libssh2 has a channel to be
 * asked for an exit status, and asking a channel that never received one
 * is what reported a dead host as exit_code 0.
 */
/*
 * A process killed by a signal sends exit-signal and no exit-status, so
 * reading the status alone reports a zero-initialized 0: a SIGKILLed build
 * would look like a clean success. The link is healthy throughout, so the
 * session must remain usable afterwards.
 */
void Test::execReportsSignalledCommandsAsSignalled()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }

    QSocSshExec exec(session);
    const auto  killed = exec.run(QStringLiteral("kill -KILL $$"), 10000);
    QCOMPARE(killed.exitSignal, QStringLiteral("KILL"));
    QCOMPARE(killed.exitCode, -1);
    QVERIFY(!killed.timedOut);
    QVERIFY(!killed.transportDead);

    /* Nothing about a killed remote process breaks the link. */
    QVERIFY(session.isConnected());
    const auto alive = exec.run(QStringLiteral("printf alive"), 10000);
    QCOMPARE(alive.exitCode, 0);
    QCOMPARE(alive.stdoutBytes, QByteArray("alive"));
    QVERIFY(alive.exitSignal.isEmpty());
}

/*
 * Giving a long command a short timeout is ordinary use, and the link is
 * healthy throughout. The workspace must survive it: condemning the session
 * here would make every impatient timeout cost the user their remote
 * workspace.
 */
void Test::anOrdinaryCommandTimeoutKeepsTheSessionUsable()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }

    QSocSshExec exec(session);
    const auto  slow = exec.run(QStringLiteral("printf started; sleep 30"), 1500);
    QVERIFY(slow.timedOut);
    QCOMPARE(slow.exitCode, -1);
    QVERIFY(!slow.transportDead);

    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::No);
    QVERIFY2(session.isConnected(), "an ordinary command timeout condemned the workspace");

    /* And the proof that matters: the next command still runs. */
    const auto next = exec.run(QStringLiteral("printf still_here"), 10000);
    QCOMPARE(next.exitCode, 0);
    QCOMPARE(next.stdoutBytes, QByteArray("still_here"));
}

void Test::execInventsNoExitStatusWhenTheLinkDiesMidCommand()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    QVERIFY(relay.port() != 0);
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    QSocSshExec exec(session);
    const auto  before = exec.run(QStringLiteral("printf ready"), 10000);
    QCOMPARE(before.exitCode, 0);
    QCOMPARE(before.stdoutBytes, QByteArray("ready"));

    /* Pull the plug while the remote command is still running. run() is
     * blocking on poll() by then, so the flag has to be set from elsewhere;
     * it is atomic and the relay reads it per chunk. */
    std::thread pullThePlug([&relay] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        relay.blackhole();
    });
    const auto  plugGuard = qScopeGuard([&pullThePlug] { pullThePlug.join(); });

    QElapsedTimer clock;
    clock.start();
    const auto   dead    = exec.run(QStringLiteral("sleep 5; printf later"), 2000);
    const qint64 elapsed = clock.elapsed();

    QVERIFY(dead.timedOut || dead.transportDead);
    QCOMPARE(dead.exitCode, -1);
    QVERIFY(dead.stdoutBytes.isEmpty());
    QVERIFY2(elapsed < 8000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
}

/* A link that is already silent when the call starts must not park the
 * caller in the channel-open loop, and SFTP must honor its own budget
 * rather than retry EAGAIN forever. */
void Test::execAndSftpGiveUpWhenTheLinkIsAlreadyDead()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    const QString  path = m_fixture.workDir() + QStringLiteral("/deadline_target.sv");
    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(1000);
    QVERIFY2(sftp.writeFile(path, QByteArray("alive\n"), &err), qPrintable(err));

    relay.blackhole();

    QElapsedTimer clock;
    clock.start();
    QSocSshExec exec(session);
    const auto  dead = exec.run(QStringLiteral("printf later"), 1500);
    QCOMPARE(dead.exitCode, -1);
    QVERIFY(dead.timedOut || dead.transportDead);
    QVERIFY2(
        clock.elapsed() < 8000, qPrintable(QStringLiteral("exec took %1 ms").arg(clock.elapsed())));

    /* The abandoned channel-open condemns the session, so the SFTP call
     * that follows must be refused outright rather than run its own
     * deadline down against a link that cannot answer. */
    QVERIFY(!session.isConnected());
    QString sftpErr;
    clock.restart();
    QVERIFY(sftp.readFile(path, 0, &sftpErr).isNull());
    QVERIFY(!sftpErr.isEmpty());
    QVERIFY2(
        clock.elapsed() < 200,
        qPrintable(QStringLiteral("a condemned session retried for %1 ms").arg(clock.elapsed())));
}

/*
 * The invariant: an interrupted overwrite must never leave the target with
 * no content anywhere. The old unlink-then-rename sequence could remove the
 * only copy and then fail to put the replacement in.
 */
void Test::overwriteKeepsACopyWhenThePublishIsCutOff()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    const QString    dir      = m_fixture.workDir() + QStringLiteral("/publish");
    const QString    path     = dir + QStringLiteral("/precious.sv");
    const QByteArray original = QByteArray("original content that must survive\n");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(1000);
    QVERIFY2(sftp.writeFile(path, original, &err), qPrintable(err));

    /* Kill the link, then attempt an overwrite. */
    relay.blackhole();
    QVERIFY(!sftp.writeFile(path, QByteArray("replacement\n"), &err));
    QVERIFY(!err.isEmpty());
    /* A write we could not confirm must be reported as uncertain so the
     * agent does not retry it blind. */
    QVERIFY(sftp.lastFailureUncertain());

    /* Reconnect on a clean link and prove the content is still reachable,
     * either under the original name or beside it. */
    QSocSshSession recovery;
    if (recovery.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("recovery connect failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }
    QSocSftpClient check(recovery);
    QString        listErr;
    const auto     entries = check.listDir(dir, 0, &listErr);
    QVERIFY2(listErr.isEmpty(), qPrintable(listErr));

    bool foundOriginalContent = false;
    for (const auto &entry : entries) {
        if (entry.name.startsWith(QStringLiteral(".qsoc-write-"))) {
            continue;
        }
        if (check.readFile(dir + QLatin1Char('/') + entry.name) == original) {
            foundOriginalContent = true;
        }
    }
    QVERIFY2(foundOriginalContent, "the only copy of the file was destroyed");
}

/*
 * A reset connection has to poison the session even though poll reports it
 * as readable-then-EOF rather than as an error, and an SFTP channel already
 * open on that session must refuse the next call instead of reusing it.
 */
/*
 * Deterministic coverage of the publish sequence: the link is cut exactly
 * after the old content was renamed aside, which is the window where the
 * target name holds nothing. Neither file may be touched afterwards, and
 * both must still be on the host.
 */
void Test::publishCutAfterAsideLeavesBothNamesAlone()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    const QString    dir      = m_fixture.workDir() + QStringLiteral("/aside");
    const QString    path     = dir + QStringLiteral("/target.sv");
    const QByteArray original = QByteArray("original that must remain readable\n");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(1000);
    QVERIFY2(sftp.writeFile(path, original, &err), qPrintable(err));

    bool sawAside = false;
    sftp.setPublishObserver([&relay, &sawAside](QSocSftpClient::PublishStage stage) {
        if (stage == QSocSftpClient::PublishStage::AfterAside) {
            sawAside = true;
            relay.blackhole();
        }
    });

    QVERIFY(!sftp.writeFile(path, QByteArray("replacement\n"), &err));
    QVERIFY2(sawAside, "the publish sequence never reached the aside step");
    QVERIFY(sftp.lastFailureUncertain());
    QVERIFY(
        err.contains(QStringLiteral("unknown")) || err.contains(QStringLiteral("left in place")));

    /* On a fresh link: the original content is still there under some name,
     * and nothing was cleaned up after the unknown outcome. */
    QSocSshSession recovery;
    if (recovery.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("recovery connect failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }
    QSocSftpClient check(recovery);
    QString        listErr;
    const auto     entries = check.listDir(dir, 0, &listErr);
    QVERIFY2(listErr.isEmpty(), qPrintable(listErr));

    bool foundOriginal = false;
    bool foundBackup   = false;
    bool foundTemp     = false;
    for (const auto &entry : entries) {
        if (entry.name.contains(QStringLiteral(".qsoc-bak-"))) {
            foundBackup = true;
        }
        if (entry.name.startsWith(QStringLiteral(".qsoc-write-"))) {
            foundTemp = true;
        }
        if (check.readFile(dir + QLatin1Char('/') + entry.name) == original) {
            foundOriginal = true;
        }
    }
    QVERIFY2(foundOriginal, "the original content is gone from the remote host");
    QVERIFY2(foundBackup, "the aside copy was removed after an unknown outcome");
    QVERIFY2(foundTemp, "the staged replacement was removed after an unknown outcome");
}

/*
 * When the publish rename is definitely refused, the restore attempt has to
 * be reported for what it was. Deleting the temp out from under the publish
 * makes the refusal deterministic.
 */
void Test::publishFailureThatCannotBeRestoredSaysSo()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }
    /* A second session does the sabotage so the client under test never sees
     * it coming. */
    QSocSshSession saboteurSession;
    if (saboteurSession.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("saboteur connect failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }
    QSocSftpClient saboteur(saboteurSession);

    const QString    dir      = m_fixture.workDir() + QStringLiteral("/refused");
    const QString    path     = dir + QStringLiteral("/target.sv");
    const QByteArray original = QByteArray("original for the refused publish\n");

    QSocSftpClient sftp(session);
    QVERIFY2(sftp.writeFile(path, original, &err), qPrintable(err));

    sftp.setPublishObserver([&](QSocSftpClient::PublishStage stage) {
        if (stage != QSocSftpClient::PublishStage::BeforePublish) {
            return;
        }
        /* Remove the staged temp so the publish rename fails for a reason
         * the server reports, not because the link died. */
        QString    listErr;
        const auto entries = saboteur.listDir(dir, 0, &listErr);
        for (const auto &entry : entries) {
            if (entry.name.startsWith(QStringLiteral(".qsoc-write-"))) {
                QString rmErr;
                saboteur.removeFile(dir + QLatin1Char('/') + entry.name, &rmErr);
            }
        }
    });

    QVERIFY(!sftp.writeFile(path, QByteArray("replacement\n"), &err));
    /* A refusal is definite, so the session stays usable and the original
     * content must be back under its own name. */
    QVERIFY2(session.isConnected(), "a server refusal condemned a healthy session");
    QVERIFY2(
        err.contains(QStringLiteral("restored")) || err.contains(QStringLiteral("still at")),
        qPrintable(err));
    QCOMPARE(sftp.readFile(path), original);
}

/*
 * A request abandoned at its deadline leaves libssh2 mid-exchange. The next
 * call on that session must be refused outright rather than reading a reply
 * that belongs to the call we walked away from.
 */
void Test::aTimedOutRequestCondemnsTheSession()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(600);
    const QString path = m_fixture.workDir() + QStringLiteral("/condemn.sv");
    QVERIFY2(sftp.writeFile(path, QByteArray("alive\n"), &err), qPrintable(err));
    QVERIFY(session.isConnected());

    relay.blackhole();
    QVERIFY(sftp.readFile(path, 0, &err).isNull());

    /* The stat / open exchange was abandoned, so the session is unusable
     * even though the socket itself was never reported as broken. */
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::AbandonedExchange);
    QVERIFY(!session.isConnected());

    QElapsedTimer clock;
    clock.start();
    QString secondErr;
    QVERIFY(sftp.readFile(path, 0, &secondErr).isNull());
    QVERIFY(!secondErr.isEmpty());
    QVERIFY2(
        clock.elapsed() < 200,
        qPrintable(QStringLiteral("a condemned session retried for %1 ms").arg(clock.elapsed())));
}

void Test::aResetConnectionPoisonsTheSessionAndItsSftp()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    /* Open SFTP for real so the next call has a cached handle to reuse. */
    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(1000);
    const QString path = m_fixture.workDir() + QStringLiteral("/poison_target.sv");
    QVERIFY2(sftp.writeFile(path, QByteArray("alive\n"), &err), qPrintable(err));
    QVERIFY(sftp.isOpen());
    QVERIFY(session.isConnected());

    relay.hangUp();

    /* One exec is enough for libssh2 to notice the reset. */
    QSocSshExec exec(session);
    const auto  dead = exec.run(QStringLiteral("printf later"), 2000);
    QCOMPARE(dead.exitCode, -1);

    QVERIFY2(session.isTransportDead(), "a reset connection left the session marked alive");
    QVERIFY2(!session.isConnected(), "isConnected() still reported a dead session as usable");

    /* The cached SFTP handle must not be handed out again. */
    QElapsedTimer clock;
    clock.start();
    QString sftpErr;
    QVERIFY(sftp.readFile(path, 0, &sftpErr).isNull());
    QVERIFY(!sftpErr.isEmpty());
    QVERIFY2(
        clock.elapsed() < 500,
        qPrintable(QStringLiteral("broken SFTP retried for %1 ms").arg(clock.elapsed())));
}

/*
 * A polite FIN is the harder case: reads come back as a clean zero and
 * libssh2 reports channel EOF, so "the command finished" and "the transport
 * went away" look identical unless the close handshake is checked too.
 */
void Test::aGracefulCloseInventsNoExitStatus()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(relay.port()), &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(QStringLiteral("connect through relay failed: %1\n--- sshd ---\n%2")
                             .arg(err, m_fixture.log())));
    }

    QSocSshExec exec(session);
    QCOMPARE(exec.run(QStringLiteral("printf ready"), 10000).exitCode, 0);

    std::thread closer([&relay] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        relay.finish();
    });
    const auto  closerGuard = qScopeGuard([&closer] { closer.join(); });

    const auto dead = exec.run(QStringLiteral("sleep 5; printf later"), 3000);
    QCOMPARE(dead.exitCode, -1);
    QVERIFY(dead.stdoutBytes.isEmpty());
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsftp_loopback.moc"
