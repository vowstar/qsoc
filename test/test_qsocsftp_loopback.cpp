// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
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
    void realpathAnswersWithTheHostsOwnSpelling();
    void canonicalizeKeepsATailThatDoesNotExistYet();
    void canonicalizeRefusesALinkTheHostCannotFollow();
    void writeRefusesADirectorySymlinkOutOfTheWorkspace();
    void writeRefusesToBuildTreesThroughASymlink();
    void editRefusesADirectorySymlinkOutOfTheWorkspace();
    void writeRefusesAFileSymlinkOutOfTheWorkspace();
    void writeThroughAnInWorkspaceLinkKeepsTheLink();
    void writeRefusesADeadLinkThatPointsOutOfTheWorkspace();
    void writeRefusesALinkTheHostCannotFollow();
    void writeRefusesADotDotEscape();
    void writeWorksInAWorkspaceReachedThroughASymlink();
    void writeAndEditStillWorkOnAnOrdinaryPath();

private:
    /** @brief Client config pointing at @p port on loopback. */
    QSocSshHostConfig hostConfig(quint16 port) const { return m_fixture.hostConfig(port); }

    /**
     * @brief Bind @p workspace to @p conn the way the agent binds one.
     * @details Seeds root = cwd = workspace and the writable set to
     *          {workspace}, so the tools under test see production wiring.
     *          @p conn takes ownership of the session and the SFTP client.
     */
    bool bindWorkspace(QSocRemoteConnection *conn, const QString &workspace, QString *error)
    {
        auto *session = new QSocSshSession();
        if (session->connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), error)
            != QSocSshSession::ConnectStatus::Ok) {
            delete session;
            return false;
        }
        AgentRemoteState state;
        state.session   = session;
        state.sftp      = new QSocSftpClient(*session);
        state.targetKey = QStringLiteral("loopback");
        state.workspace = workspace;
        if (!conn->adopt(std::move(state))) {
            /* adopt() drains the bundle only after it accepts one, so a
             * refusal leaves the transport here and it is ours to free. */
            // cppcheck-suppress accessMoved
            discardAgentRemoteState(&state);
            *error = QStringLiteral("adopt refused the staging bundle");
            return false;
        }
        return true;
    }

    /**
     * @brief A workspace directory and an escape target beside it.
     * @details Both under the fixture root and outside each other, so a write
     *          that lands in @p outside left the workspace by definition.
     */
    struct Escape
    {
        QString work;
        QString outside;
    };

    Escape makeEscape(const QString &caseName)
    {
        const Escape paths{
            m_fixture.root() + QLatin1Char('/') + caseName + QStringLiteral("/work"),
            m_fixture.root() + QLatin1Char('/') + caseName + QStringLiteral("/outside")};
        QDir().mkpath(paths.work);
        QDir().mkpath(paths.outside);
        return paths;
    }

    /** @brief Contents of a local file, empty when it is not readable. */
    static QByteArray slurp(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    static bool spill(const QString &path, const QByteArray &content)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        return file.write(content) == content.size();
    }

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

/*
 * Containment is a byte-prefix comparison on path strings, so nothing lexical
 * can see a symlink: the workspace spelling of a path and the directory the
 * host reaches through it are two different things. The cases below run the
 * real tools against the real server, and every one of them asserts on the
 * filesystem before it looks at the returned message. A tool that writes
 * outside the workspace and reports a refusal is worse than one that admits
 * it, so the message must never be the first thing checked.
 */
void Test::realpathAnswersWithTheHostsOwnSpelling()
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

    QString resolved;
    QCOMPARE(sftp.realPath(m_fixture.workDir(), &resolved, &err), QSocSftpClient::Presence::Present);
    QVERIFY2(resolved.startsWith(QLatin1Char('/')), qPrintable(resolved));
    QVERIFY2(resolved.endsWith(QStringLiteral("/work")), qPrintable(resolved));

    /* A symlink resolves to what it points at, which is the whole point: two
     * spellings name one directory, and only one of them can be compared
     * against a writable root. */
    const QString link = m_fixture.root() + QStringLiteral("/realpath_alias");
    QVERIFY(QFile::link(m_fixture.workDir(), link));
    QString viaLink;
    QCOMPARE(sftp.realPath(link, &viaLink, &err), QSocSftpClient::Presence::Present);
    QCOMPARE(viaLink, resolved);
}

