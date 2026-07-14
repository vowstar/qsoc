// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { QSocToolShellBash::killAllActive(); }

    void prompt_ynVariants()
    {
        /* Detector returns the last match in the trailing slice; the
         * "(y/n)" parenthesized form should win over a vaguer "are you
         * sure" earlier in the same line. */
        QCOMPARE(QSocToolShellBash::detectInteractivePrompt("Continue (y/n)?"), QString("(y/n)"));
        QCOMPARE(QSocToolShellBash::detectInteractivePrompt("Are you sure (Y/N)? "), QString("(Y/N)"));
        /* Bracket variant is intentionally NOT supported: "[n/Y]" gets
         * past the parens-only matcher. */
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("Proceed [n/Y] ").isEmpty());
    }

    void prompt_pressEnter()
    {
        QVERIFY(!QSocToolShellBash::detectInteractivePrompt("Press Enter to continue").isEmpty());
        QVERIFY(!QSocToolShellBash::detectInteractivePrompt("press any key to continue").isEmpty());
    }

    void prompt_password()
    {
        QCOMPARE(
            QSocToolShellBash::detectInteractivePrompt("[sudo] password: "), QString("password:"));
        QCOMPARE(
            QSocToolShellBash::detectInteractivePrompt("Enter passphrase: "),
            QString("passphrase:"));
    }

    void prompt_negativeCases()
    {
        /* Real progress output that mentions yes/no in passing must NOT
         * trigger; the regex is anchored to the trailing fragment. */
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("yes I will do it later").isEmpty());
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("hello world").isEmpty());
        /* "every PR" style trailing words that vaguely look like prompts. */
        QVERIFY(
            QSocToolShellBash::detectInteractivePrompt("printed continue successfully").isEmpty());
    }

    void prompt_promptMidStreamDecaysAsTailGrows()
    {
        /* If "(y/n)?" appeared 200 chars ago, the new tail's last 120
         * chars should not match. The detector slices to 120 chars. */
        QString tail = QStringLiteral("(y/n)?\n") + QString(200, QLatin1Char('x'));
        QVERIFY(QSocToolShellBash::detectInteractivePrompt(tail).isEmpty());
    }

    void managementWaits_areSafeAndAbortable()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        if (!qEnvironmentVariableIsSet("QSOC_TEST_SHELL_MANAGE_CHILD")) {
            QTemporaryDir scratch;
            QVERIFY(scratch.isValid());

            QProcess child;
            auto     environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("QSOC_TEST_SHELL_MANAGE_CHILD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("TMPDIR"), scratch.path());
            child.setProcessEnvironment(environment);
#ifdef Q_OS_UNIX
            child.setChildProcessModifier([]() { ::setpgid(0, 0); });
#endif
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral("managementWaits_areSafeAndAbortable")});
            QVERIFY(child.waitForStarted(5000));
            const qint64 childPid = child.processId();
            const bool   stopped  = child.waitForFinished(12000);
#ifdef Q_OS_UNIX
            const bool cleanExit = stopped && child.exitStatus() == QProcess::NormalExit
                                   && child.exitCode() == 0;
            if (!cleanExit && childPid > 0) {
                ::kill(-static_cast<pid_t>(childPid), SIGKILL);
            }
