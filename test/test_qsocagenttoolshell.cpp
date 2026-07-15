// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

#include <optional>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#ifdef Q_OS_UNIX
bool processExists(const qint64 processId)
{
    if (processId <= 0) {
        return false;
    }
    errno = 0;
    return ::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
}
#endif

class IsolatedTestProcess final : public QProcess
{
#if defined(Q_OS_UNIX) && QT_VERSION < 0x060000
protected:
    void setupChildProcess() override
    {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    }
#endif
};

void prepareIsolatedTestProcess(QProcess *process)
{
#if defined(Q_OS_UNIX) && QT_VERSION >= 0x060000
    process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    });
#else
    Q_UNUSED(process);
#endif
}

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

            IsolatedTestProcess child;
            auto                environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("QSOC_TEST_SHELL_MANAGE_CHILD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("TMPDIR"), scratch.path());
            child.setProcessEnvironment(environment);
            prepareIsolatedTestProcess(&child);
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
        const std::string longCommand = "(sleep 8; kill -KILL 0) & exec sleep 30";
        const std::string stubbornCommand
            = "(trap '' TERM; sleep 8; kill -KILL 0) & trap '' TERM; printf ready; exec sleep 30";
        const QString startResult = bash.execute({{"command", longCommand}, {"background", true}});
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
            nestedResult, QString("Error: Another blocking shell call is active for this agent."));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 1, 1000);

        const QString secondStartResult = bash.execute(
            {{"command", longCommand}, {"background", true}});
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
            QString("Error: Another blocking shell call is active for this agent."));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 2, 1000);

        const QString thirdStartResult = bash.execute(
            {{"command", longCommand}, {"background", true}});
        QVERIFY2(!thirdStartResult.startsWith("Error:"), qPrintable(thirdStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int thirdProcessId = QSocToolShellBash::snapshotActive().constFirst().id;

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
            QString("Error: Another blocking shell call is active for this agent."));
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
            {{"command", stubbornCommand}, {"background", true}});
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
            {{"command", longCommand}, {"background", true}});
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
            {{"command", stubbornCommand}, {"background", true}});
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

        const std::string childIgnoresTerm
            = "(trap '' TERM; sleep 8; kill -KILL 0) & "
              "(trap '' TERM; exec sleep 30) & child=$!; "
              "printf 'ready child=%s\\n' \"$child\"; wait \"$child\"";
        const QString eighthStartResult = bash.execute(
            {{"command", childIgnoresTerm}, {"background", true}});
        QVERIFY2(!eighthStartResult.startsWith("Error:"), qPrintable(eighthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int eighthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(
            QSocToolShellBash::tailActive(eighthProcessId, 64).contains("ready"), 1000);
        const QRegularExpression childPattern(QStringLiteral("child=(\\d+)"));
        const auto               childMatch = childPattern.match(
            QSocToolShellBash::tailActive(eighthProcessId, 128));
        QVERIFY(childMatch.hasMatch());
        const qint64 stubbornChildPid = childMatch.captured(1).toLongLong();
        QVERIFY(processExists(stubbornChildPid));

        bool graceProbeRan        = false;
        bool notificationWasEarly = false;
        QTimer::singleShot(100, &bash, [&]() {
            graceProbeRan        = true;
            notificationWasEarly = finishedSpy.size() != 7;
        });
        const QString forceKillResult = manage.execute(
            {{"process_id", eighthProcessId}, {"action", "terminate"}});
        QVERIFY(graceProbeRan);
        QVERIFY(!notificationWasEarly);
        QVERIFY2(
            forceKillResult.startsWith("Process force-killed after terminate timeout (exit code "),
            qPrintable(forceKillResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(stubbornChildPid), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 8, 1000);

        const QString terminalLatchStartResult = bash.execute(
            {{"command", "(trap '' TERM; exec sleep 1) & child=$!; printf ready; wait \"$child\""},
             {"background", true}});
        QVERIFY2(!terminalLatchStartResult.startsWith("Error:"), qPrintable(terminalLatchStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int terminalLatchProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        QTRY_VERIFY_WITH_TIMEOUT(
            QSocToolShellBash::tailActive(terminalLatchProcessId, 64).contains("ready"), 1000);

        QTimer::singleShot(20, &manage, &QSocToolBashManage::abort);
        const QString terminalLatchAbortResult = manage.execute(
            {{"process_id", terminalLatchProcessId}, {"action", "terminate"}});
        QCOMPARE(
            terminalLatchAbortResult,
            QString("Terminate requested; wait aborted; process is still running."));
        QCOMPARE(finishedSpy.size(), 8);
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            manage.execute({{"process_id", terminalLatchProcessId}, {"action", "output"}})
                .contains("(FINISHED)"),
            2000);
        QTest::qWait(100);
        QVERIFY(manage.execute({{"process_id", terminalLatchProcessId}, {"action", "output"}})
                    .contains("(FINISHED)"));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 9, 1000);
        const QString terminalLatchStatus = manage.execute(
            {{"process_id", terminalLatchProcessId}, {"action", "status"}});
        QVERIFY2(terminalLatchStatus.contains("Status: FINISHED"), qPrintable(terminalLatchStatus));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);

        const QString tenthStartResult = bash.execute(
            {{"command", longCommand}, {"background", true}});
        QVERIFY2(!tenthStartResult.startsWith("Error:"), qPrintable(tenthStartResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 1);
        const int     tenthProcessId = QSocToolShellBash::snapshotActive().constFirst().id;
        const QString killResult     = manage.execute(
            {{"process_id", tenthProcessId}, {"action", "kill"}});
        QVERIFY2(killResult.startsWith("Process killed (exit code "), qPrintable(killResult));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 10, 1000);
#endif
    }

    void scopedAbort_matchesForegroundOwner()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        QSocToolRegistry  registry;
        QSocToolShellBash bash(&registry);
        QObject           ownerA;
        QObject           ownerB;
        registry.registerTool(&bash);

        bool timedOut = false;
        QTimer::singleShot(20, &registry, [&]() { registry.abortCalls(&ownerB); });
        QTimer::singleShot(120, &registry, [&]() { registry.abortCalls(&ownerA); });
        QTimer::singleShot(1000, &registry, [&]() {
            timedOut = true;
            registry.abortAll();
        });

        QElapsedTimer elapsed;
        elapsed.start();
        const QString result = registry.executeTool(
            QStringLiteral("bash"),
            {{"command", "sleep 5"}, {"working_directory", QDir::tempPath().toStdString()}},
            &ownerA);

        QVERIFY(!timedOut);
        QVERIFY2(elapsed.elapsed() >= 80, "wrong owner cancelled the foreground command");
        QVERIFY2(elapsed.elapsed() < 1000, "matching owner did not cancel the foreground command");
        QCOMPARE(result, QStringLiteral("Command aborted."));
#endif
    }

    void blockingShellCalls_areOwnerScoped()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        QSocToolRegistry   registry;
        QSocToolShellBash  bash;
        QSocToolBashManage manage;
        QObject            ownerA;
        QObject            ownerB;
        registry.registerTool(&bash);
        registry.registerTool(&manage);

        const json background = {{"command", "exec sleep 5"}, {"background", true}};
        const QString firstStart = registry.executeTool(QStringLiteral("bash"), background, &ownerA);
        QVERIFY2(firstStart.startsWith("Started in background."), qPrintable(firstStart));
        const int     firstProcessId = QSocToolShellBash::snapshotActive().constLast().id;
        const QString secondStart
            = registry.executeTool(QStringLiteral("bash"), background, &ownerA);
        QVERIFY2(secondStart.startsWith("Started in background."), qPrintable(secondStart));
        const int secondProcessId = QSocToolShellBash::snapshotActive().constLast().id;

        QString sameOwnerWait;
        QString otherOwnerWait;
        QTimer::singleShot(0, &registry, [&]() {
            sameOwnerWait = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", secondProcessId}, {"action", "wait"}, {"timeout", 20}},
                &ownerA);
            otherOwnerWait = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", secondProcessId}, {"action", "wait"}, {"timeout", 20}},
                &ownerB);
        });
        const QString outerWait = registry.executeTool(
            QStringLiteral("bash_manage"),
            {{"process_id", firstProcessId}, {"action", "wait"}, {"timeout", 80}},
            &ownerA);

        QCOMPARE(
            sameOwnerWait, QString("Error: Another blocking shell call is active for this agent."));
        QVERIFY2(
            otherOwnerWait.startsWith("Process still running after additional 20ms wait."),
            qPrintable(otherOwnerWait));
        QVERIFY2(
            outerWait.startsWith("Process still running after additional 80ms wait."),
            qPrintable(outerWait));

        QString sameOwnerBash;
        QString sameOwnerManage;
        QString otherOwnerBash;
        QString otherOwnerManage;
        QTimer::singleShot(0, &registry, [&]() {
            sameOwnerBash = registry.executeTool(
                QStringLiteral("bash"), {{"command", "printf blocked"}}, &ownerA);
            sameOwnerManage = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", firstProcessId}, {"action", "wait"}, {"timeout", 20}},
                &ownerA);
            otherOwnerBash
                = registry
                      .executeTool(QStringLiteral("bash"), {{"command", "printf nested"}}, &ownerB);
            otherOwnerManage = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", firstProcessId}, {"action", "wait"}, {"timeout", 20}},
                &ownerB);
        });
        const QString outerBash = registry.executeTool(
            QStringLiteral("bash"), {{"command", "sleep 0.15; printf outer"}}, &ownerA);

        QCOMPARE(
            sameOwnerBash,
            QString(
                "Error: Another blocking shell call is active for this agent. Use "
                "background=true for concurrent commands."));
        QCOMPARE(
            sameOwnerManage,
            QString("Error: Another blocking shell call is active for this agent."));
        QCOMPARE(otherOwnerBash, QString("nested"));
        QVERIFY2(
            otherOwnerManage.startsWith("Process still running after additional 20ms wait."),
            qPrintable(otherOwnerManage));
        QCOMPARE(outerBash, QString("outer"));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 2);
        QSocToolShellBash::killAllActive();
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
#endif
    }

    void blockingShellCalls_shareRegistryFallback()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        QSocToolRegistry   registry;
        QSocToolShellBash  bash;
        QSocToolBashManage manage;
        registry.registerTool(&bash);
        registry.registerTool(&manage);

        const QString started = registry.executeTool(
            QStringLiteral("bash"), {{"command", "exec sleep 5"}, {"background", true}});
        QVERIFY2(started.startsWith("Started in background."), qPrintable(started));
        const int processId = QSocToolShellBash::snapshotActive().constLast().id;

        QString nested;
        QTimer::singleShot(0, &registry, [&]() {
            nested = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", processId}, {"action", "wait"}, {"timeout", 20}});
        });
        const QString outer
            = registry.executeTool(QStringLiteral("bash"), {{"command", "sleep 0.08; printf outer"}});

        QCOMPARE(nested, QString("Error: Another blocking shell call is active for this agent."));
        QCOMPARE(outer, QString("outer"));
        const QString after = registry.executeTool(
            QStringLiteral("bash_manage"),
            {{"process_id", processId}, {"action", "wait"}, {"timeout", 20}});
        QVERIFY2(
            after.startsWith("Process still running after additional 20ms wait."),
            qPrintable(after));
        QSocToolShellBash::killAllActive();
