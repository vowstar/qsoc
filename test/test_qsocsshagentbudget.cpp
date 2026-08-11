// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QElapsedTimer>
#include <QLocalServer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

#include <atomic>

#ifdef QSOC_TEST_WRAP_AUTH_TRANSPORT
namespace {
std::atomic<bool> g_failAuthTransport{false};
std::atomic<int>  g_authTransportDelayMs{0};
} // namespace

extern "C" int __real_libssh2_userauth_publickey_fromfile_ex(
    LIBSSH2_SESSION *session,
    const char      *username,
    unsigned int     usernameLength,
    const char      *publicKey,
    const char      *privateKey,
    const char      *passphrase);

extern "C" int __wrap_libssh2_userauth_publickey_fromfile_ex(
    LIBSSH2_SESSION *session,
    const char      *username,
    unsigned int     usernameLength,
    const char      *publicKey,
    const char      *privateKey,
    const char      *passphrase)
{
    if (g_failAuthTransport.load()) {
        QThread::msleep(static_cast<unsigned long>(g_authTransportDelayMs.load()));
        return LIBSSH2_ERROR_SOCKET_RECV;
    }
    return __real_libssh2_userauth_publickey_fromfile_ex(
        session, username, usernameLength, publicKey, privateKey, passphrase);
}
#endif

/*
 * What an ssh-agent that stops answering costs the caller.
 *
 * Authentication is the one connect stage that used to sit outside every bound
 * the rest of the path honours. A stalled agent socket is the ordinary way to
 * reach it: a forwarded agent whose upstream hop died still accepts the local
 * connection and then never replies. The cases here hold the agent stage to the
 * same deadline and the same stop as the handshake.
 */

