// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#include "qsoc_test_sshd.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QString>
#include <QtTest>

#include <chrono>
#include <thread>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#endif

/*
 * What happens to a session after an SFTP request is abandoned at its
 * deadline. libssh2 keeps read, write and request-id state per LIBSSH2_SFTP,
 * so the unit that becomes unusable is the subsystem; the SSH session below
 * it survives whenever that subsystem can still be released. Both halves are
 * asserted here, because condemning the session either always or never is
 * wrong in one of them.
 *
 * Every case enters the data phase through the client's own observer seam
 * rather than by racing a transfer. Budgets keep the files small; the one
 * exception is the case that has to leave an SSH packet half sent, which needs
 * more payload than the socket can swallow.
 */

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void anAbandonedWriteLeavesTheNextWriteWorking();
    void aWriteAbandonedOnADeadLinkCondemnsTheSession();
    void anAbandonedReadCondemnsTheSessionItStranded();
    void aWriteStrandedMidPacketCondemnsTheSession();
    void aTeardownOnASilentLinkStillReleasesTheSession();

private:
    /** @brief Connect @p session to @p port, failing the case on refusal. */
    bool connectOrFail(QSocSshSession *session, quint16 port)
    {
        QString err;
        if (session->connectTo(m_fixture.hostConfig(port), &err)
            != QSocSshSession::ConnectStatus::Ok) {
            const QString text = QStringLiteral("connect to port %1 failed: %2\n--- sshd ---\n%3")
                                     .arg(port)
                                     .arg(err, m_fixture.log());
            QTest::qFail(qPrintable(text), __FILE__, __LINE__);
            return false;
        }
        return true;
    }

    /** @brief Whether @p dir holds a leftover staging temp. */
    static bool hasStagingTemp(QSocSftpClient *client, const QString &dir)
    {
        QString    listErr;
        const auto entries = client->listDir(dir, 0, &listErr);
        for (const auto &entry : entries) {
            if (entry.name.startsWith(QStringLiteral(".qsoc-write-"))) {
                return true;
            }
        }
        return false;
    }

    /** @brief Pin @p fd's send buffer small, disabling kernel auto-tuning. */
    static bool capSendBuffer(qintptr fd)
    {
#ifdef Q_OS_WIN
        Q_UNUSED(fd)
        return false;
#else
        const int bytes = kStalledSendBufferBytes;
        return fd >= 0
               && ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes))
                      == 0;
#endif
    }

    /* Payload past everything the stalled path can still absorb, so libssh2
     * still has chunks queued when the socket stops taking them and one of
     * them is left half sent. */
    static constexpr int kStalledWriteBytes      = 256 * 1024;
    static constexpr int kStalledSendBufferBytes = 8 * 1024;

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

/*
 * A slow-but-alive link is the common case, and it must cost the caller one
 * write, not the workspace. The subsystem is released and rebuilt; the SSH
 * session underneath is untouched.
 */
void Test::anAbandonedWriteLeavesTheNextWriteWorking()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocSshSession session;
    QVERIFY(connectOrFail(&session, static_cast<quint16>(m_fixture.port())));

    const QString    dir    = m_fixture.workDir() + QStringLiteral("/stranded_write");
    const QString    first  = dir + QStringLiteral("/abandoned.sv");
    const QString    second = dir + QStringLiteral("/next.sv");
    const QByteArray payload("content the write after the abandoned one must deliver\n");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(300);
    /* Spends the whole budget before the first libssh2_sftp_write, so the
     * request goes out and its reply is never collected. */
    bool overspent = false;
    sftp.setDataPhaseObserver([&overspent] {
        if (overspent) {
            return;
        }
        overspent = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });

    QString err;
    QVERIFY(!sftp.writeFile(first, QByteArray("never lands\n"), &err));
    QVERIFY2(overspent, "the write never reached its data phase");
    QVERIFY(!err.isEmpty());
    QVERIFY2(err.contains(QStringLiteral("acknowledged")), qPrintable(err));
    QVERIFY(sftp.lastFailureUncertain());
    QCOMPARE(sftp.lastBytesAcked(), 0);

    /* The link was healthy the whole time, so the session must survive. */
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::No);
    QVERIFY2(session.isConnected(), "an abandoned write condemned a healthy session");

    sftp.setDataPhaseObserver({});
    sftp.setOperationTimeoutMs(5000);
    QVERIFY2(sftp.writeFile(second, payload, &err), qPrintable(err));

    QSocSshSession fresh;
    QVERIFY(connectOrFail(&fresh, static_cast<quint16>(m_fixture.port())));
    QSocSftpClient check(fresh);
    QCOMPARE(check.readFile(second), payload);
    QCOMPARE(check.presence(first), QSocSftpClient::Presence::Absent);
    QVERIFY2(!hasStagingTemp(&check, dir), "the abandoned write left its temp behind");
}

