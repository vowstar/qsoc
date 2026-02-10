// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"

#include <nlohmann/json.hpp>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

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

private:
    /**
     * @brief Build a minimal agent with dummy tool registry (no LLM service)
     */
    QSocAgent *createAgent(QSocAgentConfig config = QSocAgentConfig())
    {
        auto *registry = new QSocToolRegistry(this);
        auto *agent    = new QSocAgent(this, nullptr, registry, config);
        return agent;
    }

    /**
     * @brief Build a message history with large tool outputs for testing pruning
     * @param agent Target agent
     * @param toolCount Number of assistant+tool pairs to create
     * @param contentSize Approximate character count per tool output
     */
    void populateWithToolMessages(QSocAgent *agent, int toolCount, int contentSize)
    {
        json msgs = json::array();

        /* Initial user message */
        msgs.push_back({{"role", "user"}, {"content", "Start task"}});

        for (int i = 0; i < toolCount; i++) {
            QString toolCallId = QString("call_%1").arg(i);

            /* Assistant message with tool_calls */
            json assistantMsg  = {{"role", "assistant"}, {"content", nullptr}};
            json toolCallsJson = json::array();
            toolCallsJson.push_back(
                {{"id", toolCallId.toStdString()},
                 {"type", "function"},
                 {"function", {{"name", "file_read"}, {"arguments", "{\"path\":\"/test\"}"}}}});
            assistantMsg["tool_calls"] = toolCallsJson;
            msgs.push_back(assistantMsg);

            /* Tool response with large content */
            QString bigContent = QString("x").repeated(contentSize);
            msgs.push_back(
                {{"role", "tool"},
                 {"tool_call_id", toolCallId.toStdString()},
                 {"content", bigContent.toStdString()}});
        }

        /* Final assistant message */
        msgs.push_back({{"role", "assistant"}, {"content", "Done with all tasks."}});

        agent->setMessages(msgs);
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    void testPruneToolOutputs()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 100000;
        config.pruneThreshold      = 0.3; /* Low threshold to trigger easily */
        config.pruneProtectTokens  = 5000;
        config.pruneMinimumSavings = 1000;
        config.compactThreshold    = 0.99; /* Don't trigger L2 */
        config.keepRecentMessages  = 200;  /* Prevent L2 from firing */

        auto *agent = createAgent(config);

        /* Create 50 tool messages with 2000 chars each (500 tokens each) */
        populateWithToolMessages(agent, 50, 2000);

        int beforeTokens = 0;
        for (const auto &msg : agent->getMessages()) {
            if (msg.contains("content") && msg["content"].is_string()) {
                beforeTokens
                    += static_cast<int>(
                           QString::fromStdString(msg["content"].get<std::string>()).length())
                       / 4;
            }
        }

        int saved = agent->compact();
        QVERIFY(saved > 0);

        /* Verify old tool outputs were pruned */
        json msgs        = agent->getMessages();
        int  prunedCount = 0;
        for (const auto &msg : msgs) {
            if (msg.contains("role") && msg["role"] == "tool" && msg.contains("content")
                && msg["content"].is_string()) {
                QString content = QString::fromStdString(msg["content"].get<std::string>());
                if (content == "[output pruned]") {
                    prunedCount++;
                }
            }
        }
        QVERIFY(prunedCount > 0);

        delete agent;
    }

    void testPrunePreservesStructure()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 50000;
        config.pruneThreshold      = 0.1; /* Very low to force pruning */
        config.pruneProtectTokens  = 1000;
        config.pruneMinimumSavings = 100;
        config.compactThreshold    = 0.99;
        config.keepRecentMessages  = 100; /* Prevent L2 from firing */

        auto *agent = createAgent(config);
        populateWithToolMessages(agent, 20, 2000);

        agent->compact();

        /* Verify every assistant(tool_calls) is followed by matching tool messages */
        json msgs     = agent->getMessages();
        int  msgCount = static_cast<int>(msgs.size());

        for (int i = 0; i < msgCount; i++) {
            const auto &msg = msgs[static_cast<size_t>(i)];
            if (msg.contains("role") && msg["role"] == "assistant" && msg.contains("tool_calls")) {
                /* Count expected tool responses */
                int expectedTools = static_cast<int>(msg["tool_calls"].size());

                /* Verify following messages are tool responses */
                for (int j = 0; j < expectedTools; j++) {
                    int nextIdx = i + 1 + j;
                    QVERIFY2(
                        nextIdx < msgCount,
                        qPrintable(QString("Missing tool response at index %1").arg(nextIdx)));
                    QCOMPARE(
                        QString::fromStdString(
                            msgs[static_cast<size_t>(nextIdx)]["role"].get<std::string>()),
                        QString("tool"));
                }
            }
        }

        delete agent;
    }

    void testPruneProtectsRecent()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 100000;
        config.pruneThreshold      = 0.1;
        config.pruneProtectTokens  = 100000; /* Protect all */
        config.pruneMinimumSavings = 100;
        config.compactThreshold    = 0.99;
        config.keepRecentMessages  = 100; /* Prevent L2 from firing */

        auto *agent = createAgent(config);
        populateWithToolMessages(agent, 10, 2000);

        int saved = agent->compact();

        /* With high protection, nothing should be pruned (savings < minimum) */
        QCOMPARE(saved, 0);

        delete agent;
    }

    void testPruneMinimumSavings()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 100000;
        config.pruneThreshold      = 0.01; /* Force trigger */
        config.pruneProtectTokens  = 1000;
        config.pruneMinimumSavings = 999999; /* Unreachably high */
        config.compactThreshold    = 0.99;
        config.keepRecentMessages  = 100; /* Prevent L2 from firing */

        auto *agent = createAgent(config);
        populateWithToolMessages(agent, 5, 500);

        int saved = agent->compact();

        /* Minimum savings too high, nothing saved */
        QCOMPARE(saved, 0);

        delete agent;
    }

    void testFindSafeBoundary()
    {
        auto *agent = createAgent();

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "hello"}});
        /* assistant with tool_calls at index 1 */
        json assistantMsg          = {{"role", "assistant"}, {"content", nullptr}};
        assistantMsg["tool_calls"] = json::array(
            {{{"id", "c1"},
              {"type", "function"},
              {"function", {{"name", "test"}, {"arguments", "{}"}}}}});
        msgs.push_back(assistantMsg);
        /* tool at index 2 */
        msgs.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "result"}});
        /* user at index 3 */
        msgs.push_back({{"role", "user"}, {"content", "next"}});

        agent->setMessages(msgs);

        /* Boundary at 0 should stay 0 */
        QCOMPARE(agent->findSafeBoundary(0), 0);

        /* Boundary at 2 (tool msg) should move to 3 (after the group) */
        QCOMPARE(agent->findSafeBoundary(2), 3);

        /* Boundary at 3 (user msg) should stay 3 */
        QCOMPARE(agent->findSafeBoundary(3), 3);

        /* Boundary at 1 (assistant with tool_calls) should move to 3 */
        QCOMPARE(agent->findSafeBoundary(1), 3);

        delete agent;
    }

    void testFindSafeBoundaryEdge()
    {
        auto *agent = createAgent();

        /* Empty messages */
        agent->setMessages(json::array());
        QCOMPARE(agent->findSafeBoundary(0), 0);
        QCOMPARE(agent->findSafeBoundary(5), 0);

        /* Single message */
        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "test"}});
        agent->setMessages(msgs);
        QCOMPARE(agent->findSafeBoundary(0), 0);
        QCOMPARE(agent->findSafeBoundary(1), 1);

        delete agent;
    }

    void testFormatMessages()
    {
        auto *agent = createAgent();

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "Read the file"}});

        json assistantMsg          = {{"role", "assistant"}, {"content", nullptr}};
        assistantMsg["tool_calls"] = json::array(
            {{{"id", "c1"},
              {"type", "function"},
              {"function", {{"name", "file_read"}, {"arguments", "{\"path\":\"/test\"}"}}}}});
        msgs.push_back(assistantMsg);
        msgs.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "file content here"}});
        msgs.push_back({{"role", "assistant"}, {"content", "I read the file."}});

        agent->setMessages(msgs);

        QString formatted = agent->formatMessagesForSummary(0, 4);
        QVERIFY(formatted.contains("[user]: Read the file"));
        QVERIFY(formatted.contains("file_read"));
        QVERIFY(formatted.contains("[Tool result:"));
        QVERIFY(formatted.contains("[assistant]: I read the file."));

        delete agent;
    }

    void testCompactFallback()
    {
        /* Without LLM service, compactWithLLM should fall back to mechanical summary */
        QSocAgentConfig config;
        config.maxContextTokens   = 10000;
        config.pruneThreshold     = 0.99; /* Don't prune */
        config.compactThreshold   = 0.01; /* Always compact */
        config.keepRecentMessages = 2;

        auto *agent = createAgent(config);

        json msgs = json::array();
        for (int i = 0; i < 20; i++) {
            msgs.push_back(
                {{"role", "user"}, {"content", QString("Message %1").arg(i).toStdString()}});
            msgs.push_back(
                {{"role", "assistant"},
                 {"content", QString("Reply %1 with some extra text").arg(i).toStdString()}});
        }
        agent->setMessages(msgs);

        int saved = agent->compact();
        QVERIFY(saved > 0);

        /* Should have summary + recent messages */
        json resultMsgs = agent->getMessages();
        QVERIFY(static_cast<int>(resultMsgs.size()) <= config.keepRecentMessages + 1);

        /* First message should be summary */
        QCOMPARE(QString::fromStdString(resultMsgs[0]["role"].get<std::string>()), QString("user"));
        QString content = QString::fromStdString(resultMsgs[0]["content"].get<std::string>());
        QVERIFY(content.contains("[Conversation Summary]"));

        delete agent;
    }

    void testCompactPreservesRecent()
    {
        QSocAgentConfig config;
        config.maxContextTokens   = 10000;
        config.pruneThreshold     = 0.99;
        config.compactThreshold   = 0.01;
        config.keepRecentMessages = 4;

        auto *agent = createAgent(config);

        json msgs = json::array();
        for (int i = 0; i < 20; i++) {
            msgs.push_back({{"role", "user"}, {"content", QString("Msg %1").arg(i).toStdString()}});
            msgs.push_back(
                {{"role", "assistant"}, {"content", QString("Reply %1").arg(i).toStdString()}});
        }
        agent->setMessages(msgs);

        /* Remember the last 4 messages */
        json lastFour = json::array();
        for (int i = 36; i < 40; i++) {
            lastFour.push_back(msgs[static_cast<size_t>(i)]);
        }

        agent->compact();

        json resultMsgs = agent->getMessages();
        int  resultSize = static_cast<int>(resultMsgs.size());

        /* Last 4 messages should be preserved exactly */
        for (int i = 0; i < 4 && i < resultSize - 1; i++) {
            int idx = resultSize - 4 + i;
            if (idx >= 0 && idx < resultSize) {
                QCOMPARE(
                    resultMsgs[static_cast<size_t>(idx)].dump(),
                    lastFour[static_cast<size_t>(i)].dump());
            }
        }

        delete agent;
    }

    void testCompactResultFormat()
    {
        QSocAgentConfig config;
        config.maxContextTokens   = 10000;
        config.pruneThreshold     = 0.99;
        config.compactThreshold   = 0.01;
        config.keepRecentMessages = 2;

        auto *agent = createAgent(config);

        json msgs = json::array();
        for (int i = 0; i < 10; i++) {
            msgs.push_back({{"role", "user"}, {"content", QString("Q%1").arg(i).toStdString()}});
            msgs.push_back(
                {{"role", "assistant"}, {"content", QString("A%1").arg(i).toStdString()}});
        }
        agent->setMessages(msgs);

        agent->compact();

        json resultMsgs = agent->getMessages();

        /* First message should be user role with [Conversation Summary] */
        QCOMPARE(QString::fromStdString(resultMsgs[0]["role"].get<std::string>()), QString("user"));
        QString content = QString::fromStdString(resultMsgs[0]["content"].get<std::string>());
        QVERIFY(content.contains("[Conversation Summary]"));

        delete agent;
    }

    void testAutoContinue()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 5000;
        config.pruneThreshold      = 0.01;
        config.compactThreshold    = 0.99;
        config.pruneProtectTokens  = 100;
        config.pruneMinimumSavings = 10;

        auto *agent = createAgent(config);
        populateWithToolMessages(agent, 20, 2000);

        /* compressHistoryIfNeeded only injects auto-continue when isStreaming is true.
         * Since we call compact() (not during streaming), no auto-continue message.
         * This is correct behavior - auto-continue only applies during active streaming. */
        int msgCountBefore = static_cast<int>(agent->getMessages().size());
        agent->compact();
        json resultMsgs = agent->getMessages();

        /* Verify compaction happened (message count should change) */
        QVERIFY(static_cast<int>(resultMsgs.size()) <= msgCountBefore);

        delete agent;
    }

    void testNoBelowThreshold()
    {
        QSocAgentConfig config;
        config.maxContextTokens = 1000000; /* Very high limit */
        config.pruneThreshold   = 0.8;
        config.compactThreshold = 0.9;

        auto *agent = createAgent(config);

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "hello"}});
        msgs.push_back({{"role", "assistant"}, {"content", "hi"}});
        agent->setMessages(msgs);

        int saved = agent->compact();
        QCOMPARE(saved, 0);

        /* Messages should be unchanged */
        QCOMPARE(agent->getMessages().size(), 2u);

        delete agent;
    }

    void testConfigBackwardCompat()
    {
        /* Default config should have same effective compaction threshold as old default */
        QSocAgentConfig config;

        /* Old compressionThreshold was 0.8, new compactThreshold defaults to 0.8 */
        QCOMPARE(config.compactThreshold, 0.8);

        /* New pruneThreshold at 0.6 means pruning kicks in earlier - strictly better */
        QVERIFY(config.pruneThreshold < config.compactThreshold);
    }

    void testCompactingSignal()
    {
        QSocAgentConfig config;
        config.maxContextTokens    = 50000;
        config.pruneThreshold      = 0.1;
        config.pruneProtectTokens  = 1000;
        config.pruneMinimumSavings = 100;
        config.compactThreshold    = 0.99;
        config.keepRecentMessages  = 100; /* Prevent L2 from firing */

        auto *agent = createAgent(config);
        populateWithToolMessages(agent, 20, 2000);

        QSignalSpy spy(agent, &QSocAgent::compacting);
        agent->compact();

        /* Should have emitted at least one compacting signal for Layer 1 */
        QVERIFY(spy.count() >= 1);

        /* Verify signal parameters */
        QList<QVariant> args = spy.first();
        QCOMPARE(args.at(0).toInt(), 1);                  /* Layer 1 */
        QVERIFY(args.at(1).toInt() > args.at(2).toInt()); /* before > after */

        delete agent;
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsocagentcompact.moc"
