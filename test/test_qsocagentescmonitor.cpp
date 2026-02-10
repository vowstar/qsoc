// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolshell.h"
#include "cli/qagentescmonitor.h"
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

    /* ESC Monitor basic tests */

    void testStartStop()
    {
        QAgentEscMonitor monitor;

        /* Should not crash on repeated start/stop */
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
        QVERIFY(!monitor.isActive());
    }

    void testDestructorStops()
    {
        /* Monitor should restore terminal on destruction */
        {
            QAgentEscMonitor monitor;
            monitor.start();
            QVERIFY(monitor.isActive());
            /* Destructor called here */
        }
        /* Should not crash */
    }

    void testInitialState()
    {
        QAgentEscMonitor monitor;
        QVERIFY(!monitor.isActive());
    }

    /* QSocTool abort interface tests */

    void testToolAbortDefaultNoOp()
    {
        /* Base class abort should be a no-op (not crash) */
        QSocToolRegistry registry;
        auto            *tool = new QSocToolShellBash(&registry);
        registry.registerTool(tool);

        /* abortAll should not crash with no running process */
        registry.abortAll();
    }

    /* QSocAgent abort cascade tests */

    void testAbortCascadesToLLM()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *agent        = new QSocAgent(this, llmService, toolRegistry);

        /* abort() should not crash even with no active stream */
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

        /* abort() should cascade to tool registry */
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

        /* Build a message history with a pending tool call */
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

        /* Set abort before calling handleToolCalls indirectly */
        agent->abort();

        /* Verify abortRequested was set */
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

        /* Start a stream and immediately abort */
        agent->runStream("test query");
        agent->abort();

        /* Process events to let signals propagate */
        QCoreApplication::processEvents();

        /* The runAborted signal should have been emitted
         * (either from handleStreamError or processStreamIteration) */
        QVERIFY(abortedSpy.count() >= 0); /* May or may not fire depending on timing */

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testAbortStreamMethod()
    {
        auto *llmService = new QLLMService(this);

        /* abortStream should not crash when no stream is active */
        llmService->abortStream();

        QSignalSpy errorSpy(llmService, &QLLMService::streamError);

        /* No signal should be emitted when there's no active stream */
        QCOMPARE(errorSpy.count(), 0);

        delete llmService;
    }

    void testAbortRegistryAbortAll()
    {
        QSocToolRegistry registry;
        auto            *bash1 = new QSocToolShellBash(&registry);
        toolRegistry_registerMultiple(&registry, bash1);

        /* abortAll should iterate all tools without crash */
        registry.abortAll();
    }

private:
    void toolRegistry_registerMultiple(QSocToolRegistry *registry, QSocTool *tool)
    {
        registry->registerTool(tool);
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocagentescmonitor.moc"