/*
 * The same abandon on a link that stopped answering: the subsystem cannot be
 * released, which is the detection for a transport no later caller can put
 * back in sync, so the session is condemned. Nothing may be deleted on the
 * way out, because a stranded subsystem has one unlink request-id slot and
 * would report the abandoned request's reply as the unlink's result.
 */
void Test::aWriteAbandonedOnADeadLinkCondemnsTheSession()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QVERIFY(connectOrFail(&session, relay.port()));

    const QString dir  = m_fixture.workDir() + QStringLiteral("/stranded_dead_write");
    const QString path = dir + QStringLiteral("/target.sv");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(400);
    sftp.setDataPhaseObserver([&relay] { relay.blackhole(); });

    QString err;
    QVERIFY(!sftp.writeFile(path, QByteArray("payload that never lands\n"), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(sftp.lastFailureUncertain());
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::AbandonedExchange);

    QElapsedTimer clock;
    clock.start();
    QString secondErr;
    QVERIFY(sftp.readFile(path, 0, &secondErr).isNull());
    QVERIFY(!secondErr.isEmpty());
    QVERIFY2(
        clock.elapsed() < 200,
        qPrintable(QStringLiteral("a condemned session retried for %1 ms").arg(clock.elapsed())));

    relay.heal();
    QSocSshSession recovery;
    QVERIFY(connectOrFail(&recovery, static_cast<quint16>(m_fixture.port())));
    QSocSftpClient check(recovery);
    QVERIFY2(hasStagingTemp(&check, dir), "a temp was unlinked while the subsystem was stranded");
}

/*
 * A read strands `read_state`, which sftp_close resets only for the handle it
 * closes, so a healthy file would read back as a protocol error on the next
 * call. The subsystem it stranded must not be reused.
 */
void Test::anAbandonedReadCondemnsTheSessionItStranded()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QVERIFY(connectOrFail(&session, relay.port()));

    const QString    path = m_fixture.workDir() + QStringLiteral("/stranded_read.sv");
    const QByteArray payload("content that is readable until the link goes silent\n");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(5000);
    QString err;
    QVERIFY2(sftp.writeFile(path, payload, &err), qPrintable(err));

    sftp.setOperationTimeoutMs(400);
    sftp.setDataPhaseObserver([&relay] { relay.blackhole(); });
    QVERIFY(sftp.readFile(path, 0, &err).isNull());
    QVERIFY(!err.isEmpty());
    QVERIFY2(!session.isConnected(), "a read abandoned on a dead link left the session usable");

    QElapsedTimer clock;
    clock.start();
    QString secondErr;
    QVERIFY(sftp.readFile(path, 0, &secondErr).isNull());
    QVERIFY(!secondErr.isEmpty());
    QVERIFY2(
        clock.elapsed() < 200,
        qPrintable(QStringLiteral("a condemned session retried for %1 ms").arg(clock.elapsed())));
}

/*
 * The unrescuable strand: the peer's receive window closes while libssh2 is
 * partway through an SSH packet, so the tail of that packet stays in the
 * transport's own send state. libssh2 answers every later send with EAGAIN
 * once the arguments no longer match the saved ones, which is permanent for
 * the whole session, so the SFTP channel's CHANNEL_CLOSE can never go out and
 * the bounded drain cannot finish. That is the detection for a transport no
 * later caller can put back in sync.
 */
