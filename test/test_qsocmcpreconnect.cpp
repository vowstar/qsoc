// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"
#include "qsocmcp_fake_transport.h"

#include <algorithm>
#include <functional>
#include <nlohmann/json.hpp>
#include <utility>

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

QList<QTimer *> activeReconnectTimers(const QSocMcpManager &manager)
{
    QList<QTimer *> active;
    const auto      timers = manager.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
    for (QTimer *timer : timers) {
        if (timer->isActive()) {
            active << timer;
        }
    }
    return active;
}

void replyInitialize(QsocMcpFakeTransport *transport, int messageId)
{
    nlohmann::json resp;
    resp["jsonrpc"]                = "2.0";
    resp["id"]                     = messageId;
    resp["result"]["capabilities"] = nlohmann::json::object();
    transport->simulateMessage(resp);
}

nlohmann::json toolsListResult(const QStringList &toolNames)
{
    nlohmann::json toolsArr = nlohmann::json::array();
    for (const QString &name : toolNames) {
        nlohmann::json tool;
        tool["name"]        = name.toStdString();
        tool["description"] = ("Tool " + name).toStdString();
        tool["inputSchema"] = {{"type", "object"}};
        toolsArr.push_back(tool);
    }
    return {{"tools", std::move(toolsArr)}};
}

void replyToolsList(QsocMcpFakeTransport *transport, int messageId, const QStringList &toolNames)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = messageId;
    resp["result"]  = toolsListResult(toolNames);
    transport->simulateMessage(resp);
}

void replyToolDescriptors(QsocMcpFakeTransport *transport, int messageId, const nlohmann::json &tools)
{
    nlohmann::json resp;
    resp["jsonrpc"]         = "2.0";
    resp["id"]              = messageId;
    resp["result"]["tools"] = tools;
    transport->simulateMessage(resp);
}

void replyRequestError(QsocMcpFakeTransport *transport, int messageId)
{
    nlohmann::json resp;
    resp["jsonrpc"]          = "2.0";
    resp["id"]               = messageId;
    resp["error"]["code"]    = -32603;
    resp["error"]["message"] = "request failed";
    transport->simulateMessage(resp);
}

void replyInvalidToolsList(QsocMcpFakeTransport *transport, int messageId)
{
    nlohmann::json resp;
    resp["jsonrpc"]           = "2.0";
    resp["id"]                = messageId;
    resp["result"]["invalid"] = true;
    transport->simulateMessage(resp);
}

void notifyToolsChanged(QsocMcpFakeTransport *transport)
{
    nlohmann::json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"]  = "notifications/tools/list_changed";
    notification["params"]  = nlohmann::json::object();
    transport->simulateMessage(notification);
}

void flushQueuedCalls()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
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
    flushQueuedCalls();
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
    flushQueuedCalls();
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

class StaticTool : public QSocTool
{
public:
    explicit StaticTool(QString name)
        : name_(std::move(name))
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("local tool"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return QStringLiteral("local result"); }

private:
    QString name_;
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void toolsRegisterAfterListResponse()
    {
        QSocTestCapture                       capture;
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
        QVERIFY(!capture.text().contains(QStringLiteral("MCP tool catalog")));
    }

