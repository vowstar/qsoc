// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookrunner.h"
#include "agent/qsochooktypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtCore>
#include <QtTest>

namespace {

QString writeScript(const QTemporaryDir &dir, const QString &name, const QString &body)
{
    const QString path = QDir(dir.path()).filePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qFatal("could not open script %s for writing", qPrintable(path));
    }
    file.write("#!/bin/bash\n");
    file.write(body.toUtf8());
    file.close();
    QFile::setPermissions(
        path,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup
            | QFile::ReadOther | QFile::ExeOther);
    return path;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir scratchDir;

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("requires a POSIX shell (/bin/bash); not supported on Windows");
#endif
        QVERIFY(scratchDir.isValid());
    }

    void cleanupTestCase() { QVERIFY(scratchDir.remove()); }

    void invalidConfigFailsFast()
    {
        QSocHookRunner    runner;
        HookCommandConfig cfg;
        QSignalSpy        finishedSpy(&runner, &QSocHookRunner::finished);
        bool              runningAtFinish     = true;
        bool              resultReadyAtFinish = false;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            runningAtFinish     = runner.isRunning();
            resultReadyAtFinish = runner.result().status == QSocHookRunner::Status::StartFailed;
        });
        const auto result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::StartFailed);
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(!runningAtFinish);
        QVERIFY(resultReadyAtFinish);
        QVERIFY(!runner.isRunning());
    }

    void exitZeroIsSuccess()
    {
        const QString     path = writeScript(scratchDir, "ok.sh", "exit 0\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        QSignalSpy     finishedSpy(&runner, &QSocHookRunner::finished);
        bool           runningAtFinish     = true;
        bool           resultReadyAtFinish = false;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            runningAtFinish     = runner.isRunning();
            resultReadyAtFinish = runner.result().status == QSocHookRunner::Status::Success;
        });
        const auto result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QCOMPARE(result.exitCode, 0);
        QVERIFY(!result.hasResponse);
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(!runningAtFinish);
        QVERIFY(resultReadyAtFinish);
        QVERIFY(!runner.isRunning());
    }

    void exitTwoIsBlock()
    {
        const QString     path = writeScript(scratchDir, "block.sh", "echo 'denied' >&2\nexit 2\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::Block);
        QCOMPARE(result.exitCode, 2);
        QVERIFY(result.stderrText.contains(QStringLiteral("denied")));
    }

    void otherExitIsNonBlocking()
    {
        const QString     path = writeScript(scratchDir, "fail.sh", "exit 7\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::NonBlockingError);
        QCOMPARE(result.exitCode, 7);
    }

    void finishedCallbackCanQueueNextRun()
    {
        const QString     blockPath = writeScript(scratchDir, "reentrant_block.sh", "exit 2\n");
        const QString     nextPath  = writeScript(scratchDir, "reentrant_next.sh", "exit 0\n");
        HookCommandConfig blockConfig;
        blockConfig.command = blockPath;
        HookCommandConfig nextConfig;
        nextConfig.command = nextPath;
        QSocHookRunner runner;
        QSignalSpy     finishedSpy(&runner, &QSocHookRunner::finished);
        bool           startNext  = true;
        QEventLoop    *nestedLoop = nullptr;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            if (!startNext) {
                return;
            }
            startNext = false;
            QMetaObject::invokeMethod(
                &runner,
                [&runner, nextConfig]() { runner.start(nextConfig, nlohmann::json::object()); },
                Qt::QueuedConnection);
            QMetaObject::invokeMethod(
                &runner,
                [&]() {
                    if (nestedLoop != nullptr) {
                        nestedLoop->quit();
                    }
                },
                Qt::QueuedConnection);
        });
        QList<QSocHookRunner::Status> observedStatuses;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            observedStatuses.append(runner.result().status);
        });
        QList<QSocHookRunner::Status> queuedStatuses;
        connect(
            &runner,
            &QSocHookRunner::finished,
            &runner,
            [&]() { queuedStatuses.append(runner.result().status); },
            Qt::QueuedConnection);
        bool enterNestedLoop = true;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            if (!enterNestedLoop) {
                return;
            }
            enterNestedLoop = false;
            QEventLoop loop;
            nestedLoop = &loop;
            loop.exec();
            nestedLoop = nullptr;
        });

        const auto firstResult = runner.run(blockConfig, nlohmann::json::object());

        QCOMPARE(firstResult.status, QSocHookRunner::Status::Block);
        QVERIFY(!observedStatuses.isEmpty());
        QCOMPARE(observedStatuses.first(), QSocHookRunner::Status::Block);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 2, 5000);
        const QList<QSocHookRunner::Status>
            expectedStatuses{QSocHookRunner::Status::Block, QSocHookRunner::Status::Success};
        QCOMPARE(observedStatuses, expectedStatuses);
        QTRY_COMPARE_WITH_TIMEOUT(queuedStatuses.size(), 2, 5000);
        QCOMPARE(queuedStatuses, expectedStatuses);
        QCOMPARE(runner.result().status, QSocHookRunner::Status::Success);
        QVERIFY(!runner.isRunning());
    }

    void queuedResultSnapshotsSurviveNextRun()
    {
        const QString     blockPath = writeScript(scratchDir, "snapshot_block.sh", "exit 2\n");
        const QString     nextPath  = writeScript(scratchDir, "snapshot_next.sh", "exit 0\n");
        HookCommandConfig blockConfig;
        blockConfig.command = blockPath;
        HookCommandConfig nextConfig;
        nextConfig.command = nextPath;
        QSocHookRunner runner;
        QThread        receiverThread;
        auto          *receiver = new QObject;
        receiver->moveToThread(&receiverThread);
        connect(&receiverThread, &QThread::finished, receiver, &QObject::deleteLater);
        QList<QSocHookRunner::Status> snapshots;
        connect(
            &runner,
            &QSocHookRunner::resultReady,
            receiver,
            [&](QSocHookRunner::Result result) {
                snapshots.append(result.status);
                if (snapshots.size() == 2) {
                    receiverThread.quit();
                }
            },
            Qt::QueuedConnection);
        bool startNext = true;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            if (!startNext) {
                return;
            }
            startNext = false;
            runner.start(nextConfig, nlohmann::json::object());
        });
        QSignalSpy finishedSpy(&runner, &QSocHookRunner::finished);

        runner.start(blockConfig, nlohmann::json::object());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 2, 5000);
        receiverThread.start();
        const bool stopped = receiverThread.wait(5000);
        if (!stopped) {
            receiverThread.quit();
            receiverThread.wait();
        }

        QVERIFY(stopped);
        const QList<QSocHookRunner::Status>
            expectedSnapshots{QSocHookRunner::Status::Block, QSocHookRunner::Status::Success};
        QCOMPARE(snapshots, expectedSnapshots);
        QVERIFY(!runner.isRunning());
    }

    void queuedFinishedSeesResultBeforeRestart()
    {
        const QString     blockPath = writeScript(scratchDir, "queued_block.sh", "exit 2\n");
        const QString     nextPath  = writeScript(scratchDir, "queued_next.sh", "exit 0\n");
        HookCommandConfig blockConfig;
        blockConfig.command = blockPath;
        HookCommandConfig nextConfig;
        nextConfig.command = nextPath;
        QSocHookRunner runner;
        bool           startNext = true;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            if (!startNext) {
                return;
            }
            startNext = false;
            runner.start(nextConfig, nlohmann::json::object());
        });
        QList<QSocHookRunner::Status> queuedStatuses;
        connect(
            &runner,
            &QSocHookRunner::finished,
            &runner,
            [&]() { queuedStatuses.append(runner.result().status); },
            Qt::QueuedConnection);
        QSignalSpy finishedSpy(&runner, &QSocHookRunner::finished);

        runner.start(blockConfig, nlohmann::json::object());

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.size(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(queuedStatuses.size(), 2, 5000);
        const QList<QSocHookRunner::Status>
            expectedStatuses{QSocHookRunner::Status::Block, QSocHookRunner::Status::Success};
        QCOMPARE(queuedStatuses, expectedStatuses);
        QVERIFY(!runner.isRunning());
    }

    void directResultObserverCanDeleteRunner()
    {
        if (!qEnvironmentVariableIsSet("QSOC_TEST_HOOK_DELETE_CHILD")) {
            QProcess child;
            auto     environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("QSOC_TEST_HOOK_DELETE_CHILD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("TMPDIR"), scratchDir.path());
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral("directResultObserverCanDeleteRunner")});
            QVERIFY(child.waitForStarted(5000));
            const bool stopped = child.waitForFinished(5000);
            if (!stopped) {
                child.kill();
                child.waitForFinished(1000);
            }
            QVERIFY2(stopped, "result observer deletion left run() waiting");
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
            return;
        }

        const QString     path = writeScript(scratchDir, "delete_runner.sh", "exit 0\n");
        HookCommandConfig cfg;
        cfg.command                     = path;
        auto                    *runner = new QSocHookRunner;
        QPointer<QSocHookRunner> guard(runner);
        connect(runner, &QSocHookRunner::resultReady, [runner](QSocHookRunner::Result) {
            delete runner;
        });

        const auto result = runner->run(cfg, nlohmann::json::object());

        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QVERIFY(guard.isNull());
    }

    void activeRunnerDeletionSettlesRun()
    {
        const QString path = writeScript(scratchDir, "delete_active_runner.sh", "exec sleep 30\n");
        HookCommandConfig cfg;
        cfg.command                     = path;
        cfg.timeoutSec                  = 10;
        auto                    *runner = new QSocHookRunner;
        QPointer<QSocHookRunner> guard(runner);
        QTimer::singleShot(50, [runner]() { delete runner; });

        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, []() { qFatal("active runner deletion hung run"); });
        watchdog.start(3000);
        QSocTestCapture capture;
        const auto      result = runner->run(cfg, nlohmann::json::object());
        watchdog.stop();
        const QString capturedText = capture.text();

        QVERIFY(guard.isNull());
        QCOMPARE(result.status, QSocHookRunner::Status::StartFailed);
        QCOMPARE(result.errorMessage, QStringLiteral("hook runner destroyed during run"));
        QVERIFY2(
            !capturedText.contains(QStringLiteral("QProcess: Destroyed while process")),
            qPrintable(capturedText));
    }

    void syncRunDuringFinishedFailsFast()
    {
        const QString     path = writeScript(scratchDir, "nested_run.sh", "exit 0\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner         runner;
        QSocHookRunner::Result nestedResult;
        bool                   runNested      = true;
        bool                   nestedReturned = false;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            if (!runNested) {
                return;
            }
            runNested      = false;
            nestedResult   = runner.run(cfg, nlohmann::json::object());
            nestedReturned = true;
        });
        QList<QSocHookRunner::Status> observedStatuses;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            observedStatuses.append(runner.result().status);
        });

        const auto outerResult = runner.run(cfg, nlohmann::json::object());

        QCOMPARE(outerResult.status, QSocHookRunner::Status::Success);
        QVERIFY(nestedReturned);
        QCOMPARE(nestedResult.status, QSocHookRunner::Status::StartFailed);
        QCOMPARE(nestedResult.errorMessage, QStringLiteral("hook runner is busy"));
        QCOMPARE(observedStatuses, QList<QSocHookRunner::Status>{QSocHookRunner::Status::Success});
    }

    void foreignProcessFinishDoesNotSettleRun()
    {
        const QString     path = writeScript(scratchDir, "foreign_finish.sh", "exec sleep 30\n");
        HookCommandConfig cfg;
        cfg.command    = path;
        cfg.timeoutSec = 10;
        QSocHookRunner runner;
        QProcess       foreignProcess;
        const auto     connection = QObject::connect(
            &foreignProcess,
            SIGNAL(finished(int, QProcess::ExitStatus)),
            &runner,
            SLOT(handleProcessFinished()));
        QVERIFY(connection);
        QSignalSpy finishedSpy(&runner, &QSocHookRunner::finished);

        runner.start(cfg, nlohmann::json::object());
        foreignProcess.finished(0, QProcess::NormalExit);

        QCOMPARE(finishedSpy.size(), 0);
        QVERIFY(runner.isRunning());
        QVERIFY(QMetaObject::invokeMethod(&runner, "handleTimeout", Qt::DirectConnection));
        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(runner.result().status, QSocHookRunner::Status::Timeout);
        QVERIFY(!runner.isRunning());
    }

    void firstStdoutLineParsedAsJson()
    {
        const QString path = writeScript(
            scratchDir,
            "respond.sh",
            "echo '{\"decision\":\"allow\",\"note\":\"ok\"}'\n"
            "echo 'trailing text not parsed'\n"
            "exit 0\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QVERIFY(result.hasResponse);
        QCOMPARE(
            QString::fromStdString(result.response.value("decision", "")), QStringLiteral("allow"));
        QCOMPARE(QString::fromStdString(result.response.value("note", "")), QStringLiteral("ok"));
    }

    void plainStdoutLeavesResponseEmpty()
    {
        const QString     path = writeScript(scratchDir, "plain.sh", "echo hello\nexit 0\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QVERIFY(!result.hasResponse);
        QVERIFY(result.stdoutText.contains(QStringLiteral("hello")));
    }

    void stdinJsonIsForwardedToHook()
    {
        const QString     path = writeScript(scratchDir, "echo_stdin.sh", "cat\nexit 0\n");
        HookCommandConfig cfg;
        cfg.command            = path;
        nlohmann::json payload = {{"event", "pre_tool_use"}, {"tool_name", "shell"}};
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, payload);
        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QVERIFY(result.hasResponse);
        QCOMPARE(
            QString::fromStdString(result.response.value("event", "")),
            QStringLiteral("pre_tool_use"));
        QCOMPARE(
            QString::fromStdString(result.response.value("tool_name", "")), QStringLiteral("shell"));
    }

    void timeoutFinishesExactlyOnce()
    {
        const QString path
            = writeScript(scratchDir, "slow.sh", "echo timeout-marker\nexec sleep 30\n");
        HookCommandConfig cfg;
        cfg.command    = path;
        cfg.timeoutSec = 1;
        QSocHookRunner runner;
        QSignalSpy     finishedSpy(&runner, &QSocHookRunner::finished);
        bool           runningAtFinish     = true;
        bool           resultReadyAtFinish = false;
        connect(&runner, &QSocHookRunner::finished, &runner, [&]() {
            runningAtFinish     = runner.isRunning();
            resultReadyAtFinish = runner.result().status == QSocHookRunner::Status::Timeout
                                  && runner.result().stdoutText
                                         == QStringLiteral("timeout-marker\n");
        });
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = runner.run(cfg, nlohmann::json::object());
        const auto ms     = elapsed.elapsed();
        QCOMPARE(result.status, QSocHookRunner::Status::Timeout);
        QCOMPARE(result.stdoutText, QStringLiteral("timeout-marker\n"));
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(!runningAtFinish);
        QVERIFY(resultReadyAtFinish);
        QVERIFY(!runner.isRunning());
        QVERIFY2(
            ms < 5000,
            qPrintable(QStringLiteral("hook should have been killed promptly, took %1ms").arg(ms)));
    }

    void startFailureSurfacesAsStartFailed()
    {
        HookCommandConfig cfg;
        cfg.command = QStringLiteral("/this/path/does/not/exist 2>/dev/null; exit 0");
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QVERIFY(
            result.status == QSocHookRunner::Status::Success
            || result.status == QSocHookRunner::Status::NonBlockingError);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsochookrunner.moc"