void Test::aWriteStrandedMidPacketCondemnsTheSession()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
    relay.start();
    QVERIFY(relay.waitUntilListening(5000));
    const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

    QSocSshSession session;
    QVERIFY(connectOrFail(&session, relay.port()));
    /* An auto-tuned send buffer holds a couple of hundred kilobytes, more than
     * a whole in-flight write window, and the client then finishes every
     * packet it starts no matter what the peer does. Pinning it small is what
     * makes the send run out of room partway through one. */
    QVERIFY(capSendBuffer(session.socketFd()));

    const QString path = m_fixture.workDir() + QStringLiteral("/stranded_midpacket/target.sv");

    QSocSftpClient sftp(session);
    sftp.setOperationTimeoutMs(400);
    /* Shuts the window before the first libssh2_sftp_write, so the send that
     * follows blocks inside a packet instead of merely going unanswered. */
    sftp.setDataPhaseObserver([&relay] { relay.stopReading(); });

    QString err;
    QVERIFY(!sftp.writeFile(path, QByteArray(kStalledWriteBytes, 'x'), &err));
    QCOMPARE(session.unusableReason(), QSocSshSession::Unusable::AbandonedExchange);
    QVERIFY(!err.isEmpty());
    QVERIFY(sftp.lastFailureUncertain());
    QCOMPARE(sftp.lastBytesAcked(), 0);
}

/*
 * Teardown is where a non-blocking libssh2_session_free has to be driven to
 * completion: one call that returns EAGAIN has freed nothing, so nulling the
 * pointer there leaks the session, every channel and the SFTP channel. On a
 * peer that stopped answering the release still has to happen, and the caller
 * has to be able to tell that it was not the peer that confirmed it.
 */
void Test::aTeardownOnASilentLinkStillReleasesTheSession()
{
    QSOC_REQUIRE_SSHD(m_fixture);

    {
        QSocTestRelay relay(static_cast<quint16>(m_fixture.port()));
        relay.start();
        QVERIFY(relay.waitUntilListening(5000));
        const auto relayGuard = qScopeGuard([&relay] { relay.stop(); });

        QSocSshSession session;
        QVERIFY(connectOrFail(&session, relay.port()));
        QSocSftpClient sftp(session);
        QString        err;
        QVERIFY2(
            sftp.writeFile(
                m_fixture.workDir() + QStringLiteral("/teardown_silent.sv"),
                QByteArray("alive\n"),
                &err),
            qPrintable(err));
        QVERIFY(sftp.isOpen());

        relay.blackhole();
        QElapsedTimer clock;
        clock.start();
        session.disconnectFromHost();
        const qint64 elapsed = clock.elapsed();

        QVERIFY2(
            elapsed >= 1500,
            qPrintable(
                QStringLiteral("teardown gave up after %1 ms without draining").arg(elapsed)));
        QVERIFY2(elapsed < 6000, qPrintable(QStringLiteral("teardown took %1 ms").arg(elapsed)));
        QVERIFY2(
            !session.lastTeardownCompleted(),
            "a silent peer was reported as having confirmed the close");
        QCOMPARE(session.rawSession(), static_cast<LIBSSH2_SESSION *>(nullptr));

        clock.restart();
        session.disconnectFromHost();
        QVERIFY2(
            clock.elapsed() < 200,
            qPrintable(QStringLiteral("a second teardown took %1 ms").arg(clock.elapsed())));
        QVERIFY(!session.lastTeardownCompleted());
    }

    /* And the other half: a live peer confirms, so the same teardown is
     * quick and reports itself complete. */
    QSocSshSession session;
    QVERIFY(connectOrFail(&session, static_cast<quint16>(m_fixture.port())));
    QSocSftpClient sftp(session);
    QString        err;
    QVERIFY2(
        sftp.writeFile(
            m_fixture.workDir() + QStringLiteral("/teardown_live.sv"), QByteArray("alive\n"), &err),
        qPrintable(err));
    QVERIFY(sftp.isOpen());

    QElapsedTimer clock;
    clock.start();
    session.disconnectFromHost();
    const qint64 elapsed = clock.elapsed();
    QVERIFY2(elapsed < 500, qPrintable(QStringLiteral("teardown took %1 ms").arg(elapsed)));
    QVERIFY2(session.lastTeardownCompleted(), "a confirmed teardown reported itself incomplete");
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsftpstranded.moc"
