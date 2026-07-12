// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"
#include "qsocmcp_fake_transport.h"

#include <functional>
#include <nlohmann/json.hpp>

#include <QSignalSpy>
#include <QtCore>
#include <QtTest>

namespace {

McpServerConfig makeConfig(const QString &name)
{
    McpServerConfig cfg;
    cfg.name             = name;
    cfg.type             = "stdio";
    cfg.command          = "/bin/true";
    cfg.requestTimeoutMs = 2000;
    cfg.connectTimeoutMs = 2000;
    return cfg;
}

void replyInitialize(QsocMcpFakeTransport *transport, int messageId)
{
    nlohmann::json resp;
    resp["jsonrpc"]                = "2.0";
    resp["id"]                     = messageId;
    resp["result"]["capabilities"] = nlohmann::json::object();
    transport->simulateMessage(resp);
}

void replyToolsList(QsocMcpFakeTransport *transport, int messageId, const QStringList &toolNames)
{
    nlohmann::json resp;
    resp["jsonrpc"]         = "2.0";
    resp["id"]              = messageId;
    nlohmann::json toolsArr = nlohmann::json::array();
    for (const QString &name : toolNames) {
        nlohmann::json tool;
        tool["name"]        = name.toStdString();
        tool["description"] = ("Tool " + name).toStdString();
        tool["inputSchema"] = {{"type", "object"}};
        toolsArr.push_back(tool);
    }
    resp["result"]["tools"] = toolsArr;
    transport->simulateMessage(resp);
}

void replyToolSuccess(QsocMcpFakeTransport *transport, int messageId, const QString &text)
{
    nlohmann::json resp;
    resp["jsonrpc"]           = "2.0";
    resp["id"]                = messageId;
    resp["result"]["content"] = nlohmann::json::array(
        {{{"type", "text"}, {"text", text.toStdString()}}});
    transport->simulateMessage(resp);
}

QList<int> requestIdsForMethod(QsocMcpFakeTransport *transport, const QString &method)
{
    QList<int> ids;
    for (const auto &message : transport->sent()) {
        if (message.value("method", std::string()) == method.toStdString() && message.contains("id")
            && message["id"].is_number_integer()) {
            ids.append(message["id"].get<int>());
        }
    }
    return ids;
}

QList<int> cancelledRequestIds(QsocMcpFakeTransport *transport)
{
    QList<int> ids;
    for (const auto &message : transport->sent()) {
        if (message.value("method", std::string()) != "notifications/cancelled"
            || !message.contains("params")) {
            continue;
        }
        const auto &params = message["params"];
        if (params.contains("requestId") && params["requestId"].is_number_integer()) {
            ids.append(params["requestId"].get<int>());
        }
    }
    return ids;
}

void driveHandshakeAndList(QsocMcpFakeTransport *transport, const QStringList &toolNames)
{
    /* Initialize is the first sent message; tools/list is the third
     * (initialize, initialized notification, tools/list). */
    replyInitialize(transport, transport->firstSentId());
    const int toolsListId = transport->lastSentId();
    replyToolsList(transport, toolsListId, toolNames);
}

bool refreshTools(QsocMcpFakeTransport *transport, const QStringList &toolNames)
{
    const qsizetype previousCount
        = requestIdsForMethod(transport, QStringLiteral("tools/list")).size();

    nlohmann::json notif;
    notif["jsonrpc"] = "2.0";
    notif["method"]  = "notifications/tools/list_changed";
    notif["params"]  = nlohmann::json::object();
    transport->simulateMessage(notif);

    const QList<int> ids = requestIdsForMethod(transport, QStringLiteral("tools/list"));
    if (ids.size() != previousCount + 1) {
        return false;
    }
    replyToolsList(transport, ids.last(), toolNames);
    return true;
}

struct McpServerFixture
{
    McpServerFixture()
    {
        manager = new QSocMcpManager({makeConfig("svr")}, &registry);
        manager->setTransportFactory([this](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager->startAll();
    }

    ~McpServerFixture() { delete manager.data(); }

    bool waitForTransport()
    {
        return QTest::qWaitFor([this]() { return !transports.isEmpty(); }, 500);
    }

    bool startWithTool(const QString &name)
    {
        if (!waitForTransport()) {
            return false;
        }
        driveHandshakeAndList(transport(), {name});
        const QString toolName = QSocMcp::buildToolName(QStringLiteral("svr"), name);
        return QTest::qWaitFor([this, toolName]() { return registry.hasTool(toolName); }, 500);
    }

    QsocMcpFakeTransport *transport() const { return transports.last().data(); }

    QSocToolRegistry                      registry;
    QList<QPointer<QsocMcpFakeTransport>> transports;
    QPointer<QSocMcpManager>              manager;
};

struct ActiveCallResult
{
    QString output;
    bool    fallbackFired = false;
};

ActiveCallResult runActiveCall(
    QSocToolRegistry &registry, const QString &name, QSocTool *tool, std::function<void()> trigger)
{
    QTimer triggerTimer;
    triggerTimer.setSingleShot(true);
    QObject::connect(&triggerTimer, &QTimer::timeout, &triggerTimer, [&trigger]() { trigger(); });
    triggerTimer.start(0);

    QPointer<QSocTool> toolGuard(tool);
    ActiveCallResult   result;
    QTimer             fallback;
    fallback.setSingleShot(true);
    QObject::connect(&fallback, &QTimer::timeout, &fallback, [&]() {
        result.fallbackFired = true;
        if (toolGuard.isNull()) {
            qFatal("Active MCP wrapper was destroyed before execute returned");
        }
        toolGuard->abort();
    });
    fallback.start(500);

    result.output = registry.executeTool(name, json::object());
    triggerTimer.stop();
    fallback.stop();
    return result;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void toolsRegisterAfterListResponse()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));

