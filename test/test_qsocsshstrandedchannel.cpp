// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QElapsedTimer>
#include <QtCore>
#include <QtTest>

/*
 * What a command that outlives its budget leaves behind.
 *
 * libssh2 will not release a channel until the peer confirms the close, and the
 * peer does not confirm while the remote process is still running. So a command
 * that runs past its timeout always leaves a channel the call cannot free. The
 * cases here pin down where that channel goes: it must not be dropped, because
 * an unfreed channel stays registered and takes delivery of packets nobody
 * reads, and because libssh2_session_free stops at the first channel it cannot
 * free, which costs the orderly disconnect too.
 */

namespace {

/* Long enough that the remote process is still running when the budget runs
 * out, short enough that the same case can then wait for it to finish. */
constexpr int kRemoteSleepSec = 3;

/* Budget for the command that is meant to run out of budget. */
constexpr int kShortBudgetMs = 800;

/* Budget for the commands that are meant to finish. */
constexpr int kNormalBudgetMs = 20000;

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { m_sshd.start(); }

    void cleanupTestCase()
    {
        m_sshd.stop();
        m_sshd.removeRoot();
    }

    /*
     * A command that outruns its budget hands its channel to the session, and
     * the session keeps issuing the release until it takes. Dropping it instead
     * leaves nothing to retry and nothing to count.
     */
    void aChannelThatOutlivesItsBudgetIsQueuedAndReleased()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        QSocSshSession session;
        QString        err;
        QCOMPARE(session.connectTo(m_sshd.hostConfig(), &err), QSocSshSession::ConnectStatus::Ok);

        QSocSshExec exec(session);
        const auto  timedOut
            = exec.run(QStringLiteral("sleep %1").arg(kRemoteSleepSec), kShortBudgetMs);
        QVERIFY2(
            timedOut.timedOut,
            qPrintable(QStringLiteral("expected a timeout, got exit %1 %2")
                           .arg(timedOut.exitCode)
                           .arg(timedOut.errorText)));
        QCOMPARE(
            session.strandedChannelCount(),
            1); /* the release the call could not finish is on the queue */
        /* The link itself is fine, and the session says so. */
        QVERIFY(session.isConnected());

        /* Once the remote process exits the peer confirms the close, so the
         * queued release goes through on the next command. */
        QTest::qWait((kRemoteSleepSec * 1000) + 500);
        const auto after = exec.run(QStringLiteral("echo alive"), kNormalBudgetMs);
        QCOMPARE(after.exitCode, 0);
        QCOMPARE(after.stdoutBytes.trimmed(), QByteArray("alive"));
        QCOMPARE(session.strandedChannelCount(), 0);

        session.disconnectFromHost();
    }

    /*
     * And a teardown with a release still queued has to get libssh2 to let go of
     * the session. Anything less is the one leak nothing can reclaim: the
     * library keeps the session and there is no second way to release it.
     */
    void aTeardownWithAQueuedReleaseStillFreesTheSession()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        QSocSshSession session;
        QString        err;
        QCOMPARE(session.connectTo(m_sshd.hostConfig(), &err), QSocSshSession::ConnectStatus::Ok);

        QSocSshExec exec(session);
        const auto  timedOut
            = exec.run(QStringLiteral("sleep %1").arg(kRemoteSleepSec), kShortBudgetMs);
        QVERIFY(timedOut.timedOut);
        QCOMPARE(session.strandedChannelCount(), 1);

        QElapsedTimer clock;
        clock.start();
        session.disconnectFromHost();
        const qint64 elapsedMs = clock.elapsed();
        qInfo("teardown with a queued release took %lld ms", elapsedMs);

        QVERIFY2(
            session.lastTeardownReleasedState(),
            qPrintable(QStringLiteral(
                           "libssh2 kept the session after %1 ms of teardown; nothing "
                           "can reclaim it now")
                           .arg(elapsedMs)));
        QVERIFY(!session.isConnected());
        QCOMPARE(session.strandedChannelCount(), 0);
    }

    /*
     * A release that goes through before the session is freed keeps the orderly
     * disconnect available: libssh2_session_free walks the channels itself and
     * gives up on the first one the peer has not confirmed, and giving up there
     * is what forces the socket to be closed under the library instead.
     */
    void aReleasedChannelKeepsTheDisconnectOrderly()
    {
        QSOC_REQUIRE_SSHD(m_sshd);
        QSocSshSession session;
        QString        err;
        QCOMPARE(session.connectTo(m_sshd.hostConfig(), &err), QSocSshSession::ConnectStatus::Ok);

        QSocSshExec exec(session);
        const auto  timedOut
            = exec.run(QStringLiteral("sleep %1").arg(kRemoteSleepSec), kShortBudgetMs);
        QVERIFY(timedOut.timedOut);
        QTest::qWait((kRemoteSleepSec * 1000) + 500);

        session.disconnectFromHost();
        QVERIFY(session.lastTeardownReleasedState());
        QVERIFY2(
            session.lastTeardownCompleted(),
            "the disconnect fell back to closing the socket under libssh2");
    }

private:
    QSocTestSshd m_sshd;
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsshstrandedchannel.moc"
