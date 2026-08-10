// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#include "qsoc_test_sshd.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QtTest>

#include <chrono>
#include <thread>

/*
 * A remote command that closes stdout and stderr before it finishes reaches
 * CHANNEL_EOF at once while the process keeps running. RFC 4254 6.10 puts
 * exit-status before CHANNEL_CLOSE, so the status only arrives when the
 * process is done; treating EOF as the end of the command reported a
 * still-running process as a clean exit 0. The first two cases run over a
 * healthy loopback link, because the link is not what they are about. The other
 * two cover the remaining ways a status fails to arrive: the link loses its
 * receive direction mid-command, and the server closes the channel cleanly
 * without ever sending one.
 */

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void execWaitsForTheExitStatusWhenOutputClosesEarly();
    void execReportsUnknownWhenOutputClosesEarlyAndTheProcessOutrunsTheBudget();
    void aHalfClosedLinkInventsNoExitStatus();
    void aChannelClosedWithoutAStatusIsNotACleanExit();

private:
    /** @brief Connect to the fixture, failing the case with the sshd log. */
    void connectOrFail(QSocSshSession &session)
    {
        connectOrFail(session, m_fixture.hostConfig(), m_fixture);
    }
    /** @brief Connect to @p host, failing the case with the sshd log. */
    void connectOrFail(QSocSshSession &session, const QSocSshHostConfig &host)
    {
        connectOrFail(session, host, m_fixture);
    }
    /** @brief Connect to @p host, failing with @p fixture's sshd log. */
    void connectOrFail(
        QSocSshSession &session, const QSocSshHostConfig &host, const QSocTestSshd &fixture);

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

void Test::connectOrFail(
    QSocSshSession &session, const QSocSshHostConfig &host, const QSocTestSshd &fixture)
{
    QString err;
    if (session.connectTo(host, &err) != QSocSshSession::ConnectStatus::Ok) {
        QFAIL(qPrintable(
            QStringLiteral("connect failed: %1\n--- sshd ---\n%2").arg(err, fixture.log())));
    }
}

/*
 * The 5 s sleep must outlast the 2 s courtesy window: a shorter one exits
 * inside it and the case then passes even when nothing waits for the status.
 */
void Test::execWaitsForTheExitStatusWhenOutputClosesEarly()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    connectOrFail(session);

    QSocSshExec   exec(session);
    QElapsedTimer clock;
    clock.start();
    const auto   late    = exec.run(QStringLiteral("exec >/dev/null 2>&1; sleep 5; exit 7"), 20000);
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(late.errorText.isEmpty(), qPrintable(late.errorText));
    QCOMPARE(late.exitCode, 7);
    QVERIFY(!late.timedOut);
    QVERIFY(!late.transportDead);
    QVERIFY(late.exitSignal.isEmpty());
    QVERIFY2(
        elapsed >= 4500,
        qPrintable(
            QStringLiteral("returned after %1 ms, before the command could exit").arg(elapsed)));
    QVERIFY(session.isConnected());
}

/*
 * Same shape, budget far shorter than the command. The status never arrives,
 * so the fate is unknown and must be reported as such rather than guessed;
 * the link is healthy throughout, so the workspace has to survive it.
 */
void Test::execReportsUnknownWhenOutputClosesEarlyAndTheProcessOutrunsTheBudget()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    connectOrFail(session);

    QSocSshExec exec(session);
    const auto  unknown = exec.run(QStringLiteral("exec >/dev/null 2>&1; sleep 30"), 1500);

    QVERIFY(unknown.timedOut);
    QCOMPARE(unknown.exitCode, -1);
    QVERIFY(!unknown.transportDead);
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::No);
    QVERIFY2(session.isConnected(), "an ordinary command timeout condemned the workspace");

    const auto next = exec.run(QStringLiteral("printf still_here"), 10000);
    QCOMPARE(next.exitCode, 0);
    QCOMPARE(next.stdoutBytes, QByteArray("still_here"));
}