        driveHandshakeAndList(transports.last().data(), {"echo", "ping"});

        QVERIFY(QTest::qWaitFor([&]() { return registry.count() >= 2; }, 1000));
        QVERIFY(registry.hasTool("mcp__svr__echo"));
        QVERIFY(registry.hasTool("mcp__svr__ping"));
        QCOMPARE(manager.totalToolCount(), qsizetype(2));
    }

    void synchronousListResponseRegistersTools()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        transport->setSendHook([&](const nlohmann::json &message) {
            if (message.value("method", std::string()) != "tools/list" || !message.contains("id")) {
                return;
            }
            replyToolsList(transport, message["id"].get<int>(), {"echo"});
        });
        replyInitialize(transport, transport->firstSentId());

        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__echo")));
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
    }

    void listChangedNotificationRefreshes()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));

        auto *transport = transports.last().data();
        driveHandshakeAndList(transport, {"old"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__old"); }, 500));
        QPointer<QSocTool> oldTool = registry.getTool(QStringLiteral("mcp__svr__old"));

        nlohmann::json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "notifications/tools/list_changed";
        notif["params"]  = nlohmann::json::object();
        transport->simulateMessage(notif);

        const int newListId = transport->lastSentId();
        replyToolsList(transport, newListId, {"shiny"});

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__old")));
        QVERIFY(registry.executeTool(QStringLiteral("mcp__svr__old"), json::object())
                    .contains(QStringLiteral("not found")));

        QVERIFY(
            QTest::qWaitFor(
                [&]() {
                    return registry.hasTool("mcp__svr__shiny") && manager.totalToolCount() == 1;
                },
                1000));
        QCOMPARE(registry.count(), 1);
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__old")));
        QVERIFY(registry.executeTool(QStringLiteral("mcp__svr__old"), json::object())
                    .contains(QStringLiteral("not found")));
    }

    void sameNameRefreshKeepsNewWrapper()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));

        auto *transport = transports.last().data();
        driveHandshakeAndList(transport, {"echo"});
        QVERIFY(
            QTest::qWaitFor(
                [&]() { return registry.hasTool(QStringLiteral("mcp__svr__echo")); }, 500));
        QPointer<QSocTool> oldTool = registry.getTool(QStringLiteral("mcp__svr__echo"));

        nlohmann::json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "notifications/tools/list_changed";
        notif["params"]  = nlohmann::json::object();
        transport->simulateMessage(notif);
        replyToolsList(transport, transport->lastSentId(), {"echo"});

        QSocTool *newTool = registry.getTool(QStringLiteral("mcp__svr__echo"));
        QVERIFY(newTool != nullptr);
        QVERIFY(newTool != oldTool.data());
        QCOMPARE(registry.count(), 1);
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), newTool);
        QCOMPARE(registry.count(), 1);
    }

    void refreshLetsActiveCallFinishOnOldWrapper()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));

        auto             *transport = fixture.transport();
        QSocToolRegistry &registry  = fixture.registry;

        QPointer<QSocTool> oldTool    = registry.getTool(QStringLiteral("mcp__svr__echo"));
        QSocTool          *oldAddress = oldTool.data();
        QPointer<QSocTool> replacement;
        int                callId          = -1;
        bool               refreshComplete = false;

        const ActiveCallResult call
            = runActiveCall(registry, QStringLiteral("mcp__svr__echo"), oldTool.data(), [&]() {
                  const QList<int> callIds
                      = requestIdsForMethod(transport, QStringLiteral("tools/call"));
                  if (callIds.isEmpty()) {
                      return;
                  }
                  callId = callIds.last();

                  if (!refreshTools(transport, {"echo"})) {
                      return;
                  }
                  replacement = registry.getTool(QStringLiteral("mcp__svr__echo"));
                  if (replacement.isNull() || replacement.data() == oldAddress) {
                      return;
                  }
                  refreshComplete = true;
                  replyToolSuccess(transport, callId, QStringLiteral("finished"));
              });

        QVERIFY(!call.fallbackFired);
        QVERIFY(refreshComplete);
        QCOMPARE(call.output, QStringLiteral("finished"));
        QVERIFY(replacement.data() != oldAddress);
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), replacement.data());
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
        QVERIFY(cancelledRequestIds(transport).isEmpty());
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), replacement.data());
    }

    void abortReachesActiveCallAfterRefresh()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));

        auto             *transport = fixture.transport();
        QSocToolRegistry &registry  = fixture.registry;

        QPointer<QSocTool> oldTool    = registry.getTool(QStringLiteral("mcp__svr__echo"));
        QSocTool          *oldAddress = oldTool.data();
        QPointer<QSocTool> replacement;
        int                callId          = -1;
        bool               refreshComplete = false;

        const ActiveCallResult call
            = runActiveCall(registry, QStringLiteral("mcp__svr__echo"), oldTool.data(), [&]() {
                  const QList<int> callIds
                      = requestIdsForMethod(transport, QStringLiteral("tools/call"));
                  if (callIds.isEmpty()) {
                      return;
                  }
                  callId = callIds.last();

                  if (!refreshTools(transport, {"echo"})) {
                      return;
                  }
                  replacement     = registry.getTool(QStringLiteral("mcp__svr__echo"));
                  refreshComplete = !replacement.isNull() && replacement.data() != oldAddress;
                  registry.abortAll();
              });

        QVERIFY(!call.fallbackFired);
        QVERIFY(refreshComplete);
        QCOMPARE(call.output, QStringLiteral("[mcp aborted]"));
        QCOMPARE(cancelledRequestIds(transport), QList<int>{callId});
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), replacement.data());
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
    }

    void managerDestructionLetsActiveCallUnwind()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));

        auto             *transport = fixture.transport();
        QSocToolRegistry &registry  = fixture.registry;

        QPointer<QSocTool>     oldTool        = registry.getTool(QStringLiteral("mcp__svr__echo"));
        int                    callId         = -1;
        bool                   managerDeleted = false;
        const ActiveCallResult call
            = runActiveCall(registry, QStringLiteral("mcp__svr__echo"), oldTool.data(), [&]() {
                  const QList<int> callIds
                      = requestIdsForMethod(transport, QStringLiteral("tools/call"));
                  if (!callIds.isEmpty()) {
                      callId = callIds.last();
                  }
                  delete fixture.manager.data();
                  managerDeleted = true;
              });
        if (!fixture.manager.isNull()) {
            delete fixture.manager.data();
        }

        QVERIFY(!call.fallbackFired);
        QVERIFY(managerDeleted);
        QVERIFY(callId > 0);
        QCOMPARE(call.output, QStringLiteral("[mcp error] server closed during call"));
        QCOMPARE(registry.count(), 0);
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
    }

    void manualReconnectLetsActiveCallUnwind()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));

        auto             *transport = fixture.transport();
        QSocToolRegistry &registry  = fixture.registry;

        QPointer<QSocTool>     oldTool = registry.getTool(QStringLiteral("mcp__svr__echo"));
        int                    callId  = -1;
        bool                   reconnectStarted = false;
        const ActiveCallResult call
            = runActiveCall(registry, QStringLiteral("mcp__svr__echo"), oldTool.data(), [&]() {
                  const QList<int> callIds
                      = requestIdsForMethod(transport, QStringLiteral("tools/call"));
                  if (!callIds.isEmpty()) {
                      callId = callIds.last();
                  }
                  reconnectStarted = fixture.manager->reconnectServer(QStringLiteral("svr"));
              });

        QVERIFY(!call.fallbackFired);
        QVERIFY(reconnectStarted);
        QVERIFY(callId > 0);
        QCOMPARE(call.output, QStringLiteral("[mcp error] server closed during call"));
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
        QVERIFY(fixture.transports.size() >= 2);

        driveHandshakeAndList(fixture.transport(), {"echo"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__echo"); }, 500));
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
    }

    void nestedCallsFinishAfterRefresh()
    {
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));

        auto             *transport = fixture.transport();
        QSocToolRegistry &registry  = fixture.registry;

        QPointer<QSocTool> oldTool    = registry.getTool(QStringLiteral("mcp__svr__echo"));
        QSocTool          *oldAddress = oldTool.data();
        QPointer<QSocTool> replacement;
        QString            innerOutput;
        bool               refreshComplete = false;

        const ActiveCallResult call
            = runActiveCall(registry, QStringLiteral("mcp__svr__echo"), oldTool.data(), [&]() {
                  QTimer refresh;
                  refresh.setSingleShot(true);
                  connect(&refresh, &QTimer::timeout, &refresh, [&]() {
                      const QList<int> callIds
                          = requestIdsForMethod(transport, QStringLiteral("tools/call"));
                      if (callIds.size() != 2) {
                          return;
                      }

                      if (!refreshTools(transport, {"echo"})) {
                          return;
                      }
                      replacement = registry.getTool(QStringLiteral("mcp__svr__echo"));
                      if (replacement.isNull() || replacement.data() == oldAddress) {
                          return;
                      }

                      refreshComplete = true;
                      replyToolSuccess(transport, callIds.first(), QStringLiteral("outer"));
                      replyToolSuccess(transport, callIds.last(), QStringLiteral("inner"));
                  });
                  refresh.start(0);
                  innerOutput
                      = registry.executeTool(QStringLiteral("mcp__svr__echo"), json::object());
                  refresh.stop();
              });

        QVERIFY(!call.fallbackFired);
        QVERIFY(refreshComplete);
        QCOMPARE(call.output, QStringLiteral("outer"));
        QCOMPARE(innerOutput, QStringLiteral("inner"));
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), replacement.data());
        QVERIFY(cancelledRequestIds(transport).isEmpty());
        QVERIFY(QTest::qWaitFor([&]() { return oldTool.isNull(); }, 500));
    }

    void reconnectScheduledAfterClose()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setReconnectDelays(20, 100);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.last().data(), {"a"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__a"); }, 500));

        const auto firstTransportPtr = transports.last();
        firstTransportPtr.data()->simulateClosed();

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__a")));

        QVERIFY(QTest::qWaitFor([&]() { return scheduleSpy.size() >= 1; }, 500));
        const auto args = scheduleSpy.first();
        QCOMPARE(args.at(0).toString(), QStringLiteral("svr"));
        QCOMPARE(args.at(1).toInt(), 1);
        QCOMPARE(manager.reconnectAttempts("svr"), 1);
        QVERIFY(!manager.hasGivenUp("svr"));

        /* Wait for the rebuild to actually run, producing a fresh
         * transport. */
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() >= 2; }, 500));
        QVERIFY(transports.last().data() != firstTransportPtr.data());
    }

    void manualReconnectRemovesToolsImmediately()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.last().data(), {"old"});
        QVERIFY(
            QTest::qWaitFor([&]() { return registry.hasTool(QStringLiteral("mcp__svr__old")); }, 500));

        QVERIFY(manager.reconnectServer(QStringLiteral("svr")));

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__old")));
        QCOMPARE(registry.count(), 0);
        QVERIFY(transports.size() >= 2);
        driveHandshakeAndList(transports.last().data(), {"new"});
        QVERIFY(
            QTest::qWaitFor([&]() { return registry.hasTool(QStringLiteral("mcp__svr__new")); }, 500));
        QCOMPARE(registry.count(), 1);
    }

    void managerDestructionClearsRegistry()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        auto *manager = new QSocMcpManager(cfgs, &registry);
        manager->setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager->startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.last().data(), {"old"});
        QVERIFY(
            QTest::qWaitFor([&]() { return registry.hasTool(QStringLiteral("mcp__svr__old")); }, 500));

        delete manager;

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__old")));
        QCOMPARE(registry.count(), 0);
    }

    void reconnectClearsAttemptsAfterReady()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setReconnectDelays(20, 100);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });

        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.last().data(), {"a"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__a"); }, 500));

        transports.last().data()->simulateClosed();
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() >= 2; }, 500));
        driveHandshakeAndList(transports.last().data(), {"a"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__a"); }, 500));
        QCOMPARE(manager.reconnectAttempts("svr"), 0);
    }

    void givesUpAfterMaxAttempts()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setReconnectDelays(10, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy gaveUpSpy(&manager, &QSocMcpManager::serverGaveUp);

        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));

        /* Each transport closes immediately on its first run instead of
         * acknowledging initialize. The manager will keep rebuilding
         * until it hits the cap. */
        const int cap          = QSocMcpManager::kMaxReconnectAttempts;
        int       processedRun = 0;
        while (gaveUpSpy.isEmpty() && processedRun < cap + 2) {
            QVERIFY(QTest::qWaitFor([&]() { return transports.size() > processedRun; }, 1000));
            auto *transport = transports.at(processedRun).data();
            QVERIFY(transport != nullptr);
            transport->simulateClosed();
            processedRun++;
        }

        QVERIFY(QTest::qWaitFor([&]() { return !gaveUpSpy.isEmpty(); }, 1000));
        QCOMPARE(gaveUpSpy.first().at(0).toString(), QStringLiteral("svr"));
        QVERIFY(manager.hasGivenUp("svr"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpreconnect.moc"