#endif
    }

    void blockingShellCalls_shareProcessAcrossOwners()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        QSocToolRegistry   registry;
        QSocToolShellBash  bash;
        QSocToolBashManage manage;
        QObject            ownerA;
        QObject            ownerB;
        registry.registerTool(&bash);
        registry.registerTool(&manage);

        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString releasePath = scratch.filePath(QStringLiteral("release"));
        const QString command     = QStringLiteral(
                                        "printf shared; while [ ! -e '%1' ]; do sleep 0.01; done")
                                        .arg(releasePath);
        const QString started     = registry.executeTool(
            QStringLiteral("bash"),
            {{"command", command.toStdString()}, {"background", true}},
            &ownerA);
        QVERIFY2(started.startsWith("Started in background."), qPrintable(started));
        const int processId = QSocToolShellBash::snapshotActive().constLast().id;

        bool    releaseCreated = false;
        QString nested;
        QTimer::singleShot(0, &registry, [&]() {
            QTimer::singleShot(20, &registry, [&]() {
                QFile release(releasePath);
                releaseCreated = release.open(QIODevice::WriteOnly);
            });
            nested = registry.executeTool(
                QStringLiteral("bash_manage"),
                {{"process_id", processId}, {"action", "wait"}, {"timeout", 1000}},
                &ownerB);
        });
        const QString outer = registry.executeTool(
            QStringLiteral("bash_manage"),
            {{"process_id", processId}, {"action", "wait"}, {"timeout", 1000}},
            &ownerA);

        QVERIFY(releaseCreated);
        QVERIFY2(nested.startsWith("Process completed (exit code 0):"), qPrintable(nested));
        QVERIFY2(outer.startsWith("Process completed (exit code 0):"), qPrintable(outer));
        QVERIFY(nested.contains(QStringLiteral("shared")));
        QVERIFY(outer.contains(QStringLiteral("shared")));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
