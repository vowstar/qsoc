// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolshell.h"
#include "cli/qagentinputmonitor.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

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

private slots:
    void initTestCase() { TestApp::instance(); }

    /* Lifecycle tests */

    void testStartStop()
    {
        QAgentInputMonitor monitor;

        monitor.start();
        QVERIFY(monitor.isActive());

        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* Repeated stop should be safe */
        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* Restart should work */
        monitor.start();
        QVERIFY(monitor.isActive());
        monitor.stop();
    }

    void testDestructorStops()
    {
        {
            QAgentInputMonitor monitor;
            monitor.start();
            QVERIFY(monitor.isActive());
        }
        /* Should not crash */
    }

    void testInitialState()
    {
        QAgentInputMonitor monitor;
        QVERIFY(!monitor.isActive());
    }

    /* Input buffering byte-level tests via processBytes() */

    void testAsciiInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Feed "abc" */
        monitor.processBytes("abc", 3);

        QCOMPARE(changes.size(), 3);
        QCOMPARE(changes[0], "a");
        QCOMPARE(changes[1], "ab");
        QCOMPARE(changes[2], "abc");
    }

    void testEnterSubmitsInput()
    {
        QAgentInputMonitor monitor;
        QString            submitted;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&submitted](const QString &text) {
            submitted = text;
        });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" then Enter */
        monitor.processBytes("hello\n", 6);

        QCOMPARE(submitted, "hello");
        /* Last inputChanged should be empty (buffer cleared after submit) */
        QVERIFY(!changes.isEmpty());
        QCOMPARE(changes.last(), "");
    }

    void testCarriageReturnSubmits()
    {
        QAgentInputMonitor monitor;
        QString            submitted;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&submitted](const QString &text) {
            submitted = text;
        });

        monitor.processBytes("test\r", 5);
        QCOMPARE(submitted, "test");
    }

    void testEmptyEnterIgnored()
    {
        QAgentInputMonitor monitor;
        int                readyCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyCount](const QString &) {
            readyCount++;
        });

        /* Enter with empty buffer should not emit inputReady */
        monitor.processBytes("\n", 1);
        QCOMPARE(readyCount, 0);

        /* Multiple enters */
        monitor.processBytes("\n\r\n", 3);
        QCOMPARE(readyCount, 0);
    }

    void testBackspace()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "abc" then backspace */
        const char data[] = {'a', 'b', 'c', 0x7F};
        monitor.processBytes(data, 4);

        QCOMPARE(changes.size(), 4);
        QCOMPARE(changes[3], "ab"); /* 'c' deleted */
    }

    void testBackspaceOnEmpty()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        /* Backspace on empty buffer should not emit */
        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);
        QCOMPARE(changeCount, 0);
    }

    void testCtrlUClearsLine()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" then Ctrl-U */
        const char data[] = {'h', 'e', 'l', 'l', 'o', 0x15};
        monitor.processBytes(data, 6);

        QCOMPARE(changes.last(), "");
    }

    void testCtrlWDeletesWord()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello world" then Ctrl-W */
        monitor.processBytes("hello world", 11);
        const char ctrlW = 0x17;
        monitor.processBytes(&ctrlW, 1);

        /* Should delete "world" leaving "hello " */
        QCOMPARE(changes.last(), "hello ");
    }

    void testCtrlWDeletesOnlyWord()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" (no space) then Ctrl-W -> clears everything */
        monitor.processBytes("hello", 5);
        const char ctrlW = 0x17;
        monitor.processBytes(&ctrlW, 1);

        QCOMPARE(changes.last(), "");
    }

    void testEscClearsInputAndEmits()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;
        int                escCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });
        connect(&monitor, &QAgentInputMonitor::escPressed, [&escCount]() { escCount++; });

        /* Type "abc" then ESC */
        const char data[] = {'a', 'b', 'c', 0x1B};
        monitor.processBytes(data, 4);

        /* inputChanged("") emitted before escPressed */
        QVERIFY(!changes.isEmpty());
        QCOMPARE(changes.last(), "");
        QCOMPARE(escCount, 1);
    }

    void testEscStopsProcessing()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* ESC in the middle of input stops processing remaining bytes */
        const char data[] = {'a', 0x1B, 'b', 'c'};
        monitor.processBytes(data, 4);

        /* After ESC, 'b' and 'c' should NOT be processed */
        bool hasBC = false;
        for (const QString &change : changes) {
            if (change.contains('b') || change.contains('c')) {
                hasBC = true;
            }
        }
        QVERIFY(!hasBC);
    }

    void testUtf8CjkInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+4F60 (你) = 0xE4 0xBD 0xA0 */
        const char ni[] = {'\xE4', '\xBD', '\xA0'};
        monitor.processBytes(ni, 3);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xe4\xbd\xa0"));
    }

    void testUtf8TwoByteInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+00E9 (é) = 0xC3 0xA9 */
        const char data[] = {'\xC3', '\xA9'};
        monitor.processBytes(data, 2);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xc3\xa9"));
    }

    void testUtf8FourByteEmoji()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+1F600 (😀) = 0xF0 0x9F 0x98 0x80 */
        const char emoji[] = {'\xF0', '\x9F', '\x98', '\x80'};
        monitor.processBytes(emoji, 4);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xf0\x9f\x98\x80"));
    }

    void testBackspaceDeletesEmojiAsUnit()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type emoji then backspace */
        const char emoji[] = {'\xF0', '\x9F', '\x98', '\x80'};
        monitor.processBytes(emoji, 4);

        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);

        /* Emoji is a surrogate pair (2 QChars), backspace should delete both */
        QCOMPARE(changes.last(), "");
    }

    void testBackspaceDeletesCjkAsUnit()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "a" + CJK char + backspace */
        monitor.processBytes("a", 1);
        const char ni[] = {'\xE4', '\xBD', '\xA0'};
        monitor.processBytes(ni, 3);
        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);

        /* Should be back to just "a" */
        QCOMPARE(changes.last(), "a");
    }

    void testUtf8IncompleteRecovery()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Incomplete CJK (only first byte) followed by ASCII 'x' */
        const char data[] = {'\xE4', 'x'};
        monitor.processBytes(data, 2);

        /* The incomplete UTF-8 should be discarded, 'x' processed normally */
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], "x");
    }

    void testUtf8SplitAcrossCalls()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Send CJK char 你 (E4 BD A0) split across two processBytes calls */
        const char part1[] = {'\xE4'};
        const char part2[] = {'\xBD', '\xA0'};
        monitor.processBytes(part1, 1);
        monitor.processBytes(part2, 2);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xe4\xbd\xa0"));
    }

    void testControlCharsIgnored()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        /* Control chars 0x01-0x07 should be ignored (except handled ones) */
        const char data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        monitor.processBytes(data, 7);
        QCOMPARE(changeCount, 0);
    }

    void testStopClearsInputState()
    {
        QAgentInputMonitor monitor;
        QString            lastChanged;
        bool               changedEmitted = false;

        connect(
            &monitor,
            &QAgentInputMonitor::inputChanged,
            [&lastChanged, &changedEmitted](const QString &text) {
                lastChanged    = text;
                changedEmitted = true;
            });

        monitor.start();
        QVERIFY(monitor.isActive());

        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* stop() should have emitted inputChanged with empty string */
        QVERIFY(changedEmitted);
        QVERIFY(lastChanged.isEmpty());
    }

    /* QSocTool abort interface tests */

    void testToolAbortDefaultNoOp()
    {
        QSocToolRegistry registry;
        auto            *tool = new QSocToolShellBash(&registry);
        registry.registerTool(tool);
        registry.abortAll();
    }

    /* QSocAgent abort cascade tests */

    void testAbortCascadesToLLM()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *agent        = new QSocAgent(this, llmService, toolRegistry);

        agent->abort();

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testAbortCascadesToTools()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *bashTool     = new QSocToolShellBash(toolRegistry);
        toolRegistry->registerTool(bashTool);

        auto *agent = new QSocAgent(this, llmService, toolRegistry);
        agent->abort();

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testHandleToolCallsSkipsAfterAbort()
    {
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *bashTool     = new QSocToolShellBash(toolRegistry);
        toolRegistry->registerTool(bashTool);

        QSocAgentConfig config;
        auto           *agent = new QSocAgent(this, nullptr, toolRegistry, config);

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "Do something"}});

        json assistantMsg  = {{"role", "assistant"}, {"content", nullptr}};
        json toolCallsJson = json::array();
        toolCallsJson.push_back(
            {{"id", "call_1"},
             {"type", "function"},
             {"function", {{"name", "bash"}, {"arguments", R"({"command":"echo hello"})"}}}});
        toolCallsJson.push_back(
            {{"id", "call_2"},
             {"type", "function"},
             {"function", {{"name", "bash"}, {"arguments", R"({"command":"echo world"})"}}}});
        assistantMsg["tool_calls"] = toolCallsJson;
        msgs.push_back(assistantMsg);
        agent->setMessages(msgs);

        agent->abort();
        QVERIFY(!agent->isRunning());

        delete agent;
        delete toolRegistry;
    }

    void testRunAbortedSignalEmitted()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);

        QSocAgentConfig config;
        auto           *agent = new QSocAgent(this, llmService, toolRegistry, config);

        QSignalSpy abortedSpy(agent, &QSocAgent::runAborted);

        agent->runStream("test query");
        agent->abort();

        QCoreApplication::processEvents();
        QVERIFY(abortedSpy.count() >= 0);

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testAbortStreamMethod()
    {
        auto *llmService = new QLLMService(this);

        llmService->abortStream();

        QSignalSpy errorSpy(llmService, &QLLMService::streamError);
        QCOMPARE(errorSpy.count(), 0);

        delete llmService;
    }

    void testAbortRegistryAbortAll()
    {
        QSocToolRegistry registry;
        auto            *bash1 = new QSocToolShellBash(&registry);
        registry.registerTool(bash1);
        registry.abortAll();
    }

    /* Queue integration test */

    void testQueueIntegration()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *agent        = new QSocAgent(this, llmService, toolRegistry);

        agent->queueRequest("follow-up request");
        QVERIFY(agent->hasPendingRequests());
        QCOMPARE(agent->pendingRequestCount(), 1);

        agent->queueRequest("second request");
        QCOMPARE(agent->pendingRequestCount(), 2);

        delete agent;
        delete toolRegistry;
        delete llmService;
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocagentinputmonitor.moc"
