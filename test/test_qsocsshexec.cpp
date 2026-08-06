// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Exec on an un-connected session must not crash and must report the
     * missing session via errorText rather than abort. */
    void testRunWithoutConnectedSession()
    {
        QSocSshSession session;
        QSocSshExec    exec(session);
        const auto     result = exec.run(QStringLiteral("echo hi"), 500);
        QCOMPARE(result.exitCode, -1);
        QVERIFY(result.stdoutBytes.isEmpty());
        QVERIFY(result.stderrBytes.isEmpty());
        QVERIFY(!result.timedOut);
        QVERIFY(!result.errorText.isEmpty());
    }

    /* requestAbort() before run() must leave the object in a sane state and
     * the next run() must still fail gracefully on a disconnected session. */
    void testAbortBeforeRun()
    {
        QSocSshSession session;
        QSocSshExec    exec(session);
        exec.requestAbort();
        const auto result = exec.run(QStringLiteral("true"), 500);
        QCOMPARE(result.exitCode, -1);
        QVERIFY(!result.errorText.isEmpty());
    }

    /* A dead-flagged session must be refused immediately: no waiting out the
     * timeout, no exit status invented for a command that never ran. The
     * flag surviving into the result is covered against a real server in
     * test_qsocsftp_loopback. */
    void testDeadTransportIsRefusedImmediately()
    {
        QSocSshSession session;
        session.markTransportDead();
        QVERIFY(!session.isConnected());
        QSocSshExec   exec(session);
        QElapsedTimer clock;
        clock.start();
        const auto   result  = exec.run(QStringLiteral("echo hi"), 5000);
        const qint64 elapsed = clock.elapsed();
        QCOMPARE(result.exitCode, -1);
        QVERIFY(!result.timedOut);
        QVERIFY(!result.errorText.isEmpty());
        QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsshexec.moc"