/*
 * How much of a missing path a server resolves is the server's business:
 * OpenSSH answers a single missing component itself and refuses a deeper one,
 * so the client walk has to produce the same canonical string either way.
 */
void Test::canonicalizeKeepsATailThatDoesNotExistYet()
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

    QString base;
    QCOMPARE(sftp.realPath(m_fixture.workDir(), &base, &err), QSocSftpClient::Presence::Present);

    QString canonical;
    QCOMPARE(
        sftp.canonicalize(m_fixture.workDir() + QStringLiteral("/absent_leaf"), &canonical, &err),
        QSocSftpClient::Canonical::Ok);
    QCOMPARE(canonical, base + QStringLiteral("/absent_leaf"));

    QCOMPARE(
        sftp.canonicalize(
            m_fixture.workDir() + QStringLiteral("/absent/deeper/leaf.sv"), &canonical, &err),
        QSocSftpClient::Canonical::Ok);
    QCOMPARE(canonical, base + QStringLiteral("/absent/deeper/leaf.sv"));
}

/*
 * A name the host holds but cannot follow has no canonical form and therefore
 * no containment. Handing back its lexical spelling would pass a byte-prefix
 * check that the write does not honor.
 */
void Test::canonicalizeRefusesALinkTheHostCannotFollow()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const QString dir = m_fixture.workDir() + QStringLiteral("/unfollowable");
    QVERIFY(QDir().mkpath(dir));
    const QString intoNowhere = dir + QStringLiteral("/into_missing_dir");
    const QString loop        = dir + QStringLiteral("/loop_a");
    QVERIFY(QFile::link(dir + QStringLiteral("/absent_dir/leaf"), intoNowhere));
    QVERIFY(QFile::link(loop, dir + QStringLiteral("/loop_b")));
    QVERIFY(QFile::link(dir + QStringLiteral("/loop_b"), loop));

    QSocSshSession session;
    QString        err;
    if (session.connectTo(hostConfig(static_cast<quint16>(m_fixture.port())), &err)
        != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, m_fixture.log())));
    }
    QSocSftpClient sftp(session);

    for (const QString &path : QStringList{intoNowhere, loop}) {
        QString canonical;
        QCOMPARE(sftp.canonicalize(path, &canonical, &err), QSocSftpClient::Canonical::Unresolvable);
        QVERIFY2(canonical.isEmpty(), qPrintable(canonical));
        QVERIFY2(err.contains(QStringLiteral("does not lead to a file")), qPrintable(err));
    }
}

