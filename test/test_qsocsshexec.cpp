// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

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
};

} // namespace

QTEST_GUILESS_MAIN(Test)
#include "test_qsocsshexec.moc"
