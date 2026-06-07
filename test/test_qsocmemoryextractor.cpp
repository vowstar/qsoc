// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemoryextractor.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

class Test : public QObject
{
    Q_OBJECT

    static json userMsg(const QString &text)
    {
        return json{{"role", "user"}, {"content", text.toStdString()}};
    }
    static json assistantMsg(const QString &text)
    {
        return json{{"role", "assistant"}, {"content", text.toStdString()}};
    }
    static json memoryWriteMsg()
    {
        return json{
            {"role", "assistant"},
            {"content", nullptr},
            {"tool_calls",
             json::array({json{
                 {"id", "c1"}, {"function", {{"name", "memory_write"}, {"arguments", "{}"}}}}})}};
    }

private slots:
    /* Nothing new past the cursor: do not run. */
    void testNothingNew()
    {
        json            messages = json::array({userMsg("hi"), assistantMsg("hello")});
        QSocAgentConfig cfg;
        const auto      decision = QSocMemoryExtractor::decide(messages, 2, cfg);
        QVERIFY(!decision.run);
        QCOMPARE(decision.newCount, 0);
    }

    /* Enough new user/assistant messages: run. */
    void testRunsOnEnoughMessages()
    {
        json            messages = json::array({userMsg("fix the build"), assistantMsg("done")});
        QSocAgentConfig cfg;
        cfg.memoryExtractMinNewMessages = 2;
        const auto decision             = QSocMemoryExtractor::decide(messages, 0, cfg);
        QVERIFY(decision.run);
        QVERIFY(!decision.alreadyWritten);
        QCOMPARE(decision.newCount, 2);
    }

    /* Below the min-new-messages threshold: skip. */
    void testSkipsTrivialTurn()
    {
        json            messages = json::array({userMsg("hi")});
        QSocAgentConfig cfg;
        cfg.memoryExtractMinNewMessages = 2;
        const auto decision             = QSocMemoryExtractor::decide(messages, 0, cfg);
        QVERIFY(!decision.run);
        QCOMPARE(decision.newCount, 1);
    }

    /* Main agent already called memory_write: skip the fork, advance. */
    void testAlreadyWritten()
    {
        json messages = json::array(
            {userMsg("remember X"), memoryWriteMsg(), assistantMsg("saved")});
        QSocAgentConfig cfg;
        cfg.memoryExtractMinNewMessages = 1;
        const auto decision             = QSocMemoryExtractor::decide(messages, 0, cfg);
        QVERIFY(decision.alreadyWritten);
        QVERIFY(!decision.run);
    }

    /* newCount ignores tool and system messages. */
    void testCountsOnlyUserAssistant()
    {
        json messages = json::array(
            {userMsg("q"),
             json{{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "result"}},
             json{{"role", "system"}, {"content", "note"}},
             assistantMsg("a")});
        QSocAgentConfig cfg;
        cfg.memoryExtractMinNewMessages = 2;
        const auto decision             = QSocMemoryExtractor::decide(messages, 0, cfg);
        QCOMPARE(decision.newCount, 2);
        QVERIFY(decision.run);
    }

    /* Cadence/empty edge cases. */
    void testEmptyAndBounds()
    {
        QSocAgentConfig cfg;
        QVERIFY(!QSocMemoryExtractor::decide(json::array(), 0, cfg).run);
        QVERIFY(!QSocMemoryExtractor::decide(json(5), 0, cfg).run); /* not an array */
    }

    /* System prompt states the tool restriction, types, and exclusions. */
    void testSystemPrompt()
    {
        const QString prompt = QSocMemoryExtractor::systemPrompt();
        QVERIFY(prompt.contains("memory_read"));
        QVERIFY(prompt.contains("memory_write"));
        QVERIFY(prompt.contains("user:"));
        QVERIFY(prompt.contains("feedback:"));
        QVERIFY(prompt.contains("project:"));
        QVERIFY(prompt.contains("reference:"));
        QVERIFY(prompt.contains("Do NOT save"));
    }

    /* Manifest: empty marker vs formatted lines. */
    void testManifest()
    {
        QCOMPARE(QSocMemoryExtractor::buildManifest({}), QStringLiteral("(none yet)"));

        QSocMemoryManager::MemoryHeader header;
        header.scope           = QStringLiteral("user");
        header.name            = QStringLiteral("user-role");
        header.type            = QStringLiteral("user");
        header.description     = QStringLiteral("ASIC designer");
        const QString manifest = QSocMemoryExtractor::buildManifest({header});
        QVERIFY(manifest.contains("user-role"));
        QVERIFY(manifest.contains("ASIC designer"));
        QVERIFY(manifest.contains("[user/user]"));
    }

    /* User message embeds manifest, count, and transcript. */
    void testUserMessage()
    {
        const QString msg = QSocMemoryExtractor::buildUserMessage(
            QStringLiteral("TRANSCRIPT"), QStringLiteral("MANIFEST"), 3);
        QVERIFY(msg.contains("TRANSCRIPT"));
        QVERIFY(msg.contains("MANIFEST"));
        QVERIFY(msg.contains("last 3 messages"));
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocmemoryextractor.moc"
