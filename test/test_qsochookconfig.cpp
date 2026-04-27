// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochooktypes.h"
#include "qsoc_test.h"

#include <yaml-cpp/yaml.h>

#include <QtCore>
#include <QtTest>

namespace {

YAML::Node loadYaml(const char *text)
{
    return YAML::Load(text);
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc_test";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app(argc, argv.data());
    }

    void eventKeyRoundTrip()
    {
        for (auto event : allHookEvents()) {
            const QString key   = hookEventToYamlKey(event);
            const auto    round = hookEventFromYamlKey(key);
            QVERIFY(round.has_value());
            QCOMPARE(*round, event);
        }
    }

    void eventKeysAreSnakeCase()
    {
        QCOMPARE(hookEventToYamlKey(QSocHookEvent::PreToolUse), QStringLiteral("pre_tool_use"));
        QCOMPARE(hookEventToYamlKey(QSocHookEvent::PostToolUse), QStringLiteral("post_tool_use"));
        QCOMPARE(
            hookEventToYamlKey(QSocHookEvent::UserPromptSubmit),
            QStringLiteral("user_prompt_submit"));
        QCOMPARE(hookEventToYamlKey(QSocHookEvent::SessionStart), QStringLiteral("session_start"));
        QCOMPARE(hookEventToYamlKey(QSocHookEvent::Stop), QStringLiteral("stop"));
    }

    void unknownKeyReturnsNullopt()
    {
        QVERIFY(!hookEventFromYamlKey(QStringLiteral("PreToolUse")).has_value());
        QVERIFY(!hookEventFromYamlKey(QStringLiteral("foo")).has_value());
        QVERIFY(!hookEventFromYamlKey({}).has_value());
    }

    void parseUndefinedReturnsEmpty()
    {
        const YAML::Node node;
        const auto       cfg = QSocHookConfig::parseFromYaml(node);
        QVERIFY(cfg.isEmpty());
        QCOMPARE(cfg.totalCommands(), 0);
    }

    void parseNonMapReturnsEmpty()
    {
        const YAML::Node node = loadYaml("- not_a_map\n");
        const auto       cfg  = QSocHookConfig::parseFromYaml(node);
        QVERIFY(cfg.isEmpty());
    }

    void parseSingleEventSingleCommand()
    {
        const YAML::Node node = loadYaml(
            "pre_tool_use:\n"
            "  - matcher: \"shell|file\"\n"
            "    hooks:\n"
            "      - type: command\n"
            "        command: /tmp/audit.sh\n"
            "        timeout: 5\n");
        const auto cfg = QSocHookConfig::parseFromYaml(node);
        QVERIFY(!cfg.isEmpty());
        QCOMPARE(cfg.totalCommands(), 1);

        const auto matchers = cfg.matchersFor(QSocHookEvent::PreToolUse);
        QCOMPARE(matchers.size(), qsizetype(1));
        QCOMPARE(matchers.at(0).matcher, QStringLiteral("shell|file"));
        QCOMPARE(matchers.at(0).commands.size(), qsizetype(1));
        const auto &cmd = matchers.at(0).commands.at(0);
        QCOMPARE(cmd.type, QStringLiteral("command"));
        QCOMPARE(cmd.command, QStringLiteral("/tmp/audit.sh"));
        QCOMPARE(cmd.timeoutSec, 5);
    }

    void omittedMatcherIsEmptyString()
    {
        const YAML::Node node = loadYaml(
            "post_tool_use:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/log.sh\n");
        const auto cfg      = QSocHookConfig::parseFromYaml(node);
        const auto matchers = cfg.matchersFor(QSocHookEvent::PostToolUse);
        QCOMPARE(matchers.size(), qsizetype(1));
        QVERIFY(matchers.at(0).matcher.isEmpty());
    }

    void timeoutDefaultsToTenSeconds()
    {
        const YAML::Node node = loadYaml(
            "stop:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/done.sh\n");
        const auto cfg      = QSocHookConfig::parseFromYaml(node);
        const auto matchers = cfg.matchersFor(QSocHookEvent::Stop);
        QCOMPARE(matchers.at(0).commands.at(0).timeoutSec, 10);
    }

    void invalidEntriesAreSkipped()
    {
        const YAML::Node node = loadYaml(
            "pre_tool_use:\n"
            "  - hooks:\n"
            "      - type: command\n" /* missing command field */
            "      - type: prompt\n"  /* unsupported type */
            "        command: /tmp/x.sh\n"
            "      - type: command\n"
            "        command: /tmp/ok.sh\n");
        const auto cfg = QSocHookConfig::parseFromYaml(node);
        QCOMPARE(cfg.totalCommands(), 1);
        const auto matchers = cfg.matchersFor(QSocHookEvent::PreToolUse);
        QCOMPARE(matchers.at(0).commands.at(0).command, QStringLiteral("/tmp/ok.sh"));
    }

    void unknownEventIsSkipped()
    {
        const YAML::Node node = loadYaml(
            "PreToolUse:\n" /* PascalCase is not accepted */
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/x.sh\n"
            "stop:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/y.sh\n");
        const auto cfg = QSocHookConfig::parseFromYaml(node);
        QCOMPARE(cfg.totalCommands(), 1);
        QVERIFY(cfg.matchersFor(QSocHookEvent::PreToolUse).isEmpty());
        QCOMPARE(cfg.matchersFor(QSocHookEvent::Stop).size(), qsizetype(1));
    }

    void multipleEventsAreParsed()
    {
        const YAML::Node node = loadYaml(
            "pre_tool_use:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/a.sh\n"
            "post_tool_use:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/b.sh\n"
            "user_prompt_submit:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/c.sh\n"
            "session_start:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/d.sh\n"
            "stop:\n"
            "  - hooks:\n"
            "      - type: command\n"
            "        command: /tmp/e.sh\n");
        const auto cfg = QSocHookConfig::parseFromYaml(node);
        QCOMPARE(cfg.totalCommands(), 5);
        for (auto event : allHookEvents()) {
            QCOMPARE(cfg.matchersFor(event).size(), qsizetype(1));
        }
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsochookconfig.moc"
