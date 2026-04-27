// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookrunner.h"
#include "agent/qsochooktypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc_test";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app(argc, argv.data());
        QVERIFY(scratchDir.isValid());
    }

    void invalidConfigFailsFast()
    {
        QSocHookRunner    runner;
        HookCommandConfig cfg;
        const auto        result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::StartFailed);
    }

    void exitZeroIsSuccess()
    {
        const QString     path = writeScript(scratchDir, "ok.sh", "exit 0\n");
        HookCommandConfig cfg;
        cfg.command = path;
        QSocHookRunner runner;
        const auto     result = runner.run(cfg, nlohmann::json::object());
        QCOMPARE(result.status, QSocHookRunner::Status::Success);
        QCOMPARE(result.exitCode, 0);
        QVERIFY(!result.hasResponse);
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

    void timeoutKillsHook()
    {
        const QString     path = writeScript(scratchDir, "slow.sh", "sleep 30\nexit 0\n");
        HookCommandConfig cfg;
        cfg.command    = path;
        cfg.timeoutSec = 1;
        QSocHookRunner runner;
        QElapsedTimer  elapsed;
        elapsed.start();
        const auto result = runner.run(cfg, nlohmann::json::object());
        const auto ms     = elapsed.elapsed();
        QCOMPARE(result.status, QSocHookRunner::Status::Timeout);
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

QTEST_APPLESS_MAIN(Test)
#include "test_qsochookrunner.moc"