    void invalidToolNamesAreSkipped()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int  listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        QSignalSpy toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);

        nlohmann::json tools = nlohmann::json::array(
            {{{"name", "before"}, {"inputSchema", {{"type", "object"}}}},
             {{"description", "missing name"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", ""}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "..."}, {"inputSchema", {{"type", "object"}}}},
             {{"name", 7}, {"inputSchema", {{"type", "object"}}}},
             {{"name", nullptr}, {"inputSchema", {{"type", "object"}}}},
             {{"name", true}, {"inputSchema", {{"type", "object"}}}},
             {{"name", nlohmann::json::array()}, {"inputSchema", {{"type", "object"}}}},
             {{"name", nlohmann::json::object()}, {"inputSchema", {{"type", "object"}}}},
             nullptr,
             12,
             nlohmann::json::array(),
             {{"name", "after"}, {"inputSchema", {{"type", "object"}}}}});

        bool threw = false;
        try {
            replyToolDescriptors(transport, listId, tools);
        } catch (const nlohmann::json::exception &) {
            threw = true;
        }
        QVERIFY(!threw);
        flushQueuedCalls();

        auto *client = fixture.manager->findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);
        QCOMPARE(fixture.registry.count(), 2);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(2));
        QCOMPARE(fixture.manager->toolsForClient(QStringLiteral("svr")).size(), qsizetype(2));
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(toolsSpy.first().at(1).value<qsizetype>(), qsizetype(2));
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__before")));
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__after")));
        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__")));
        QCOMPARE(fixture.manager->reconnectAttempts(QStringLiteral("svr")), 0);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 11, sanitized fields 0")));
    }

    void optionalDescriptorFieldsAreSanitized()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();

        nlohmann::json tools = nlohmann::json::array(
            {{{"name", "bad_description"},
              {"description", 7},
              {"inputSchema", nlohmann::json::array()},
              {"annotations", "invalid"}},
             {{"name", "bad_hints"},
              {"inputSchema", {{"type", "object"}}},
              {"annotations", {{"readOnlyHint", "yes"}, {"destructiveHint", 0}}}},
             {{"name", "bad_root"}, {"inputSchema", {{"type", "array"}}}},
             {{"name", "bad_properties"},
              {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::array()}}}},
             {{"name", "bad_required"},
              {"inputSchema", {{"type", "object"}, {"required", nlohmann::json::array({1})}}}},
             {{"name", "missing_schema"}},
             {{"name", "valid"},
              {"description", "valid description"},
              {"inputSchema",
               {{"type", "object"},
                {"properties", {{"value", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"value"})}}},
              {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}}}},
             {{"name", "missing_type"},
              {"inputSchema", {{"properties", {{"value", {{"type", "integer"}}}}}}}},
             {{"name", "bad_type_kind"}, {"inputSchema", {{"type", 7}}}}});

        bool threw = false;
        try {
            replyToolDescriptors(transport, listId, tools);
        } catch (const nlohmann::json::exception &) {
            threw = true;
        }
        QVERIFY(!threw);
        flushQueuedCalls();

        QCOMPARE(fixture.registry.count(), 9);
        const auto emptySchema = nlohmann::json{{"type", "object"}};
        for (const QString &name :
             {QStringLiteral("bad_description"),
              QStringLiteral("bad_root"),
              QStringLiteral("bad_properties"),
              QStringLiteral("bad_required"),
              QStringLiteral("missing_schema"),
              QStringLiteral("bad_type_kind")}) {
            auto *tool = qobject_cast<QSocMcpTool *>(
                fixture.registry.getTool(QSocMcp::buildToolName(QStringLiteral("svr"), name)));
            QVERIFY(tool != nullptr);
            QVERIFY(tool->getParametersSchema() == emptySchema);
        }

        auto *badDescription = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__bad_description")));
        auto *badHints = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__bad_hints")));
        auto *valid = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__valid")));
        auto *missingSchema = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__missing_schema")));
        auto *missingType = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__missing_type")));
        QVERIFY(badDescription != nullptr);
        QVERIFY(badHints != nullptr);
        QVERIFY(valid != nullptr);
        QVERIFY(missingSchema != nullptr);
        QVERIFY(missingType != nullptr);
        QVERIFY(badDescription->getDescription().isEmpty());
        QVERIFY(!badHints->descriptor().readOnly);
        QVERIFY(badHints->descriptor().destructive);
        QVERIFY(valid->descriptor().readOnly);
        QVERIFY(!valid->descriptor().destructive);
        QVERIFY(!valid->isReadOnly());
        QVERIFY(valid->getParametersSchema() == tools.at(6).at("inputSchema"));
        QVERIFY(!missingSchema->descriptor().readOnly);
        QVERIFY(missingSchema->descriptor().destructive);
        const auto missingTypeSchema = missingType->getParametersSchema();
        QCOMPARE(
            QString::fromStdString(missingTypeSchema.at("type").get<std::string>()),
            QStringLiteral("object"));
        QVERIFY(missingTypeSchema.at("properties").contains("value"));
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 0, sanitized fields 11")));
    }

    void duplicateToolNamesKeepFirst()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int  listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        QSignalSpy toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);

        const nlohmann::json tools = nlohmann::json::array(
            {{{"name", "same"}, {"description", "first"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "same"}, {"description", "second"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "safe"}, {"inputSchema", {{"type", "object"}}}}});
        replyToolDescriptors(transport, listId, tools);
        flushQueuedCalls();

        auto *same = qobject_cast<QSocMcpTool *>(
            fixture.registry.getTool(QStringLiteral("mcp__svr__same")));
        QVERIFY(same != nullptr);
        QCOMPARE(same->getDescription(), QStringLiteral("first"));
        QCOMPARE(fixture.registry.count(), 2);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(2));
        QCOMPARE(fixture.manager->toolsForClient(QStringLiteral("svr")).size(), qsizetype(2));
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(toolsSpy.first().at(1).value<qsizetype>(), qsizetype(2));
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 1, sanitized fields 0")));
    }

    void normalizedToolCollisionsAreOmitted_data()
    {
        QTest::addColumn<bool>("reverse");

        QTest::newRow("forward") << false;
        QTest::newRow("reverse") << true;
    }

    void normalizedToolCollisionsAreOmitted()
    {
        QFETCH(bool, reverse);

        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int   listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        QStringList names{QStringLiteral("read file"), QStringLiteral("read.file")};
        if (reverse) {
            std::reverse(names.begin(), names.end());
        }

        nlohmann::json tools = nlohmann::json::array();
        for (const QString &name : names) {
            tools.push_back({{"name", name.toStdString()}, {"inputSchema", {{"type", "object"}}}});
        }
        tools.push_back({{"name", "safe"}, {"inputSchema", {{"type", "object"}}}});
        QSignalSpy toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);
        replyToolDescriptors(transport, listId, tools);
        flushQueuedCalls();

        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__read_file")));
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__safe")));
        QCOMPARE(fixture.registry.count(), 1);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
        QCOMPARE(fixture.manager->toolsForClient(QStringLiteral("svr")).size(), qsizetype(1));
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(toolsSpy.first().at(1).value<qsizetype>(), qsizetype(1));
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 2, sanitized fields 0")));
    }

    void unusableCatalogDoesNotReplaceCurrentTools()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("old")));
        capture.clear();

        auto                *transport = fixture.transport();
        QPointer<QSocTool>   oldTool   = fixture.registry.getTool(QStringLiteral("mcp__svr__old"));
        QSignalSpy           toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);
        const nlohmann::json unusable = nlohmann::json::array(
            {{{"name", "..."}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "a b"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "a.b"}, {"inputSchema", {{"type", "object"}}}}});

        for (int i = 0; i < 64; ++i) {
            notifyToolsChanged(transport);
            const int listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
            replyToolDescriptors(transport, listId, unusable);
            flushQueuedCalls();
        }

        QCOMPARE(fixture.registry.getTool(QStringLiteral("mcp__svr__old")), oldTool.data());
        QCOMPARE(fixture.registry.count(), 1);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
        QCOMPARE(toolsSpy.size(), 0);
        QCOMPARE(fixture.manager->reconnectAttempts(QStringLiteral("svr")), 0);
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(
            QStringLiteral("ignored entries 3, sanitized fields 0; catalog unchanged")));

        notifyToolsChanged(transport);
        int listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        replyToolDescriptors(transport, listId, nlohmann::json::array());
        flushQueuedCalls();
        QCOMPARE(fixture.registry.count(), 0);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(0));
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(toolsSpy.first().at(1).value<qsizetype>(), qsizetype(0));

        notifyToolsChanged(transport);
        listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        replyToolsList(transport, listId, {QStringLiteral("new")});
        flushQueuedCalls();
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__new")));
        QCOMPARE(fixture.registry.count(), 1);
        QCOMPARE(toolsSpy.size(), 2);
    }

    void unusableInitialCatalogStaysReady()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int  listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        QSignalSpy toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);
        const nlohmann::json unusable = nlohmann::json::array(
            {{{"name", "..."}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "a b"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "a.b"}, {"inputSchema", {{"type", "object"}}}}});
        replyToolDescriptors(transport, listId, unusable);
        flushQueuedCalls();

        auto *client = fixture.manager->findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);
        QCOMPARE(fixture.registry.count(), 0);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(0));
        QCOMPARE(fixture.manager->reconnectAttempts(QStringLiteral("svr")), 0);
        QCOMPARE(fixture.transports.size(), qsizetype(1));
        QCOMPARE(toolsSpy.size(), 0);
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(
            QStringLiteral("ignored entries 3, sanitized fields 0; catalog unchanged")));
    }

    void normalizedServerNamespaceKeepsFirstConfig_data()
    {
        QTest::addColumn<QString>("first");
        QTest::addColumn<QString>("second");

        QTest::newRow("space-first") << QStringLiteral("a b") << QStringLiteral("a.b");
        QTest::newRow("dot-first") << QStringLiteral("a.b") << QStringLiteral("a b");
    }

    void normalizedServerNamespaceKeepsFirstConfig()
    {
        QFETCH(QString, first);
        QFETCH(QString, second);

        QSocTestCapture  capture;
        QSocToolRegistry registry;
        QSocMcpManager   manager(
            {makeConfig(first), makeConfig(second), makeConfig(QStringLiteral("..."))}, &registry);

        QCOMPARE(manager.serverNames(), QStringList{first});
        QCOMPARE(manager.clientCount(), qsizetype(1));
        QCOMPARE(capture.text().count(QStringLiteral("MCP server namespace")), 2);
        QCOMPARE(capture.text().count(QStringLiteral("namespace collision")), 1);
        QCOMPARE(capture.text().count(QStringLiteral("namespace is empty")), 1);
    }

    void foreignRegistryToolIsNotShadowed()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        StaticTool       local(QStringLiteral("mcp__svr__echo"));
        fixture.registry.registerTool(&local);
        QVERIFY(fixture.waitForTransport());

        auto *transport = fixture.transport();
        replyInitialize(transport, transport->firstSentId());
        const int listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        replyToolsList(transport, listId, {QStringLiteral("echo"), QStringLiteral("safe")});
        flushQueuedCalls();

        QCOMPARE(fixture.registry.getTool(QStringLiteral("mcp__svr__echo")), &local);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__safe")));
        QCOMPARE(fixture.registry.count(), 2);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
        QCOMPARE(fixture.manager->toolsForClient(QStringLiteral("svr")).size(), qsizetype(1));
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 1, sanitized fields 0")));
    }

    void foreignConflictClearsHiddenRemoteWrapper()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("echo")));
        QPointer<QSocTool> remote = fixture.registry.getTool(QStringLiteral("mcp__svr__echo"));
        StaticTool         local(QStringLiteral("mcp__svr__echo"));
        fixture.registry.registerTool(&local);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(1));
        QCOMPARE(fixture.registry.getTool(QStringLiteral("mcp__svr__echo")), &local);
        capture.clear();

        auto *transport = fixture.transport();
        notifyToolsChanged(transport);
        const int  listId = requestIdsForMethod(transport, QStringLiteral("tools/list")).last();
        QSignalSpy toolsSpy(fixture.manager, &QSocMcpManager::toolsRegistered);
        replyToolsList(transport, listId, {QStringLiteral("echo")});
        flushQueuedCalls();

        QCOMPARE(fixture.registry.getTool(QStringLiteral("mcp__svr__echo")), &local);
        QCOMPARE(fixture.registry.count(), 1);
        QCOMPARE(fixture.manager->totalToolCount(), qsizetype(0));
        QCOMPARE(fixture.manager->toolsForClient(QStringLiteral("svr")).size(), qsizetype(0));
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(toolsSpy.first().at(1).value<qsizetype>(), qsizetype(0));
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 1, sanitized fields 0")));
        QVERIFY(QTest::qWaitFor([&]() { return remote.isNull(); }, 500));
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

    void coalescesToolListChanges_data()
    {
        QTest::addColumn<bool>("inlineCompletion");
        QTest::addColumn<bool>("firstFails");

        QTest::newRow("async-success") << false << false;
        QTest::newRow("async-failure") << false << true;
        QTest::newRow("inline-success") << true << false;
        QTest::newRow("inline-failure") << true << true;
    }

    void coalescesToolListChanges()
    {
        QFETCH(bool, inlineCompletion);
        QFETCH(bool, firstFails);

        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("base")));

        auto *transport = fixture.transport();
        auto *manager   = fixture.manager.data();
        QVERIFY(transport != nullptr);
        QVERIFY(manager != nullptr);
        QSignalSpy toolsSpy(manager, &QSocMcpManager::toolsRegistered);

        int firstRefreshId = -1;
        int hookDepth      = 0;
        int maxHookDepth   = 0;
        transport->setSendHook([&](const nlohmann::json &message) {
            if (message.value("method", std::string()) != "tools/list" || !message.contains("id")) {
                return;
            }
            hookDepth++;
            maxHookDepth = std::max(maxHookDepth, hookDepth);
            if (firstRefreshId < 0) {
                firstRefreshId = message["id"].get<int>();
                if (inlineCompletion) {
                    for (int i = 0; i < 64; ++i) {
                        notifyToolsChanged(transport);
                    }
                    if (firstFails) {
                        replyRequestError(transport, firstRefreshId);
                    } else {
                        replyToolsList(transport, firstRefreshId, {"intermediate"});
                    }
                }
            }
            hookDepth--;
        });

        notifyToolsChanged(transport);
        QVERIFY(firstRefreshId > 0);
        if (!inlineCompletion) {
            for (int i = 0; i < 64; ++i) {
                notifyToolsChanged(transport);
            }
            if (firstFails) {
                replyRequestError(transport, firstRefreshId);
            } else {
                replyToolsList(transport, firstRefreshId, {"intermediate"});
            }
        }

        QList<int> listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 2);
        QCOMPARE(maxHookDepth, 1);
        QCOMPARE(toolsSpy.size(), 0);
        QCOMPARE(fixture.registry.count(), 1);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__base")));
        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__intermediate")));
        flushQueuedCalls();
        listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 3);
        const int trailingId = listIds.last();
        QVERIFY(trailingId != firstRefreshId);

        auto *client = manager->findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        client->responseReceived(firstRefreshId, toolsListResult({"stale"}));
        QCOMPARE(fixture.registry.count(), 1);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__base")));

        replyToolsList(transport, trailingId, {"final"});
        QCOMPARE(toolsSpy.size(), 1);
        QCOMPARE(fixture.registry.count(), 1);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__final")));
        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__base")));
        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__stale")));

        flushQueuedCalls();
        QCOMPARE(requestIdsForMethod(transport, QStringLiteral("tools/list")).size(), 3);
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool refresh failed")), 0);
    }

    void notificationsDuringToolsRegisteredAreCoalesced()
    {
        QSocTestCapture  capture;
        McpServerFixture fixture;
        QVERIFY(fixture.startWithTool(QStringLiteral("base")));

        auto *transport = fixture.transport();
        auto *manager   = fixture.manager.data();
        QVERIFY(transport != nullptr);
        QVERIFY(manager != nullptr);

        bool callbackSeen       = false;
        int  requestsInCallback = -1;
        connect(manager, &QSocMcpManager::toolsRegistered, manager, [&]() {
            if (callbackSeen) {
                return;
            }
            callbackSeen = true;
            const qsizetype before
                = requestIdsForMethod(transport, QStringLiteral("tools/list")).size();
            for (int i = 0; i < 64; ++i) {
                notifyToolsChanged(transport);
            }
            requestsInCallback = requestIdsForMethod(transport, QStringLiteral("tools/list")).size()
                                 - before;
        });

        notifyToolsChanged(transport);
        QList<int> listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 2);
        replyToolDescriptors(
            transport,
            listIds.last(),
            nlohmann::json::array(
                {{{"name", "intermediate"},
                  {"description", 7},
                  {"inputSchema", {{"type", "object"}}}}}));

        QVERIFY(callbackSeen);
        QCOMPARE(requestsInCallback, 0);
        QCOMPARE(requestIdsForMethod(transport, QStringLiteral("tools/list")).size(), 2);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__intermediate")));

        flushQueuedCalls();
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QVERIFY(capture.text().contains(QStringLiteral("ignored entries 0, sanitized fields 1")));
        listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 3);
        replyToolsList(transport, listIds.last(), {"final"});
        flushQueuedCalls();

        QCOMPARE(capture.text().count(QStringLiteral("MCP tool catalog")), 1);
        QCOMPARE(requestIdsForMethod(transport, QStringLiteral("tools/list")).size(), 3);
        QCOMPARE(fixture.registry.count(), 1);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__final")));
    }

    void queuedRefreshDiesWithClient_data()
    {
        QTest::addColumn<bool>("manualReconnect");

        QTest::newRow("closed-client") << false;
        QTest::newRow("manual-reconnect") << true;
    }

    void queuedRefreshDiesWithClient()
    {
        QFETCH(bool, manualReconnect);

        McpServerFixture fixture;
        fixture.manager->setReconnectDelays(1, 1);
        QVERIFY(fixture.startWithTool(QStringLiteral("base")));

        QPointer<QsocMcpFakeTransport> oldTransport = fixture.transport();
        QVERIFY(!oldTransport.isNull());
        notifyToolsChanged(oldTransport.data());
        const QList<int> listIds
            = requestIdsForMethod(oldTransport.data(), QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 2);
        notifyToolsChanged(oldTransport.data());
        QCOMPARE(requestIdsForMethod(oldTransport.data(), QStringLiteral("tools/list")).size(), 2);
        replyToolsList(oldTransport.data(), listIds.last(), {"obsolete"});

        if (manualReconnect) {
            QVERIFY(fixture.manager->reconnectServer(QStringLiteral("svr")));
        } else {
            oldTransport->simulateClosed();
        }
        flushQueuedCalls();
        QVERIFY(QTest::qWaitFor([&]() { return fixture.transports.size() == 2; }, 500));

        auto *replacement = fixture.transport();
        QVERIFY(replacement != nullptr);
        replyInitialize(replacement, replacement->firstSentId());
        QList<int> replacementLists = requestIdsForMethod(replacement, QStringLiteral("tools/list"));
        QCOMPARE(replacementLists.size(), 1);
        replyToolsList(replacement, replacementLists.first(), {"final"});
        flushQueuedCalls();

        replacementLists = requestIdsForMethod(replacement, QStringLiteral("tools/list"));
        QCOMPARE(replacementLists.size(), 1);
        QCOMPARE(fixture.registry.count(), 1);
        QVERIFY(fixture.registry.hasTool(QStringLiteral("mcp__svr__final")));
        QVERIFY(!fixture.registry.hasTool(QStringLiteral("mcp__svr__obsolete")));
    }

    void toolsRegisteredReentryIsLifetimeSafe_data()
    {
        QTest::addColumn<bool>("deleteManager");

        QTest::newRow("delete-manager") << true;
        QTest::newRow("manual-reconnect") << false;
    }

    void toolsRegisteredReentryIsLifetimeSafe()
    {
        QFETCH(bool, deleteManager);

        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QPointer<QSocMcpManager> manager = new QSocMcpManager({makeConfig("svr")}, &registry);
        manager->setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager->startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.first().data(), {"base"});

        QObject observer;
        bool    callbackSeen       = false;
        int     requestsInCallback = -1;
        connect(manager.data(), &QSocMcpManager::toolsRegistered, &observer, [&]() {
            if (callbackSeen) {
                return;
            }
            callbackSeen              = true;
            auto           *transport = transports.first().data();
            const qsizetype before
                = requestIdsForMethod(transport, QStringLiteral("tools/list")).size();
            for (int i = 0; i < 64; ++i) {
                notifyToolsChanged(transport);
            }
            requestsInCallback = requestIdsForMethod(transport, QStringLiteral("tools/list")).size()
                                 - before;
            if (deleteManager) {
                delete manager.data();
            } else {
                manager->reconnectServer(QStringLiteral("svr"));
            }
        });

        auto *oldTransport = transports.first().data();
        notifyToolsChanged(oldTransport);
        const QList<int> listIds = requestIdsForMethod(oldTransport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 2);
        replyToolsList(oldTransport, listIds.last(), {"intermediate"});

        QVERIFY(callbackSeen);
        QCOMPARE(requestsInCallback, 0);
        flushQueuedCalls();
        if (deleteManager) {
            QVERIFY(manager.isNull());
            QCOMPARE(registry.count(), 0);
            return;
        }

        QVERIFY(!manager.isNull());
        QCOMPARE(transports.size(), 2);
        auto *replacement = transports.last().data();
        QVERIFY(replacement != nullptr);
        replyInitialize(replacement, replacement->firstSentId());
        const QList<int> replacementLists
            = requestIdsForMethod(replacement, QStringLiteral("tools/list"));
        QCOMPARE(replacementLists.size(), 1);
        replyToolsList(replacement, replacementLists.first(), {"final"});
        flushQueuedCalls();

        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.hasTool(QStringLiteral("mcp__svr__final")));
        delete manager.data();
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

    void duplicateCloseSchedulesOneReconnect()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        QSocMcpClient *client = manager.findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);

        transports.last()->simulateClosed();
        QVERIFY(QMetaObject::invokeMethod(client, "closed", Qt::DirectConnection));

        QCOMPARE(scheduleSpy.size(), 1);
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 1);
        QCOMPARE(activeReconnectTimers(manager).size(), 1);
    }

    void manualReconnectCancelsEveryPendingTimer()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        QSocMcpClient *client = manager.findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);

        transports.last()->simulateClosed();
        QVERIFY(QMetaObject::invokeMethod(client, "closed", Qt::DirectConnection));
        const QList<QTimer *> timers = activeReconnectTimers(manager);
        QCOMPARE(timers.size(), 1);
        QTimer *pendingTimer = timers.first();
        QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        QCOMPARE(transports.size(), 2);
        QCOMPARE(activeReconnectTimers(manager).size(), 0);

        QPointer<QSocMcpClient> replacement = manager.findClient(QStringLiteral("svr"));
        QVERIFY(QMetaObject::invokeMethod(pendingTimer, "timeout", Qt::DirectConnection));
        QCOMPARE(manager.findClient(QStringLiteral("svr")), replacement.data());
        QCOMPARE(transports.size(), 2);
    }

    void factoryReplacementCancelsPendingReconnect()
    {
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs, &registry);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));

        transports.last()->simulateClosed();
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 1);
        QCOMPARE(activeReconnectTimers(manager).size(), 1);

        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QCOMPARE(activeReconnectTimers(manager).size(), 0);
        manager.startAll();
        QCOMPARE(transports.size(), 2);

        QPointer<QSocMcpClient> replacement = manager.findClient(QStringLiteral("svr"));
        QVERIFY(!replacement.isNull());
        driveHandshakeAndList(transports.last().data(), {"new"});
        QVERIFY(QTest::qWaitFor([&]() { return registry.hasTool("mcp__svr__new"); }, 500));

        QCOMPARE(transports.size(), 2);
        QCOMPARE(manager.findClient(QStringLiteral("svr")), replacement.data());
        QVERIFY(registry.hasTool(QStringLiteral("mcp__svr__new")));
    }

    void factoryReplacementClearsFailureState()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory(
            [](const McpServerConfig &) { return static_cast<QSocMcpTransport *>(nullptr); });
        QVERIFY(manager.hasGivenUp(QStringLiteral("svr")));
        QCOMPARE(manager.clientCount(), qsizetype(0));

        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QVERIFY(!manager.hasGivenUp(QStringLiteral("svr")));
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 0);

        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);
        manager.startAll();
        QCOMPARE(transports.size(), 1);
        transports.last()->simulateClosed();
        QCOMPARE(scheduleSpy.size(), 1);
        QCOMPARE(activeReconnectTimers(manager).size(), 1);
    }

    void factoryReplacementStopsOldTransportBeforeReplacement()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};
        QPointer<QsocMcpFakeTransport>        oldTransport;
        bool                                  stoppedBeforeReplacement = false;

        QSocMcpManager manager(cfgs);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QCOMPARE(transports.size(), 1);
        oldTransport = transports.last();

        manager.setTransportFactory([&](const McpServerConfig &) {
            stoppedBeforeReplacement = !oldTransport.isNull() && oldTransport->stopCount() == 1;
            auto *transport          = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });

        QVERIFY(stoppedBeforeReplacement);
        QCOMPARE(oldTransport->stopCount(), 1);
        QCOMPARE(transports.size(), 2);
        QCOMPARE(transports.last()->startCount(), 0);

        manager.startAll();
        QCOMPARE(transports.last()->startCount(), 1);
    }

    void startAllCancelsPendingReconnect()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QCOMPARE(transports.size(), 1);

        QPointer<QSocMcpClient> client = manager.findClient(QStringLiteral("svr"));
        transports.last()->simulateClosed();
        QCOMPARE(activeReconnectTimers(manager).size(), 1);

        manager.startAll();
        QCOMPARE(manager.findClient(QStringLiteral("svr")), client.data());
        QCOMPARE(client->state(), QSocMcpClient::State::Initializing);
        QCOMPARE(activeReconnectTimers(manager).size(), 0);
    }

    void staleReconnectDoesNotReplaceRestartedClient()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(5000, 5000);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QCOMPARE(transports.size(), 1);

        QPointer<QSocMcpClient> client = manager.findClient(QStringLiteral("svr"));
        transports.last()->simulateClosed();
        const QList<QTimer *> timers = activeReconnectTimers(manager);
        QCOMPARE(timers.size(), 1);

        client->start();
        QCOMPARE(client->state(), QSocMcpClient::State::Initializing);
        timers.first()->stop();
        QVERIFY(QMetaObject::invokeMethod(timers.first(), "timeout", Qt::DirectConnection));

        QCOMPARE(manager.findClient(QStringLiteral("svr")), client.data());
        QCOMPARE(transports.size(), 1);
    }

    void manualReconnectStopsOldTransportBeforeReplacement()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};
        QPointer<QsocMcpFakeTransport>        oldTransport;
        bool                                  stoppedBeforeReplacement = false;

        QSocMcpManager manager(cfgs);
        manager.setTransportFactory([&](const McpServerConfig &) {
            if (!oldTransport.isNull()) {
                stoppedBeforeReplacement = oldTransport->stopCount() == 1;
            }
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QCOMPARE(transports.size(), 1);
        oldTransport = transports.last();

        QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        QVERIFY(stoppedBeforeReplacement);
        QVERIFY(!oldTransport.isNull());
        QCOMPARE(oldTransport->stopCount(), 1);
        QCOMPARE(transports.size(), 2);
        QCOMPARE(transports.last()->startCount(), 1);
    }

    void reentrantReconnectDuringStopKeepsLatestClient()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager.startAll();
        QCOMPARE(transports.size(), 1);

        QSocMcpClient *client    = manager.findClient(QStringLiteral("svr"));
        bool           reentered = false;
        connect(client, &QSocMcpClient::stateChanged, client, [&](QSocMcpClient::State state) {
            if (state != QSocMcpClient::State::Disconnected || reentered) {
                return;
            }
            reentered = true;
            QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        });

        QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        QVERIFY(reentered);
        QCOMPARE(transports.size(), 2);
        QCOMPARE(
            manager.findClient(QStringLiteral("svr"))->state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transports.first()->stopCount(), 1);
        QCOMPARE(transports.last()->startCount(), 1);
    }

    void deletionDuringStopEndsReplacementSafely()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QPointer<QSocMcpManager> manager = new QSocMcpManager(cfgs);
        manager->setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        manager->startAll();
        QCOMPARE(transports.size(), 1);

        QSocMcpClient *client = manager->findClient(QStringLiteral("svr"));
        connect(client, &QSocMcpClient::stateChanged, client, [&](QSocMcpClient::State state) {
            if (state == QSocMcpClient::State::Disconnected) {
                delete manager.data();
            }
        });

        manager->reconnectServer(QStringLiteral("svr"));
        QVERIFY(manager.isNull());
    }

    void deletionInsideFactoryEndsReplacementSafely()
    {
        QList<McpServerConfig> cfgs{makeConfig("svr")};
        QObject                observer;
        int                    destroyedTransports = 0;

        QPointer<QSocMcpManager> manager = new QSocMcpManager(cfgs);
        manager->setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            connect(transport, &QObject::destroyed, &observer, [&]() { destroyedTransports++; });
            delete manager.data();
            return transport;
        });

        QVERIFY(manager.isNull());
        QCOMPARE(destroyedTransports, 1);
    }

    void deletionDuringStartAllStopsIterationSafely()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("first"), makeConfig("second")};

        QPointer<QSocMcpManager> manager = new QSocMcpManager(cfgs);
        manager->setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QCOMPARE(transports.size(), 2);

        QSocMcpClient *client = manager->findClient(QStringLiteral("first"));
        QVERIFY(client != nullptr);
        connect(client, &QSocMcpClient::stateChanged, client, [&](QSocMcpClient::State state) {
            if (state == QSocMcpClient::State::Connecting) {
                delete manager.data();
            }
        });

        manager->startAll();
        QVERIFY(manager.isNull());
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

    void reconnectClearsAttemptsAfterToolsList()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(20, 100);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });

        manager.startAll();
        QVERIFY(QTest::qWaitFor([&]() { return !transports.isEmpty(); }, 500));
        driveHandshakeAndList(transports.last().data(), {"a"});

        transports.last().data()->simulateClosed();
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() >= 2; }, 500));

        auto *replacement = transports.last().data();
        replyInitialize(replacement, replacement->firstSentId());
        QCOMPARE(manager.reconnectAttempts("svr"), 1);

        auto *client = manager.findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        const int probeId = client->request(QStringLiteral("ping"));
        QVERIFY(probeId > 0);
        replyToolSuccess(replacement, probeId, QStringLiteral("ok"));
        QCOMPARE(manager.reconnectAttempts("svr"), 1);

        const QList<int> listIds = requestIdsForMethod(replacement, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 1);
        replyToolsList(replacement, listIds.first(), {"a"});
        QCOMPARE(manager.reconnectAttempts("svr"), 0);
    }

    void initialListFailureReconnects_data()
    {
        QTest::addColumn<QString>("failure");

        QTest::newRow("rpc-error") << QStringLiteral("rpc-error");
        QTest::newRow("message-failure") << QStringLiteral("message-failure");
        QTest::newRow("recoverable-error") << QStringLiteral("recoverable-error");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("invalid-result") << QStringLiteral("invalid-result");
    }

    void initialListFailureReconnects()
    {
        QFETCH(QString, failure);

        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        McpServerConfig                       config = makeConfig("svr");
        config.requestTimeoutMs                      = 40;

        QSocMcpManager manager({config}, &registry);
        manager.setReconnectDelays(20, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);
        QSignalSpy gaveUpSpy(&manager, &QSocMcpManager::serverGaveUp);

        manager.startAll();
        QCOMPARE(transports.size(), qsizetype(1));
        QPointer<QsocMcpFakeTransport> oldTransport = transports.first();
        QPointer<QSocMcpClient>        oldClient    = manager.findClient(QStringLiteral("svr"));
        QVERIFY(!oldTransport.isNull());
        QVERIFY(!oldClient.isNull());
        QSignalSpy closedSpy(oldClient.data(), &QSocMcpClient::closed);
        QSignalSpy failureSpy(oldClient.data(), &QSocMcpClient::requestFailed);

        auto *transport = oldTransport.data();
        replyInitialize(transport, transport->firstSentId());
        const QList<int> listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 1);
        const int listId = listIds.first();
        for (int i = 0; i < 64; ++i) {
            notifyToolsChanged(transport);
        }
        QCOMPARE(requestIdsForMethod(transport, QStringLiteral("tools/list")).size(), 1);
        bool lateResponseSent = false;
        connect(&manager, &QSocMcpManager::reconnectScheduled, &manager, [&]() {
            if (!oldTransport.isNull()) {
                replyToolsList(oldTransport.data(), listId, {"late"});
                lateResponseSent = true;
            }
        });

        const int probeId = oldClient->request(QStringLiteral("probe"));
        QVERIFY(probeId > 0);
        transport->simulateMessageFailure(0, {probeId}, QStringLiteral("probe failed"));
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), probeId);
        QCOMPARE(oldClient->state(), QSocMcpClient::State::Ready);
        QCOMPARE(scheduleSpy.size(), 0);

        if (failure == QStringLiteral("rpc-error")) {
            replyRequestError(transport, listId);
        } else if (failure == QStringLiteral("message-failure")) {
            transport->simulateMessageFailure(0, {listId}, QStringLiteral("list failed"));
        } else if (failure == QStringLiteral("recoverable-error")) {
            transport->simulateError(QStringLiteral("transport failed"));
        } else if (failure == QStringLiteral("invalid-result")) {
            replyInvalidToolsList(transport, listId);
        } else {
            QCOMPARE(failure, QStringLiteral("timeout"));
        }

        QTRY_COMPARE_WITH_TIMEOUT(scheduleSpy.size(), 1, 500);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(scheduleSpy.first().at(0).toString(), QStringLiteral("svr"));
        QCOMPARE(scheduleSpy.first().at(1).toInt(), 1);
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 1);
        QCOMPARE(gaveUpSpy.size(), 0);

        QVERIFY(lateResponseSent);
        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__late")));
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() == 2; }, 500));
        QVERIFY(manager.findClient(QStringLiteral("svr")) != oldClient.data());
        QCOMPARE(registry.count(), 0);
    }

    void synchronousInitialListFailureReconnectsOnce()
    {
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QSocMcpManager                        manager({makeConfig("svr")});
        manager.setReconnectDelays(20, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QCOMPARE(transports.size(), qsizetype(1));
        auto *transport = transports.first().data();
        auto *client    = manager.findClient(QStringLiteral("svr"));
        QVERIFY(transport != nullptr);
        QVERIFY(client != nullptr);
        QSignalSpy closedSpy(client, &QSocMcpClient::closed);

        bool handled = false;
        transport->setSendHook([transport, &handled](const nlohmann::json &message) {
            if (handled || message.value("method", std::string()) != "tools/list"
                || !message.contains("id")) {
                return;
            }
            handled = true;
            for (int i = 0; i < 64; ++i) {
                notifyToolsChanged(transport);
            }
            replyRequestError(transport, message["id"].get<int>());
        });
        replyInitialize(transport, transport->firstSentId());

        QCOMPARE(requestIdsForMethod(transport, QStringLiteral("tools/list")).size(), 1);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(scheduleSpy.size(), 1);
        QCOMPARE(scheduleSpy.first().at(1).toInt(), 1);
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 1);
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() == 2; }, 500));
    }

    void unownedFailureIsIgnored()
    {
        QSocTestCapture                       capture;
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QSocMcpManager                        manager({makeConfig("svr")}, &registry);
        manager.setReconnectDelays(20, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QCOMPARE(transports.size(), qsizetype(1));
        auto *transport = transports.first().data();
        QVERIFY(transport != nullptr);
        driveHandshakeAndList(transport, {"old"});

        auto *client = manager.findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        QSignalSpy closedSpy(client, &QSocMcpClient::closed);
        QSignalSpy failureSpy(client, &QSocMcpClient::requestFailed);

        nlohmann::json malformed;
        malformed["jsonrpc"] = "2.0";
        malformed["id"]      = 999;
        malformed["result"]  = nlohmann::json::object();
        malformed["error"]   = nlohmann::json::object();
        transport->simulateMessage(malformed);

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), -1);
        QCOMPARE(closedSpy.size(), 0);
        QCOMPARE(scheduleSpy.size(), 0);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);
        QCOMPARE(manager.findClient(QStringLiteral("svr")), client);
        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.hasTool(QStringLiteral("mcp__svr__old")));

        const QString output = capture.text();
        QVERIFY(!output.contains(QStringLiteral("MCP tool discovery failed")));
        QVERIFY(!output.contains(QStringLiteral("MCP tool refresh failed")));
    }

    void refreshFailurePreservesCatalog_data()
    {
        QTest::addColumn<QStringList>("initialTools");
        QTest::addColumn<bool>("invalidResult");

        QTest::newRow("empty-catalog") << QStringList() << false;
        QTest::newRow("populated-catalog") << QStringList{QStringLiteral("old")} << false;
        QTest::newRow("invalid-result") << QStringList{QStringLiteral("old")} << true;
    }

    void refreshFailurePreservesCatalog()
    {
        QFETCH(QStringList, initialTools);
        QFETCH(bool, invalidResult);

        QSocTestCapture                       capture;
        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QSocMcpManager                        manager({makeConfig("svr")}, &registry);
        manager.setReconnectDelays(20, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);
        QSignalSpy gaveUpSpy(&manager, &QSocMcpManager::serverGaveUp);

        manager.startAll();
        QCOMPARE(transports.size(), qsizetype(1));
        auto *transport = transports.first().data();
        QVERIFY(transport != nullptr);
        driveHandshakeAndList(transport, initialTools);

        QSocMcpClient *client = manager.findClient(QStringLiteral("svr"));
        QVERIFY(client != nullptr);
        QPointer<QSocTool> oldTool = registry.getTool(QStringLiteral("mcp__svr__old"));

        nlohmann::json notification;
        notification["jsonrpc"] = "2.0";
        notification["method"]  = "notifications/tools/list_changed";
        notification["params"]  = nlohmann::json::object();
        transport->simulateMessage(notification);

        QList<int> listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 2);
        const int failedId = listIds.last();
        if (invalidResult) {
            replyInvalidToolsList(transport, failedId);
        } else {
            transport->simulateMessageFailure(0, {failedId}, QStringLiteral("refresh failed"));
        }
        flushQueuedCalls();
        QCOMPARE(capture.text().count(QStringLiteral("MCP tool refresh failed")), 1);

        QCOMPARE(manager.findClient(QStringLiteral("svr")), client);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);
        QCOMPARE(transports.size(), qsizetype(1));
        QCOMPARE(scheduleSpy.size(), 0);
        QCOMPARE(gaveUpSpy.size(), 0);
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 0);
        QCOMPARE(registry.count(), initialTools.size());
        if (!oldTool.isNull()) {
            QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__old")), oldTool.data());
        }

        replyToolsList(transport, failedId, {"late"});
        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__late")));
        QCOMPARE(registry.count(), initialTools.size());

        transport->simulateMessage(notification);
        listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 3);
        QVERIFY(listIds.last() != failedId);
        replyToolsList(transport, listIds.last(), {"new"});

        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.hasTool(QStringLiteral("mcp__svr__new")));
        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__old")));
        QCOMPARE(manager.findClient(QStringLiteral("svr")), client);
        QCOMPARE(transports.size(), qsizetype(1));
    }

    void replacementListFailureIsInitial_data()
    {
        QTest::addColumn<bool>("manualReconnect");

        QTest::newRow("closed-client") << false;
        QTest::newRow("manual-reconnect") << true;
    }

    void replacementListFailureIsInitial()
    {
        QFETCH(bool, manualReconnect);

        QSocToolRegistry                      registry;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QSocMcpManager                        manager({makeConfig("svr")}, &registry);
        manager.setReconnectDelays(20, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QCOMPARE(transports.size(), qsizetype(1));
        auto *firstTransport = transports.first().data();
        QVERIFY(firstTransport != nullptr);
        driveHandshakeAndList(firstTransport, {"old"});
        QVERIFY(registry.hasTool(QStringLiteral("mcp__svr__old")));

        const int expectedAttempt = manualReconnect ? 1 : 2;
        if (manualReconnect) {
            QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        } else {
            firstTransport->simulateClosed();
        }
        QVERIFY(QTest::qWaitFor([&]() { return transports.size() == 2; }, 500));
        scheduleSpy.clear();
        QCOMPARE(registry.count(), 0);

        auto *replacement = transports.last().data();
        QVERIFY(replacement != nullptr);
        auto *replacementClient = manager.findClient(QStringLiteral("svr"));
        QVERIFY(replacementClient != nullptr);
        QSignalSpy closedSpy(replacementClient, &QSocMcpClient::closed);

        replyInitialize(replacement, replacement->firstSentId());
        const QList<int> listIds = requestIdsForMethod(replacement, QStringLiteral("tools/list"));
        QCOMPARE(listIds.size(), 1);
        replacement->simulateMessageFailure(0, {listIds.first()}, QStringLiteral("list failed"));

        QTRY_COMPARE_WITH_TIMEOUT(scheduleSpy.size(), 1, 500);
        QCOMPARE(scheduleSpy.first().at(1).toInt(), expectedAttempt);
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), expectedAttempt);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(registry.count(), 0);
    }

    void givesUpWhenInitialToolsListAlwaysFails()
    {
        QSocTestCapture                       capture;
        QList<QPointer<QsocMcpFakeTransport>> transports;
        QList<McpServerConfig>                cfgs{makeConfig("svr")};

        QSocMcpManager manager(cfgs);
        manager.setReconnectDelays(10, 50);
        manager.setTransportFactory([&](const McpServerConfig &) {
            auto *transport = new QsocMcpFakeTransport;
            transports << transport;
            return transport;
        });
        QSignalSpy scheduleSpy(&manager, &QSocMcpManager::reconnectScheduled);
        QSignalSpy gaveUpSpy(&manager, &QSocMcpManager::serverGaveUp);

        manager.startAll();
        const int cap          = QSocMcpManager::kMaxReconnectAttempts;
        int       processedRun = 0;
        while (gaveUpSpy.isEmpty() && processedRun < cap + 2) {
            QVERIFY(QTest::qWaitFor([&]() { return transports.size() > processedRun; }, 1000));
            auto *transport = transports.at(processedRun).data();
            QVERIFY(transport != nullptr);
            replyInitialize(transport, transport->firstSentId());
            const QList<int> listIds = requestIdsForMethod(transport, QStringLiteral("tools/list"));
            QCOMPARE(listIds.size(), 1);
            transport->simulateMessageFailure(0, {listIds.first()}, QStringLiteral("list failed"));
            processedRun++;
        }

        QVERIFY(QTest::qWaitFor([&]() { return !gaveUpSpy.isEmpty(); }, 1000));
        QCOMPARE(gaveUpSpy.size(), 1);
        QCOMPARE(gaveUpSpy.first().at(0).toString(), QStringLiteral("svr"));
        QCOMPARE(scheduleSpy.size(), cap);
        for (int attempt = 1; attempt <= cap; ++attempt) {
            QCOMPARE(scheduleSpy.at(attempt - 1).at(1).toInt(), attempt);
        }
        QCOMPARE(processedRun, cap + 1);
        QCOMPARE(transports.size(), qsizetype(cap + 1));
        QVERIFY(manager.hasGivenUp(QStringLiteral("svr")));
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), cap);
        QCOMPARE(manager.clientCount(), qsizetype(0));
        QCOMPARE(activeReconnectTimers(manager).size(), 0);

        const QString output = capture.text();
        QCOMPARE(output.count(QStringLiteral("; reconnecting")), cap);
        QCOMPARE(output.count(QStringLiteral("retry limit reached")), 1);
        QVERIFY(output.contains(QStringLiteral("use /mcp reconnect svr")));
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
        QCOMPARE(manager.clientCount(), qsizetype(0));
        QCOMPARE(activeReconnectTimers(manager).size(), 0);
        QCOMPARE(manager.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly).size(), 1);

        const qsizetype transportCount = transports.size();
        QVERIFY(manager.reconnectServer(QStringLiteral("svr")));
        QVERIFY(!manager.hasGivenUp(QStringLiteral("svr")));
        QCOMPARE(manager.reconnectAttempts(QStringLiteral("svr")), 0);
        QCOMPARE(manager.clientCount(), qsizetype(1));
        QCOMPARE(transports.size(), transportCount + 1);
        QCOMPARE(activeReconnectTimers(manager).size(), 0);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpreconnect.moc"