#endif
            if (!stopped) {
                child.waitForFinished(1000);
            }
            const QByteArray childOutput = child.readAllStandardOutput()
                                           + child.readAllStandardError();
            QVERIFY2(stopped, "bash_manage lifecycle scenario hung");
            QVERIFY2(child.exitStatus() == QProcess::NormalExit, childOutput.constData());
            QVERIFY2(child.exitCode() == 0, childOutput.constData());
            return;
        }

        QSocToolShellBash  bash;
        QSocToolBashManage manage;
        QSignalSpy         finishedSpy(&bash, &QSocToolShellBash::backgroundProcessFinished);
        QVERIFY(finishedSpy.isValid());
        const QString startResult = bash.execute(
            {{"command", "exec sleep 30"}, {"background", true}});
        QVERIFY2(!startResult.startsWith("Error:"), qPrintable(startResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int processId = QSocToolShellBash::snapshotActive().constFirst().id;

        QString nestedResult;
        QTimer::singleShot(0, &manage, [&manage, &nestedResult, processId]() {
            nestedResult = manage.execute(
                {{"process_id", processId}, {"action", "wait"}, {"timeout", 1000}});
        });
        bool removed = false;
        QTimer::singleShot(10, &manage, [&removed, processId]() {
            removed = QSocToolShellBash::killActive(processId);
        });
        const QString result = manage.execute(
            {{"process_id", processId}, {"action", "wait"}, {"timeout", 1000}});

        QVERIFY(removed);
        QVERIFY2(result.startsWith("Process completed (exit code "), qPrintable(result));
        QCOMPARE(
            nestedResult,
            QString("Error: Another bash_manage wait or terminate is already active."));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);

        const QString secondStartResult = bash.execute(
            {{"command", "exec sleep 30"}, {"background", true}});
        QVERIFY2(!secondStartResult.startsWith("Error:"), qPrintable(secondStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int secondProcessId = QSocToolShellBash::snapshotActive().constFirst().id;

        QString terminateResult;
        QTimer::singleShot(0, &manage, [&manage, &terminateResult, secondProcessId]() {
            terminateResult = manage.execute(
                {{"process_id", secondProcessId}, {"action", "terminate"}});
        });
        QTimer::singleShot(10, &manage, [secondProcessId]() {
            QSocToolShellBash::killActive(secondProcessId);
        });
        const QString waitResult = manage.execute(
            {{"process_id", secondProcessId}, {"action", "wait"}, {"timeout", 1000}});

        QVERIFY2(waitResult.startsWith("Process completed (exit code "), qPrintable(waitResult));
        QCOMPARE(
            terminateResult,
            QString("Error: Another bash_manage wait or terminate is already active."));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 2, 1000);

        const QString thirdStartResult = bash.execute(
            {{"command", "exec sleep 30"}, {"background", true}});
        QVERIFY2(!thirdStartResult.startsWith("Error:"), qPrintable(thirdStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int thirdProcessId = QSocToolShellBash::snapshotActive().constFirst().id;

        for (const int timeout : {0, -1}) {
            QCOMPARE(
                manage.execute(
                    {{"process_id", thirdProcessId}, {"action", "wait"}, {"timeout", timeout}}),
                QString("Error: timeout must be a positive integer"));
        }
        QString nestedTimeoutResult;
        QTimer::singleShot(0, &manage, [&manage, &nestedTimeoutResult, thirdProcessId]() {
            nestedTimeoutResult = manage.execute(
                {{"process_id", thirdProcessId}, {"action", "wait"}, {"timeout", 2000}});
        });
        QElapsedTimer timeoutElapsed;
        timeoutElapsed.start();
        const QString timeoutResult = manage.execute(
            {{"process_id", thirdProcessId}, {"action", "wait"}, {"timeout", 20}});
        QVERIFY2(
            timeoutResult.startsWith("Process still running after additional 20ms wait."),
            qPrintable(timeoutResult));
        QCOMPARE(
            nestedTimeoutResult,
            QString("Error: Another bash_manage wait or terminate is already active."));
        QVERIFY2(timeoutElapsed.elapsed() < 500, "nested wait delayed the outer deadline");
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);

        QTimer::singleShot(10, &manage, &QSocToolBashManage::abort);
        const QString abortResult = manage.execute(
            {{"process_id", thirdProcessId}, {"action", "wait"}, {"timeout", 1000}});
        QCOMPARE(abortResult, QString("Wait aborted; process is still running."));
        const auto abortSnapshots = QSocToolShellBash::snapshotActive();
        QCOMPARE(abortSnapshots.size(), 1);
        QVERIFY(abortSnapshots.constFirst().isRunning);
        QVERIFY(QSocToolShellBash::killActive(thirdProcessId));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 3, 1000);

        const QString fourthStartResult = bash.execute(
            {{"command", "trap '' TERM; printf ready; exec sleep 30"}, {"background", true}});
        QVERIFY2(!fourthStartResult.startsWith("Error:"), qPrintable(fourthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int fourthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(
            QSocToolShellBash::tailActive(fourthProcessId, 64).contains("ready"), 1000);

        QTimer::singleShot(10, &manage, &QSocToolBashManage::abort);
        const QString terminateAbortResult = manage.execute(
            {{"process_id", fourthProcessId}, {"action", "terminate"}});
        QCOMPARE(
            terminateAbortResult,
            QString("Terminate requested; wait aborted; process is still running."));
        const auto terminateSnapshots = QSocToolShellBash::snapshotActive();
        QCOMPARE(terminateSnapshots.size(), 1);
        QVERIFY(terminateSnapshots.constFirst().isRunning);
        QSocToolShellBash::killAllActive();
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 4, 1000);

        const QString fifthStartResult = bash.execute(
            {{"command", "exec true"}, {"background", true}});
        QVERIFY2(!fifthStartResult.startsWith("Error:"), qPrintable(fifthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int fifthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(!QSocToolShellBash::snapshotActive().constFirst().isRunning, 1000);
        const QString finishedKillResult = manage.execute(
            {{"process_id", fifthProcessId}, {"action", "kill"}});
        QVERIFY2(
            finishedKillResult.startsWith("Process already finished (exit code 0):"),
            qPrintable(finishedKillResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 5, 1000);

        const QString sixthStartResult = bash.execute(
            {{"command", "exec sleep 30"}, {"background", true}});
        QVERIFY2(!sixthStartResult.startsWith("Error:"), qPrintable(sixthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int sixthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;

        auto                        *deletedWaitManage = new QSocToolBashManage;
        QPointer<QSocToolBashManage> deletedWaitGuard(deletedWaitManage);
        QTimer::singleShot(10, &bash, [deletedWaitManage]() { delete deletedWaitManage; });
        const QString deletedWaitResult = deletedWaitManage->execute(
            {{"process_id", sixthProcessId}, {"action", "wait"}, {"timeout", 1000}});
        QVERIFY(deletedWaitGuard.isNull());
        QCOMPARE(deletedWaitResult, QString("Wait aborted; process is still running."));
        const auto deletedWaitSnapshots = QSocToolShellBash::snapshotActive();
        QCOMPARE(deletedWaitSnapshots.size(), 1);
        QVERIFY(deletedWaitSnapshots.constFirst().isRunning);
        QVERIFY(QSocToolShellBash::killActive(sixthProcessId));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 6, 1000);

        const QString seventhStartResult = bash.execute(
            {{"command", "trap '' TERM; printf ready; exec sleep 30"}, {"background", true}});
        QVERIFY2(!seventhStartResult.startsWith("Error:"), qPrintable(seventhStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int seventhProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(
            QSocToolShellBash::tailActive(seventhProcessId, 64).contains("ready"), 1000);

        auto                        *deletedTerminateManage = new QSocToolBashManage;
        QPointer<QSocToolBashManage> deletedTerminateGuard(deletedTerminateManage);
        QTimer::singleShot(10, &bash, [deletedTerminateManage]() { delete deletedTerminateManage; });
        const QString deletedTerminateResult = deletedTerminateManage->execute(
            {{"process_id", seventhProcessId}, {"action", "terminate"}});
        QVERIFY(deletedTerminateGuard.isNull());
        QCOMPARE(
            deletedTerminateResult,
            QString("Terminate requested; wait aborted; process is still running."));
        const auto deletedTerminateSnapshots = QSocToolShellBash::snapshotActive();
        QCOMPARE(deletedTerminateSnapshots.size(), 1);
        QVERIFY(deletedTerminateSnapshots.constFirst().isRunning);
        QVERIFY(QSocToolShellBash::killActive(seventhProcessId));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 7, 1000);

        const QString eighthStartResult = bash.execute(
            {{"command", "trap '' TERM; printf ready; exec sleep 30"}, {"background", true}});
        QVERIFY2(!eighthStartResult.startsWith("Error:"), qPrintable(eighthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int eighthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(
            QSocToolShellBash::tailActive(eighthProcessId, 64).contains("ready"), 1000);

        const QString forceKillResult = manage.execute(
            {{"process_id", eighthProcessId}, {"action", "terminate"}});
        QVERIFY2(
            forceKillResult.startsWith("Process force-killed after terminate timeout (exit code "),
            qPrintable(forceKillResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 8, 1000);

        const QString ninthStartResult = bash.execute(
            {{"command", "exec sleep 30"}, {"background", true}});
        QVERIFY2(!ninthStartResult.startsWith("Error:"), qPrintable(ninthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int     ninthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        const QString killResult     = manage.execute(
            {{"process_id", ninthProcessId}, {"action", "kill"}});
        QVERIFY2(killResult.startsWith("Process killed (exit code "), qPrintable(killResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 9, 1000);
#endif
    }
};

} /* namespace */

QSOC_TEST_MAIN(Test)
#include "test_qsocagenttoolshell.moc"
