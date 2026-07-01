// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

/*
 * Wire-contract tests for QSocAgent::appendTurnReminder. The per-turn
 * ephemeral reminders must ride as trailing <system-reminder> user-turn
 * content: the cached system prefix (messages[0]) stays byte-stable and no
 * role:"system" message is ever emitted after the history, which some chat
 * templates reject.
 */
class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Empty content is a no-op: the wire is untouched. */
    void testEmptyContentIsNoop()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "hi"}});
        const json before = wire;
        QSocAgent::appendTurnReminder(wire, QString());
        QVERIFY(wire == before);
    }

    /* Content is wrapped in <system-reminder> tags. */
    void testWrapsInSystemReminderTags()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "hi"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("stay on task"));
        const std::string content = wire.back()["content"].get<std::string>();
        QVERIFY(
            content.find("<system-reminder>\nstay on task\n</system-reminder>")
            != std::string::npos);
    }

    /* A trailing user message is folded into, not duplicated. */
    void testFoldsIntoTrailingUserMessage()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "original"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
        const std::string content = wire.back()["content"].get<std::string>();
        QVERIFY(content.rfind("original", 0) == 0);
        QVERIFY(content.find("original\n\n<system-reminder>") != std::string::npos);
    }

    /* A trailing tool result is folded into rather than followed by a
     * second consecutive user turn. */
    void testFoldsIntoTrailingToolMessage()
    {
        json wire = json::array();
        wire.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "result"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("tool"));
        QVERIFY(
            wire.back()["content"].get<std::string>().find("<system-reminder>")
            != std::string::npos);
    }

    /* After an assistant turn, a fresh user message carries the reminder. */
    void testStartsFreshUserTurnAfterAssistant()
    {
        json wire = json::array();
        wire.push_back({{"role", "assistant"}, {"content", "done"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(2));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
    }

    /* Empty wire gets a single trailing user reminder. */
    void testEmptyWireGetsUserMessage()
    {
        json wire = json::array();
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
    }

    /* Multiple reminders accumulate into one tail turn, and no role:"system"
     * message is ever introduced after the head. */
    void testMultipleRemindersAccumulateNoSystemRole()
    {
        json wire = json::array();
        wire.push_back({{"role", "system"}, {"content", "SYSTEM PROMPT"}});
        wire.push_back({{"role", "assistant"}, {"content", "done"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("A"));
        QSocAgent::appendTurnReminder(wire, QStringLiteral("B"));
        QSocAgent::appendTurnReminder(wire, QStringLiteral("C"));
        /* system(0) + assistant(1) + one merged user(2). */
        QCOMPARE(wire.size(), static_cast<std::size_t>(3));

        int systemCount = 0;
        for (std::size_t i = 0; i < wire.size(); ++i) {
            if (wire[i]["role"].get<std::string>() == "system") {
                ++systemCount;
                QCOMPARE(i, static_cast<std::size_t>(0));
            }
        }
        QCOMPARE(systemCount, 1);
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));

        const std::string tail = wire.back()["content"].get<std::string>();
        QVERIFY(tail.find("\nA\n") != std::string::npos);
        QVERIFY(tail.find("\nB\n") != std::string::npos);
        QVERIFY(tail.find("\nC\n") != std::string::npos);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentreminders.moc"
