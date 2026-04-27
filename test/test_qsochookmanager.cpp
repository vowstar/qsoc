// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookmanager.h"
#include "agent/qsochooktypes.h"
#include "qsoc_test.h"

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

QString writeScript(const QTemporaryDir &dir, const QString &name, const QString &body)
{
    const QString path = QDir(dir.path()).filePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qFatal("could not open script %s", qPrintable(path));
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

QSocHookConfig configWithSinglePreToolUse(
    const QString &matcher, const QString &command, int timeoutSec = 10)
{
    QSocHookConfig    cfg;
    HookCommandConfig cmd;
    cmd.command    = command;
    cmd.timeoutSec = timeoutSec;
    HookMatcherConfig group;
    group.matcher = matcher;
    group.commands.append(cmd);
    cfg.byEvent.insert(QSocHookEvent::PreToolUse, {group});
    return cfg;
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

    /* Matcher behavior */

    void emptyPatternMatchesAll()
    {
        QVERIFY(QSocHookManager::matches(QString(), QStringLiteral("anything")));
        QVERIFY(QSocHookManager::matches(QString(), QString()));
    }

    void wildcardMatchesAll()
    {
        QVERIFY(QSocHookManager::matches(QStringLiteral("*"), QStringLiteral("shell")));
    }

    void exactNameMatcher()
    {
        QVERIFY(QSocHookManager::matches(QStringLiteral("shell"), QStringLiteral("shell")));
        QVERIFY(!QSocHookManager::matches(QStringLiteral("shell"), QStringLiteral("file")));
    }

    void pipeSeparatedMatcher()
    {
        QVERIFY(QSocHookManager::matches(QStringLiteral("shell|file"), QStringLiteral("shell")));
        QVERIFY(QSocHookManager::matches(QStringLiteral("shell|file"), QStringLiteral("file")));
        QVERIFY(!QSocHookManager::matches(QStringLiteral("shell|file"), QStringLiteral("path")));
    }

    void regexMatcher()
    {
        QVERIFY(QSocHookManager::matches(QStringLiteral("^read.*"), QStringLiteral("read_file")));
        QVERIFY(!QSocHookManager::matches(QStringLiteral("^read.*"), QStringLiteral("write_file")));
    }

    void invalidRegexFailsClosed()
    {
        QVERIFY(
            !QSocHookManager::matches(QStringLiteral("[unterminated"), QStringLiteral("anything")));
    }

    /* Fire behavior */

    void fireWithoutMatchersIsNoop()
    {
        QSocHookManager mgr;
        const auto      out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(!out.blocked);
        QVERIFY(out.rawResults.isEmpty());
    }

    void fireWithUnmatchedSubjectIsNoop()
    {
        const QString   script = writeScript(scratchDir, "audit.sh", "exit 0\n");
        QSocHookManager mgr;
        mgr.setConfig(configWithSinglePreToolUse(QStringLiteral("file"), script));
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(!out.blocked);
        QVERIFY(out.rawResults.isEmpty());
    }

    void fireMatchedAndAllowed()
    {
        const QString   script = writeScript(scratchDir, "allow.sh", "exit 0\n");
        QSocHookManager mgr;
        mgr.setConfig(configWithSinglePreToolUse(QStringLiteral("shell"), script));
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(!out.blocked);
        QCOMPARE(out.rawResults.size(), qsizetype(1));
        QCOMPARE(out.rawResults.at(0).status, QSocHookRunner::Status::Success);
    }

    void blockReasonFromStderr()
    {
        const QString script
            = writeScript(scratchDir, "block_stderr.sh", "echo 'too dangerous' >&2\nexit 2\n");
        QSocHookManager mgr;
        mgr.setConfig(configWithSinglePreToolUse(QStringLiteral("shell"), script));
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(out.blocked);
        QVERIFY(out.blockReason.contains(QStringLiteral("too dangerous")));
    }

    void blockReasonFromResponseField()
    {
        const QString script = writeScript(
            scratchDir,
            "block_json.sh",
            "echo '{\"decision\":\"block\",\"reason\":\"policy violation\"}'\n"
            "exit 2\n");
        QSocHookManager mgr;
        mgr.setConfig(configWithSinglePreToolUse(QStringLiteral("shell"), script));
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(out.blocked);
        QCOMPARE(out.blockReason, QStringLiteral("policy violation"));
    }

    void mergedResponseFromLastSuccessfulHook()
    {
        const QString a
            = writeScript(scratchDir, "first.sh", "echo '{\"note\":\"first\"}'\nexit 0\n");
        const QString b
            = writeScript(scratchDir, "second.sh", "echo '{\"note\":\"second\"}'\nexit 0\n");
        QSocHookConfig    cfg;
        HookCommandConfig cmdA;
        cmdA.command = a;
        HookCommandConfig cmdB;
        cmdB.command = b;
        HookMatcherConfig group;
        group.commands = {cmdA, cmdB};
        cfg.byEvent.insert(QSocHookEvent::PreToolUse, {group});
        QSocHookManager mgr;
        mgr.setConfig(cfg);
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        QVERIFY(!out.blocked);
        QCOMPARE(out.rawResults.size(), qsizetype(2));
        QVERIFY(out.hasMergedResponse);
        QVERIFY(out.mergedResponse["note"] == "first" || out.mergedResponse["note"] == "second");
    }

    void parallelExecutionIsFasterThanSerial()
    {
        const QString     a = writeScript(scratchDir, "slow_a.sh", "sleep 1\nexit 0\n");
        const QString     b = writeScript(scratchDir, "slow_b.sh", "sleep 1\nexit 0\n");
        QSocHookConfig    cfg;
        HookCommandConfig cmdA;
        cmdA.command    = a;
        cmdA.timeoutSec = 5;
        HookCommandConfig cmdB;
        cmdB.command    = b;
        cmdB.timeoutSec = 5;
        HookMatcherConfig group;
        group.commands = {cmdA, cmdB};
        cfg.byEvent.insert(QSocHookEvent::PreToolUse, {group});
        QSocHookManager mgr;
        mgr.setConfig(cfg);

        QElapsedTimer timer;
        timer.start();
        const auto out
            = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), nlohmann::json::object());
        const auto elapsedMs = timer.elapsed();
        QVERIFY(!out.blocked);
        QCOMPARE(out.rawResults.size(), qsizetype(2));
        QVERIFY2(
            elapsedMs < 1800,
            qPrintable(
                QStringLiteral("expected parallel execution under 1.8s, got %1ms").arg(elapsedMs)));
    }

    void payloadReachesHookViaStdin()
    {
        const QString script = writeScript(
            scratchDir,
            "echo_in.sh",
            "cat\n" /* echoes stdin which is the JSON payload */
            "exit 0\n");
        QSocHookManager mgr;
        mgr.setConfig(configWithSinglePreToolUse(QStringLiteral("shell"), script));
        nlohmann::json payload = {{"event", "pre_tool_use"}, {"tool_name", "shell"}};
        const auto     out = mgr.fire(QSocHookEvent::PreToolUse, QStringLiteral("shell"), payload);
        QVERIFY(!out.blocked);
        QCOMPARE(out.rawResults.size(), qsizetype(1));
        QVERIFY(out.rawResults.at(0).hasResponse);
        QCOMPARE(
            QString::fromStdString(out.rawResults.at(0).response.value("event", "")),
            QStringLiteral("pre_tool_use"));
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsochookmanager.moc"