namespace {

/* Budget for the whole connect. Large enough that the identity-file route
 * visibly still runs after the agent stage gave up, small enough that a
 * revert-check finishes. */
constexpr int kConnectTimeoutMs = 4000;

/* When a case asks the connect to stop, measured from the call starting. */
constexpr int kAbortAfterMs = 400;

/* What a stop may cost. One slice is 200 ms; this allows a slice, the poll
 * already in flight and scheduling noise. */
constexpr qint64 kAcceptableLatencyMs = 1500;

/* Outer safety net. Any case that reaches this has not returned bounded: the
 * whole point of the fix is that no branch can outlive the connect budget by
 * more than teardown. */
constexpr qint64 kOuterSafetyMs = 20000;

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_sshd.start();
        /* A socket that accepts and never answers. Nothing runs an event loop
         * while connectTo() is inside the agent exchange, so the connection
         * stays in the listen backlog: the client's connect() succeeds and the
         * reply never comes, which is exactly a stalled agent. */
        QVERIFY(m_stallDir.isValid());
        m_stallPath = m_stallDir.path() + QStringLiteral("/agent.sock");
        QVERIFY2(m_stall.listen(m_stallPath), qPrintable(m_stall.errorString()));
    }

    void cleanupTestCase()
    {
        m_stall.close();
        m_sshd.stop();
        m_sshd.removeRoot();
    }

    /*
     * A stalled agent must not swallow the whole connect. The agent is one of
     * several auth routes and the only one that depends on a third process, so
     * an unresponsive one has to leave the routes behind it enough budget to
     * finish. Before the fix this call never returned at all.
     */
    void aStalledAgentLeavesTheIdentityRouteItsBudget()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", m_stallPath.toLocal8Bit());

        QSocSshSession session;
        session.setTimeoutMs(kConnectTimeoutMs);

        QSocSshHostConfig host = m_sshd.hostConfig();
        /* Not IdentitiesOnly, so the agent is tried ahead of the file. */
        host.identitiesOnly = false;

        QString       err;
        QElapsedTimer clock;
        clock.start();
        const auto   status    = session.connectTo(host, &err);
        const qint64 elapsedMs = clock.elapsed();
        qputenv("SSH_AUTH_SOCK", previous);

        qInfo("connect through a stalled agent returned after %lld ms", elapsedMs);
        QVERIFY2(
            elapsedMs < kOuterSafetyMs,
            qPrintable(QStringLiteral("the call took %1 ms on a %2 ms budget")
                           .arg(elapsedMs)
                           .arg(kConnectTimeoutMs)));
        QVERIFY2(
            status == QSocSshSession::ConnectStatus::Ok,
            qPrintable(QStringLiteral("connect failed after %1 ms: %2").arg(elapsedMs).arg(err)));
        QVERIFY(session.isConnected());
        session.disconnectFromHost();
    }

    /*
     * And a stop asked for while the agent is being waited on has to be
     * honoured inside a slice, not after the agent's share of the budget.
     */
    void aStopDuringTheAgentExchangeIsHonoured()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", m_stallPath.toLocal8Bit());

        QSocSshSession session;
        session.setTimeoutMs(kConnectTimeoutMs);

        QElapsedTimer clock;
        qint64        askedAtMs = -1;
        session.setAbortProbe([&clock, &askedAtMs] {
            if (clock.elapsed() < kAbortAfterMs) {
                return false;
            }
            if (askedAtMs < 0) {
                askedAtMs = clock.elapsed();
            }
            return true;
        });

        QSocSshHostConfig host = m_sshd.hostConfig();
        host.identitiesOnly    = false;

        QString err;
        clock.start();
        const auto   status       = session.connectTo(host, &err);
        const qint64 returnedAtMs = clock.elapsed();
        qputenv("SSH_AUTH_SOCK", previous);

        QVERIFY2(
            askedAtMs >= 0,
            qPrintable(QStringLiteral("the stop was never consulted; the call returned after %1 ms")
                           .arg(returnedAtMs)));
        /* Measured from when the stop became true, not from when the code got
         * round to asking. A stage that never consults the probe would otherwise
         * report a perfect latency by moving the start of the interval. */
        const qint64 latencyMs = returnedAtMs - kAbortAfterMs;
        qInfo(
            "agent-stage abort latency: %lld ms (stop true from %d ms, first consulted at %lld ms, "
            "returned at %lld ms)",
            latencyMs,
            kAbortAfterMs,
            askedAtMs,
            returnedAtMs);
        QVERIFY2(
            latencyMs <= kAcceptableLatencyMs,
            qPrintable(QStringLiteral(
                           "the user waited %1 ms after asking the connect to stop; the "
                           "stop was not consulted until %2 ms")
                           .arg(latencyMs)
                           .arg(askedAtMs)));
        /* A user-requested stop is an abort, not a timeout: the two must not
         * report under one status. */
        QCOMPARE(status, QSocSshSession::ConnectStatus::Aborted);
    }

    void aFatalAuthTransportIsNotReportedAsRejectedCredentials()
    {
#ifdef QSOC_TEST_WRAP_AUTH_TRANSPORT
        QSOC_REQUIRE_SSHD(m_sshd);
        QSocSshSession    session;
        QSocSshHostConfig host = m_sshd.hostConfig();
        session.setTimeoutMs(kConnectTimeoutMs);

        g_failAuthTransport.store(true);
        QString    err;
        const auto status = session.connectTo(host, &err);
        g_failAuthTransport.store(false);

        QCOMPARE(status, QSocSshSession::ConnectStatus::NetworkError);
        QVERIFY2(
            err.contains(QStringLiteral("transport failed during authentication")), qPrintable(err));
        QVERIFY2(!err.contains(QStringLiteral("Authentication failed")), qPrintable(err));
#else
        QSKIP("libssh2 auth failure injection is available on Linux linkers");
#endif
    }

    void aFatalAuthTransportOutranksAnExpiredDeadline()
    {
#ifdef QSOC_TEST_WRAP_AUTH_TRANSPORT
        QSOC_REQUIRE_SSHD(m_sshd);
        QSocSshSession    session;
        QSocSshHostConfig host = m_sshd.hostConfig();
        session.setTimeoutMs(kConnectTimeoutMs);

        g_authTransportDelayMs.store(kConnectTimeoutMs + 200);
        g_failAuthTransport.store(true);
        const auto resetFailure = qScopeGuard([] {
            g_failAuthTransport.store(false);
            g_authTransportDelayMs.store(0);
        });

        QString       err;
        QElapsedTimer clock;
        clock.start();
        const auto status = session.connectTo(host, &err);

        QVERIFY2(
            clock.elapsed() >= kConnectTimeoutMs,
            "the injected fatal result arrived before the deadline expired");
        QCOMPARE(status, QSocSshSession::ConnectStatus::NetworkError);
        QVERIFY2(
            err.contains(QStringLiteral("transport failed during authentication")), qPrintable(err));
        QVERIFY2(!err.contains(QStringLiteral("did not finish in time")), qPrintable(err));
#else
        QSKIP("libssh2 auth failure injection is available on Linux linkers");
#endif
    }

    /*
     * An agent that takes the request and then hangs up is not "try again".
     * Reading zero bytes forever is the same unbounded wait as never reading at
     * all, only busier, so the hangup has to end the exchange and let the file
     * route run.
     */
    void anAgentThatHangsUpMidReplyEndsTheExchange()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("python3"));
        }

        QTemporaryDir hangupDir;
        QVERIFY(hangupDir.isValid());
        const QString sockPath = hangupDir.path() + QStringLiteral("/hangup.sock");
        /* Accept, read whatever is asked, answer nothing, close. */
        const QString script = QStringLiteral(
                                   "import socket\n"
                                   "s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
                                   "s.bind(%1)\n"
                                   "s.listen(8)\n"
                                   "while True:\n"
                                   "    c, _ = s.accept()\n"
                                   "    c.recv(4096)\n"
                                   "    c.close()\n")
                                   .arg(QStringLiteral("'%1'").arg(sockPath));
        QProcess      hangup;
        hangup.start(python, {QStringLiteral("-c"), script});
        QVERIFY2(hangup.waitForStarted(5000), qPrintable(hangup.errorString()));
        for (int attempt = 0; attempt < 50 && !QFile::exists(sockPath); ++attempt) {
            QTest::qWait(100);
        }
        QVERIFY2(QFile::exists(sockPath), "the hangup peer never created its socket");

        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", sockPath.toLocal8Bit());

        QSocSshSession session;
        session.setTimeoutMs(kConnectTimeoutMs);
        QSocSshHostConfig host = m_sshd.hostConfig();
        host.identitiesOnly    = false;

        QString       err;
        QElapsedTimer clock;
        clock.start();
        const auto   status    = session.connectTo(host, &err);
        const qint64 elapsedMs = clock.elapsed();
        qputenv("SSH_AUTH_SOCK", previous);

        qInfo("connect past an agent that hung up took %lld ms", elapsedMs);
        QVERIFY2(
            elapsedMs < kOuterSafetyMs,
            qPrintable(QStringLiteral("the call took %1 ms").arg(elapsedMs)));
        QVERIFY2(
            status == QSocSshSession::ConnectStatus::Ok,
            qPrintable(QStringLiteral("connect failed after %1 ms: %2").arg(elapsedMs).arg(err)));
        session.disconnectFromHost();

        hangup.terminate();
        if (!hangup.waitForFinished(3000)) {
            hangup.kill();
            hangup.waitForFinished(2000);
        }
    }

    /*
     * An agent that answers the identity list with a key the server accepts,
     * then hangs up before the signature, makes the client send into a socket
     * whose peer is gone. That send must not raise SIGPIPE and take the whole
     * process down with it: a routine agent hangup is not a crash. Reaching the
     * assertions at all is the proof, since a SIGPIPE would kill this binary
     * mid-connect.
     */
    void anAgentThatHangsUpBeforeSigningDoesNotRaiseSigpipe()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("python3"));
        }

        QTemporaryDir hangupDir;
        QVERIFY(hangupDir.isValid());
        const QString sockPath = hangupDir.path() + QStringLiteral("/presign.sock");
        const QString pubPath  = m_sshd.keyPath() + QStringLiteral(".pub");
        /* Answer the list with the server's own key so the client goes on to
         * sign it, then RST the socket so the signature send lands on a dead
         * peer. SO_LINGER 0 forces the reset over a lingering FIN. */
        const QString script
            = QStringLiteral(
                  "import socket, struct, base64\n"
                  "blob = base64.b64decode(open(%1).read().split()[1])\n"
                  "s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
                  "s.bind(%2)\n"
                  "s.listen(8)\n"
                  "while True:\n"
                  "    c, _ = s.accept()\n"
                  "    h = c.recv(4)\n"
                  "    if len(h) < 4:\n"
                  "        c.close()\n"
                  "        continue\n"
                  "    c.recv(struct.unpack('>I', h)[0])\n"
                  "    body = bytes([12]) + struct.pack('>I', 1)\n"
                  "    body += struct.pack('>I', len(blob)) + blob\n"
                  "    body += struct.pack('>I', 1) + b'k'\n"
                  "    c.sendall(struct.pack('>I', len(body)) + body)\n"
                  "    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,"
                  " struct.pack('ii', 1, 0))\n"
                  "    c.close()\n")
                  .arg(QStringLiteral("'%1'").arg(pubPath), QStringLiteral("'%1'").arg(sockPath));
        QProcess presign;
        presign.start(python, {QStringLiteral("-c"), script});
        QVERIFY2(presign.waitForStarted(5000), qPrintable(presign.errorString()));
        for (int attempt = 0; attempt < 50 && !QFile::exists(sockPath); ++attempt) {
            QTest::qWait(100);
        }
        QVERIFY2(QFile::exists(sockPath), "the pre-sign peer never created its socket");

        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", sockPath.toLocal8Bit());

        QSocSshSession session;
        session.setTimeoutMs(kConnectTimeoutMs);
        QSocSshHostConfig host = m_sshd.hostConfig();
        host.identitiesOnly    = false;
        /* No file route, so the dead agent sign is the path that runs. */
        host.identityFiles = {hangupDir.path() + QStringLiteral("/absent_key")};

        QString       err;
        QElapsedTimer clock;
        clock.start();
        const auto   status    = session.connectTo(host, &err);
        const qint64 elapsedMs = clock.elapsed();
        qputenv("SSH_AUTH_SOCK", previous);
        Q_UNUSED(status);

        qInfo("connect past an agent that hung up before signing took %lld ms", elapsedMs);
        /* The process survived the send: the assertion is that control returned
         * here at all, bounded so a hang does not masquerade as survival. */
        QVERIFY2(
            elapsedMs < kOuterSafetyMs,
            qPrintable(QStringLiteral("the call took %1 ms").arg(elapsedMs)));
        session.disconnectFromHost();

        presign.terminate();
        if (!presign.waitForFinished(3000)) {
            presign.kill();
            presign.waitForFinished(2000);
        }
    }

    /*
     * The bound must not have been bought by dropping agent authentication.
     * A real ssh-agent holding the only key the server accepts has to still
     * get the session authenticated, with no identity file to fall back on.
     */
    void aLiveAgentStillAuthenticates()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QString agentExe = QStandardPaths::findExecutable(QStringLiteral("ssh-agent"));
        const QString addExe   = QStandardPaths::findExecutable(QStringLiteral("ssh-add"));
        if (agentExe.isEmpty() || addExe.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("ssh-agent, ssh-add"));
        }

        QTemporaryDir agentDir;
        QVERIFY(agentDir.isValid());
        const QString sockPath = agentDir.path() + QStringLiteral("/live.sock");
        QProcess      agent;
        agent.start(agentExe, {QStringLiteral("-D"), QStringLiteral("-a"), sockPath});
        QVERIFY2(agent.waitForStarted(5000), qPrintable(agent.errorString()));
        for (int attempt = 0; attempt < 50 && !QFile::exists(sockPath); ++attempt) {
            QTest::qWait(100);
        }
        QVERIFY2(QFile::exists(sockPath), "ssh-agent never created its socket");

        /* A key the server does not accept goes in first, so the case also
         * covers rotation: a rejected agent key has to leave the userauth state
         * fit for the next one. */
        const QString strangerKey = agentDir.path() + QStringLiteral("/stranger");
        QVERIFY(makeKey(strangerKey));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SSH_AUTH_SOCK"), sockPath);
        for (const QString &key : {strangerKey, m_sshd.keyPath()}) {
            QProcess add;
            add.setProcessEnvironment(env);
            add.start(addExe, {key});
            QVERIFY(add.waitForStarted(5000));
            QVERIFY(add.waitForFinished(15000));
            QCOMPARE(add.exitCode(), 0);
        }

        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", sockPath.toLocal8Bit());

        QSocSshSession session;
        session.setTimeoutMs(kConnectTimeoutMs);
        QSocSshHostConfig host = m_sshd.hostConfig();
        host.identitiesOnly    = false;
        /* No usable file route: only the agent can authenticate this. */
        host.identityFiles = {agentDir.path() + QStringLiteral("/absent_key")};

        QString       err;
        QElapsedTimer clock;
        clock.start();
        const auto   status    = session.connectTo(host, &err);
        const qint64 elapsedMs = clock.elapsed();
        qputenv("SSH_AUTH_SOCK", previous);

        qInfo("agent authentication took %lld ms", elapsedMs);
        QVERIFY2(
            status == QSocSshSession::ConnectStatus::Ok,
            qPrintable(QStringLiteral("agent auth failed after %1 ms: %2\n%3")
                           .arg(elapsedMs)
                           .arg(err, m_sshd.log())));
        QVERIFY(session.isConnected());
        session.disconnectFromHost();

        agent.terminate();
        if (!agent.waitForFinished(3000)) {
            agent.kill();
            agent.waitForFinished(2000);
        }
    }

    /*
     * A hop's agent is the local agent. libssh2's own agent client could not be
     * used through a ProxyJump child at all, because its agent bytes ride the
     * session's send/recv callbacks and a child's callbacks lead into the remote
     * channel. Ours has its own socket, so the route has to work.
     */
    void anAgentAuthenticatesAProxyJumpHop()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        const QString agentExe = QStandardPaths::findExecutable(QStringLiteral("ssh-agent"));
        const QString addExe   = QStandardPaths::findExecutable(QStringLiteral("ssh-add"));
        if (agentExe.isEmpty() || addExe.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("ssh-agent, ssh-add"));
        }

        QTemporaryDir agentDir;
        QVERIFY(agentDir.isValid());
        const QString sockPath = agentDir.path() + QStringLiteral("/hop.sock");
        QProcess      agent;
        agent.start(agentExe, {QStringLiteral("-D"), QStringLiteral("-a"), sockPath});
        QVERIFY2(agent.waitForStarted(5000), qPrintable(agent.errorString()));
        for (int attempt = 0; attempt < 50 && !QFile::exists(sockPath); ++attempt) {
            QTest::qWait(100);
        }
        QVERIFY(QFile::exists(sockPath));

        QProcess            add;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SSH_AUTH_SOCK"), sockPath);
        add.setProcessEnvironment(env);
        add.start(addExe, {m_sshd.keyPath()});
        QVERIFY(add.waitForStarted(5000));
        QVERIFY(add.waitForFinished(15000));
        QCOMPARE(add.exitCode(), 0);

        const QByteArray previous = qgetenv("SSH_AUTH_SOCK");
        qputenv("SSH_AUTH_SOCK", sockPath.toLocal8Bit());

        /* The hop and the target are the same server, which is all a jump needs
         * to be a jump: the child's bytes travel inside the parent's channel. */
        QSocSshSession    hop;
        QSocSshHostConfig hopHost = m_sshd.hostConfig();
        QString           err;
        QCOMPARE(hop.connectTo(hopHost, &err), QSocSshSession::ConnectStatus::Ok);

        QSocSshSession target;
        target.setTimeoutMs(kConnectTimeoutMs);
        QSocSshHostConfig targetHost = m_sshd.hostConfig();
        targetHost.identitiesOnly    = false;
        /* No usable file route: only the agent can authenticate the child. */
        targetHost.identityFiles = {agentDir.path() + QStringLiteral("/absent_key")};

        const auto status = target.connectToVia(targetHost, &hop, &err);
        qputenv("SSH_AUTH_SOCK", previous);
        QVERIFY2(
            status == QSocSshSession::ConnectStatus::Ok,
            qPrintable(
                QStringLiteral("agent auth through a hop failed: %1\n%2").arg(err, m_sshd.log())));
        target.disconnectFromHost();
        hop.disconnectFromHost();

        agent.terminate();
        if (!agent.waitForFinished(3000)) {
            agent.kill();
            agent.waitForFinished(2000);
        }
    }

private:
    /* A throwaway RSA key in classic PEM, generated at runtime like every other
     * key these fixtures use. */
    static bool makeKey(const QString &path)
    {
        const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        if (keygen.isEmpty()) {
            return false;
        }
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
             path});
        return proc.waitForStarted(5000) && proc.waitForFinished(30000) && proc.exitCode() == 0
               && QFile::exists(path);
    }

    QSocTestSshd  m_sshd;
    QTemporaryDir m_stallDir;
    QLocalServer  m_stall;
    QString       m_stallPath;
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsshagentbudget.moc"