void Test::writeRefusesADirectorySymlinkOutOfTheWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape paths = makeEscape(QStringLiteral("link_dir_write"));
    QVERIFY(QFile::link(paths.outside, paths.work + QStringLiteral("/linkdir")));
    const QString escaped = paths.outside + QStringLiteral("/escaped.txt");

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString           result = tool.execute(
        json{{"file_path", "linkdir/escaped.txt"}, {"content", "escaped\n"}});

    QVERIFY2(
        !QFileInfo::exists(escaped),
        qPrintable(
            QStringLiteral("the write escaped to %1; the tool said: %2").arg(escaped, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
    QVERIFY2(result.contains(QStringLiteral("outside writable directories")), qPrintable(result));
}

/* The escape is not limited to overwriting: writeFile mkdir -p's the parent,
 * so an unguarded write builds directory trees wherever the link points. */
void Test::writeRefusesToBuildTreesThroughASymlink()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape paths = makeEscape(QStringLiteral("link_dir_tree"));
    QVERIFY(QFile::link(paths.outside, paths.work + QStringLiteral("/linkdir")));
    const QString newTree = paths.outside + QStringLiteral("/newsub");

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString           result = tool.execute(
        json{{"file_path", "linkdir/newsub/deep/x.txt"}, {"content", "escaped\n"}});

    QVERIFY2(
        !QFileInfo::exists(newTree),
        qPrintable(QStringLiteral("a directory tree was created at %1; the tool said: %2")
                       .arg(newTree, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
}

void Test::editRefusesADirectorySymlinkOutOfTheWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape     paths    = makeEscape(QStringLiteral("link_dir_edit"));
    const QByteArray original = QByteArray("keep me exactly as I am\n");
    const QString    victim   = paths.outside + QStringLiteral("/editme.txt");
    QVERIFY(spill(victim, original));
    QVERIFY(QFile::link(paths.outside, paths.work + QStringLiteral("/linkdir")));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    /* Read first, so the read-before-edit guard is satisfied and the write
     * guard is the only thing left between the tool and the escape. */
    QSocToolRemoteFileRead reader(nullptr, &conn, conn.path());
    const QString          read = reader.execute(json{{"file_path", "linkdir/editme.txt"}});
    QVERIFY2(read.contains(QStringLiteral("keep me")), qPrintable(read));

    QSocToolRemoteFileEdit tool(nullptr, &conn, conn.path());
    const QString          result = tool.execute(
        json{
            {"file_path", "linkdir/editme.txt"},
            {"old_string", "keep me"},
            {"new_string", "lose me"}});

    QVERIFY2(
        slurp(victim) == original,
        qPrintable(QStringLiteral("the edit escaped to %1; the tool said: %2").arg(victim, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
    QVERIFY2(result.contains(QStringLiteral("outside writable directories")), qPrintable(result));
}

/*
 * A symlinked FILE is the case the publish sequence hid: the staged temp lands
 * in the link's own directory and the final rename replaces the link, so the
 * target outside the workspace survives by accident. The guard resolves the
 * last component too, so the refusal no longer depends on how writeFile
 * happens to stage its content, and the link is left intact.
 */
void Test::writeRefusesAFileSymlinkOutOfTheWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape     paths    = makeEscape(QStringLiteral("link_file_write"));
    const QByteArray original = QByteArray("outside content that must survive\n");
    const QString    victim   = paths.outside + QStringLiteral("/target.txt");
    const QString    link     = paths.work + QStringLiteral("/link.txt");
    QVERIFY(spill(victim, original));
    QVERIFY(QFile::link(victim, link));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileRead reader(nullptr, &conn, conn.path());
    const QString          read = reader.execute(json{{"file_path", "link.txt"}});
    QVERIFY2(read.contains(QStringLiteral("outside content")), qPrintable(read));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString           result = tool.execute(
        json{{"file_path", "link.txt"}, {"content", "replacement\n"}});

    QVERIFY2(
        slurp(victim) == original,
        qPrintable(
            QStringLiteral("the write escaped to %1; the tool said: %2").arg(victim, result)));
    QVERIFY2(
        QFileInfo(link).isSymbolicLink(),
        qPrintable(
            QStringLiteral("%1 is no longer a symlink; the tool said: %2").arg(link, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
    QVERIFY2(result.contains(QStringLiteral("outside writable directories")), qPrintable(result));
}

/*
 * A link into the workspace is followed, so the file the agent read is the
 * file it writes and the link survives. The alternative, replacing the link
 * with a regular file, is what the temp+rename publish does by accident, and
 * it silently drops an indirection the workspace was built with.
 */
void Test::writeThroughAnInWorkspaceLinkKeepsTheLink()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape  paths = makeEscape(QStringLiteral("link_inside_write"));
    const QString real  = paths.work + QStringLiteral("/real.txt");
    const QString alias = paths.work + QStringLiteral("/alias.txt");
    QVERIFY(spill(real, QByteArray("first\n")));
    QVERIFY(QFile::link(real, alias));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileRead reader(nullptr, &conn, conn.path());
    const QString          read = reader.execute(json{{"file_path", "alias.txt"}});
    QVERIFY2(read.contains(QStringLiteral("first")), qPrintable(read));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString result = tool.execute(json{{"file_path", "alias.txt"}, {"content", "second\n"}});

    QCOMPARE(slurp(real), QByteArray("second\n"));
    QVERIFY2(
        QFileInfo(alias).isSymbolicLink(),
        qPrintable(
            QStringLiteral("%1 is no longer a symlink; the tool said: %2").arg(alias, result)));
    QVERIFY2(!result.startsWith(QStringLiteral("Error:")), qPrintable(result));
}

/* The target of a link is where the write lands, so a link whose target is
 * outside the workspace is refused even while the target does not exist yet. */
void Test::writeRefusesADeadLinkThatPointsOutOfTheWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape  paths = makeEscape(QStringLiteral("link_dead_outward"));
    const QString gone  = paths.outside + QStringLiteral("/gone.txt");
    const QString link  = paths.work + QStringLiteral("/dangling.txt");
    QVERIFY(QFile::link(gone, link));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString           result = tool.execute(
        json{{"file_path", "dangling.txt"}, {"content", "escaped\n"}});

    QVERIFY2(
        !QFileInfo::exists(gone),
        qPrintable(QStringLiteral("the write escaped to %1; the tool said: %2").arg(gone, result)));
    QVERIFY2(
        QFileInfo(link).isSymbolicLink(),
        qPrintable(
            QStringLiteral("%1 is no longer a symlink; the tool said: %2").arg(link, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
    QVERIFY2(result.contains(QStringLiteral("outside writable directories")), qPrintable(result));
}

/* Nothing can be said about the containment of a name the host cannot follow,
 * so the write is refused rather than aimed at the lexical spelling. */
void Test::writeRefusesALinkTheHostCannotFollow()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape  paths = makeEscape(QStringLiteral("link_unfollowable_write"));
    const QString loop  = paths.work + QStringLiteral("/loop_a");
    QVERIFY(QFile::link(loop, paths.work + QStringLiteral("/loop_b")));
    QVERIFY(QFile::link(paths.work + QStringLiteral("/loop_b"), loop));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString result = tool.execute(json{{"file_path", "loop_a"}, {"content", "guessed\n"}});

    QVERIFY2(
        QFileInfo(loop).isSymbolicLink(),
        qPrintable(
            QStringLiteral("%1 is no longer a symlink; the tool said: %2").arg(loop, result)));
    QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
    QVERIFY2(result.contains(QStringLiteral("does not lead to a file")), qPrintable(result));
}

/* The lexical rule already refused this one. It stays a case because nothing
 * covered it end to end, and canonicalizing must not weaken it. */
void Test::writeRefusesADotDotEscape()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape  paths   = makeEscape(QStringLiteral("dotdot_write"));
    const QString control = paths.outside + QStringLiteral("/control.txt");

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString           result = tool.execute(
        json{{"file_path", "../outside/control.txt"}, {"content", "escaped\n"}});

    QVERIFY2(
        !QFileInfo::exists(control),
        qPrintable(
            QStringLiteral("the write escaped to %1; the tool said: %2").arg(control, result)));
    QVERIFY2(result.contains(QStringLiteral("outside writable directories")), qPrintable(result));
}

/*
 * The other half of canonicalizing: the workspace itself is often reached
 * through a symlink (a temp directory under /var, for one), and comparing a
 * canonical path against a lexical writable directory would refuse every
 * write inside it.
 */
void Test::writeWorksInAWorkspaceReachedThroughASymlink()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const QString base = m_fixture.root() + QStringLiteral("/symlinked_workspace");
    const QString real = base + QStringLiteral("/real");
    const QString via  = base + QStringLiteral("/via_link");
    QVERIFY(QDir().mkpath(real));
    QVERIFY(QFile::link(real, via));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, via, &err), qPrintable(err));

    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    const QString result = tool.execute(json{{"file_path", "inside.txt"}, {"content", "allowed\n"}});

    QCOMPARE(slurp(real + QStringLiteral("/inside.txt")), QByteArray("allowed\n"));
    QVERIFY2(!result.startsWith(QStringLiteral("Error:")), qPrintable(result));
}

void Test::writeAndEditStillWorkOnAnOrdinaryPath()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    const Escape  paths = makeEscape(QStringLiteral("ordinary_paths"));
    const QString file  = paths.work + QStringLiteral("/sub/plain.sv");

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    QSocToolRemoteFileWrite writer(nullptr, &conn, conn.path());
    const QString           wrote = writer.execute(
        json{{"file_path", "sub/plain.sv"}, {"content", "module a;\nendmodule\n"}});
    QCOMPARE(slurp(file), QByteArray("module a;\nendmodule\n"));
    QVERIFY2(!wrote.startsWith(QStringLiteral("Error:")), qPrintable(wrote));

    QSocToolRemoteFileRead reader(nullptr, &conn, conn.path());
    const QString          read = reader.execute(json{{"file_path", "sub/plain.sv"}});
    QVERIFY2(read.contains(QStringLiteral("module a;")), qPrintable(read));

    QSocToolRemoteFileEdit editor(nullptr, &conn, conn.path());
    const QString          edited = editor.execute(
        json{{"file_path", "sub/plain.sv"}, {"old_string", "module a;"}, {"new_string", "module b;"}});
    QCOMPARE(slurp(file), QByteArray("module b;\nendmodule\n"));
    QVERIFY2(!edited.startsWith(QStringLiteral("Error:")), qPrintable(edited));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsftp_loopback.moc"