/*
 * A link that goes one-way mid-command: the client reaches EOF on recv while
 * its sends are still accepted. The exit-status never arrived, and
 * exit_status carries no companion flag, so the calloc'd zero behind it must
 * not be handed out. libssh2 reports the lost receive direction as a failed
 * channel close, and this pins that a command whose fate is unknown is
 * reported as unknown rather than as a clean exit.
 */
void Test::aHalfClosedLinkInventsNoExitStatus()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    connectOrFail(session, m_fixture.hostConfig(relay.port()));

    QSocSshExec exec(session);
    QCOMPARE(exec.run(QStringLiteral("printf ready"), 10000).exitCode, 0);

    std::thread closer([&relay] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        relay.halfClose();
    });
    const auto  closerGuard = qScopeGuard([&closer] { closer.join(); });

    /* Silent and far longer than the wait before the half-close, so no status
     * can have been sent by the time the receive direction goes away. */
    const auto stranded = exec.run(QStringLiteral("exec >/dev/null 2>&1; sleep 5"), 20000);
    QCOMPARE(stranded.exitCode, -1);
    QVERIFY(stranded.exitSignal.isEmpty());
    QVERIFY(stranded.transportDead);
    QVERIFY(!stranded.errorText.isEmpty());
}

/*
 * A server can close a channel cleanly and never send exit-status. OpenSSH does
 * it under `ChannelTimeout session:*=N`: channel_force_close() sends EOF and
 * CHANNEL_CLOSE and unlinks the session record, so the process's later SIGCHLD
 * finds no session and session_exit_message() never runs. It does not kill the
 * process either, which is why the command's fate is unknown rather than
 * failed. exit_status reads as its allocated zero behind that clean close, so
 * this pins the arrival flag: without it a command still running reported a
 * clean exit 0.
 *
 * Its own sshd: the directive would kill every idle channel the other cases
 * depend on.
 */
void Test::aChannelClosedWithoutAStatusIsNotACleanExit()
{
    QSocTestSshd fixture;
    fixture.setExtraConfig({QStringLiteral("ChannelTimeout session:*=1s")});
    if (!fixture.start() && fixture.state() == QSocTestSshd::State::InitFailed
        && fixture.log().contains(QStringLiteral("ChannelTimeout"), Qt::CaseInsensitive)) {
        QSOC_TEST_MISSING_DEPENDENCY("an sshd that supports ChannelTimeout");
    }
    QSOC_REQUIRE_SSHD(fixture);
    const auto fixtureGuard = qScopeGuard([&fixture] {
        fixture.stop();
        fixture.removeRoot();
    });

    QSocSshSession session;
    connectOrFail(session, fixture.hostConfig(), fixture);

    /* Silent, so the channel is idle from the start and the server closes it
     * long before the command is done. */
    const QString started  = fixture.workDir() + QStringLiteral("/started");
    const QString finished = fixture.workDir() + QStringLiteral("/finished");
    QSocSshExec   exec(session);
    QElapsedTimer clock;
    clock.start();
    const auto unreported = exec.run(
        QStringLiteral("touch %1; sleep 6; touch %2; exit 7").arg(started, finished), 30000);
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(QFile::exists(started), "the command never ran");
    QVERIFY2(
        !QFile::exists(finished),
        qPrintable(QStringLiteral(
                       "the command was already done after %1 ms, so the case proves "
                       "nothing about a status that never arrived")
                       .arg(elapsed)));
    QCOMPARE(unreported.exitCode, -1);
    QVERIFY(unreported.exitSignal.isEmpty());
    QVERIFY(!unreported.transportDead);
    QVERIFY2(session.isConnected(), "a server-closed channel condemned the workspace");
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::No);

    /* The command outlives the report: proof that the exit 7 it eventually
     * reaches was never available to be reported. */
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(finished), 30000);
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsshexecstatus.moc"
