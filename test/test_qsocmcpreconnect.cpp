// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"
#include "qsocmcp_fake_transport.h"

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

void driveHandshakeAndList(QsocMcpFakeTransport *transport, const QStringList &toolNames)
{
    /* Initialize is the first sent message; tools/list is the third
     * (initialize, initialized notification, tools/list). */
    replyInitialize(transport, transport->firstSentId());
    QTest::qWait(20);
    const int toolsListId = transport->lastSentId();
    replyToolsList(transport, toolsListId, toolNames);
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

        nlohmann::json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "notifications/tools/list_changed";
        notif["params"]  = nlohmann::json::object();
        transport->simulateMessage(notif);

        QTest::qWait(20);
        const int newListId = transport->lastSentId();
        replyToolsList(transport, newListId, {"shiny"});

        QVERIFY(
            QTest::qWaitFor(
                [&]() {
                    return registry.hasTool("mcp__svr__shiny") && manager.totalToolCount() == 1;
                },
                1000));
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

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpreconnect.moc"