#endif
    }

    void blockingShellCalls_ignoreDestroyedOwnerAddress()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        QSocToolRegistry  registry;
        QSocToolShellBash bash;
        registry.registerTool(&bash);

        std::optional<QObject> owner;
        owner.emplace();
        QObject *const originalAddress = &owner.value();
        QString        nested;
        QTimer::singleShot(0, &registry, [&]() {
            owner.reset();
            owner.emplace();
            QCOMPARE(&owner.value(), originalAddress);
            nested = registry.executeTool(
                QStringLiteral("bash"), {{"command", "printf replacement"}}, &owner.value());
        });
        const QString outer = registry.executeTool(
            QStringLiteral("bash"), {{"command", "sleep 0.08; printf outer"}}, originalAddress);

        QCOMPARE(nested, QString("replacement"));
        QCOMPARE(outer, QString("outer"));
#endif
    }

    void foregroundWaits_areSafeAndIsolated()
    {
#ifdef Q_OS_WIN
        QSKIP("The bash tool is unavailable on Windows");
#else
        if (!qEnvironmentVariableIsSet("QSOC_TEST_SHELL_FOREGROUND_CHILD")) {
            QTemporaryDir scratch;
            QVERIFY(scratch.isValid());

            IsolatedTestProcess child;
            auto                environment = QProcessEnvironment::systemEnvironment();
            environment
                .insert(QStringLiteral("QSOC_TEST_SHELL_FOREGROUND_CHILD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("TMPDIR"), scratch.path());
            child.setProcessEnvironment(environment);
            prepareIsolatedTestProcess(&child);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral("foregroundWaits_areSafeAndIsolated")});
            QVERIFY(child.waitForStarted(5000));
            const qint64 childPid  = child.processId();
            const bool   stopped   = child.waitForFinished(12000);
            const bool   cleanExit = stopped && child.exitStatus() == QProcess::NormalExit
                                     && child.exitCode() == 0;
            if (!cleanExit && childPid > 0) {
                ::kill(-static_cast<pid_t>(childPid), SIGKILL);
            }
            if (!stopped) {
                child.waitForFinished(1000);
            }
            const QByteArray childOutput = child.readAllStandardOutput()
                                           + child.readAllStandardError();
            QVERIFY2(stopped, "foreground bash lifecycle scenario hung");
            QVERIFY2(child.exitStatus() == QProcess::NormalExit, childOutput.constData());
            QVERIFY2(child.exitCode() == 0, childOutput.constData());
            return;
        }

        const QString workDir      = QDir::tempPath();
        const QString nestedMarker = QDir(workDir).filePath(QStringLiteral("nested-marker"));
        QFile::remove(nestedMarker);

        QSocToolShellBash bash;
        QSocToolShellBash other;
        const std::string longCommand = "(sleep 8; kill -KILL 0) & exec sleep 30";

        QString sameInstanceResult;
        QString nestedResult;
        QString nestedBackgroundResult;
        QTimer::singleShot(0, &bash, [&]() {
            sameInstanceResult = bash.execute(
                {{"command", "touch same-instance-marker"},
                 {"working_directory", workDir.toStdString()}});
        });
        QTimer::singleShot(0, &other, [&]() {
            nestedResult = other.execute(
                {{"command", "touch nested-marker"}, {"working_directory", workDir.toStdString()}});
            nestedBackgroundResult = other.execute(
                {{"command", longCommand},
                 {"working_directory", workDir.toStdString()},
                 {"background", true}});
        });
        QElapsedTimer elapsed;
        elapsed.start();
        const QString timeoutResult = bash.execute(
            {{"command", longCommand},
             {"working_directory", workDir.toStdString()},
             {"timeout", 20}});
        QVERIFY2(timeoutResult.contains("timed out after 20ms"), qPrintable(timeoutResult));
        QCOMPARE(
            sameInstanceResult,
            QString(
                "Error: Another blocking shell call is active for this agent. Use "
                "background=true for concurrent commands."));
        QCOMPARE(nestedResult, QString("(no output)"));
        QVERIFY2(
            nestedBackgroundResult.startsWith("Started in background."),
            qPrintable(nestedBackgroundResult));
        QVERIFY2(elapsed.elapsed() < 500, "nested foreground command delayed the outer deadline");
        QVERIFY(QFileInfo::exists(nestedMarker));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 2);
        QSocToolShellBash::killAllActive();
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);

        const QString descendantFile = QDir(workDir).filePath(QStringLiteral("descendant.pid"));
        QFile::remove(descendantFile);
        qint64     descendantPid     = -1;
        const auto cleanupDescendant = qScopeGuard([&descendantPid]() {
            if (processExists(descendantPid)) {
                ::kill(static_cast<pid_t>(descendantPid), SIGKILL);
            }
        });
        QTimer     abortWhenReady;
        abortWhenReady.setInterval(5);
        QObject::connect(&abortWhenReady, &QTimer::timeout, &bash, [&]() {
            if (QFileInfo::exists(descendantFile)) {
                abortWhenReady.stop();
                bash.abort();
            }
        });
        abortWhenReady.start();
        const QString abortResult = bash.execute(
            {{"command",
              "(sleep 8; kill -KILL 0) & sleep 30 & "
              "printf '%s' $! > descendant.pid; wait"},
             {"working_directory", workDir.toStdString()},
             {"timeout", 1000}});
        abortWhenReady.stop();
        QCOMPARE(abortResult, QString("Command aborted."));
        QFile pidFile(descendantFile);
        QVERIFY(pidFile.open(QIODevice::ReadOnly | QIODevice::Text));
        descendantPid = QString::fromLatin1(pidFile.readAll()).trimmed().toLongLong();
        pidFile.close();
        QVERIFY(descendantPid > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(descendantPid), 1000);
        descendantPid = -1;
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);

        QCOMPARE(
            bash.execute(
                {{"command", "printf ready"}, {"working_directory", workDir.toStdString()}}),
            QString("ready"));

        const QString selfBackgroundedFile = QDir(workDir).filePath(
            QStringLiteral("self-backgrounded.pid"));
        QFile::remove(selfBackgroundedFile);
        qint64     selfBackgroundedPid     = -1;
        const auto cleanupSelfBackgrounded = qScopeGuard([&selfBackgroundedPid]() {
            if (processExists(selfBackgroundedPid)) {
                ::kill(static_cast<pid_t>(selfBackgroundedPid), SIGKILL);
            }
        });
        QCOMPARE(
            bash.execute(
                {{"command", "sleep 2 & printf '%s' $! > self-backgrounded.pid"},
                 {"working_directory", workDir.toStdString()}}),
            QString("(no output)"));
        QFile selfBackgroundedPidFile(selfBackgroundedFile);
        QVERIFY(selfBackgroundedPidFile.open(QIODevice::ReadOnly | QIODevice::Text));
        selfBackgroundedPid
            = QString::fromLatin1(selfBackgroundedPidFile.readAll()).trimmed().toLongLong();
        selfBackgroundedPidFile.close();
        QVERIFY(processExists(selfBackgroundedPid));
        ::kill(static_cast<pid_t>(selfBackgroundedPid), SIGKILL);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(selfBackgroundedPid), 1000);
        selfBackgroundedPid = -1;

        auto                       *deleted = new QSocToolShellBash;
        QPointer<QSocToolShellBash> deletedGuard(deleted);
        QTimer::singleShot(10, QCoreApplication::instance(), [deleted]() { delete deleted; });
        QCOMPARE(
            deleted->execute(
                {{"command", longCommand},
                 {"working_directory", workDir.toStdString()},
                 {"timeout", 1000}}),
            QString("Command aborted."));
        QVERIFY(deletedGuard.isNull());
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
        QCOMPARE(
            other.execute(
                {{"command", "printf after-delete"}, {"working_directory", workDir.toStdString()}}),
            QString("after-delete"));

        auto         *ownerA      = new QSocToolShellBash;
        auto         *ownerB      = new QSocToolShellBash;
        const QString backgroundA = ownerA->execute(
            {{"command", longCommand},
             {"working_directory", workDir.toStdString()},
             {"background", true}});
        const QString backgroundB = ownerB->execute(
            {{"command", longCommand},
             {"working_directory", workDir.toStdString()},
             {"background", true}});
        QVERIFY2(backgroundA.startsWith("Started in background."), qPrintable(backgroundA));
        QVERIFY2(backgroundB.startsWith("Started in background."), qPrintable(backgroundB));
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 2);
        delete ownerA;
        const auto snapshots = QSocToolShellBash::snapshotActive();
        QCOMPARE(snapshots.size(), 1);
        QVERIFY(snapshots.constFirst().isRunning);
        delete ownerB;
        QCOMPARE(QSocToolShellBash::activeProcessCount(), 0);
#endif
    }
};

} /* namespace */

QSOC_TEST_MAIN(Test)
#include "test_qsocagenttoolshell.moc"
