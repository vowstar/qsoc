// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

#include <QSignalSpy>
#include <QTimer>
#include <QtCore>
#include <QtTest>

namespace {

class FakeTransport : public QSocMcpTransport
{
    Q_OBJECT

public:
    explicit FakeTransport(QObject *parent = nullptr)
        : QSocMcpTransport(parent)
    {}

    void start() override
    {
        setState(State::Running);
        emit started();
    }

    void stop() override
    {
        if (state() == State::Stopped) {
            return;
        }
        setState(State::Stopped);
        emit closed();
    }

    void sendMessage(const nlohmann::json &message) override
    {
        sent_ << message;
        if (failToolCallSynchronously_ && message.value("method", std::string()) == "tools/call") {
            emit errorOccurred(QStringLiteral("synchronous send failure"));
        }
    }

    void simulateMessage(const nlohmann::json &message) { emit messageReceived(message); }
    void setFailToolCallSynchronously(bool fail) { failToolCallSynchronously_ = fail; }

    int firstSentId() const { return sent_.first()["id"].get<int>(); }
    int lastSentId() const { return sent_.last()["id"].get<int>(); }

    QList<int> requestIdsForMethod(const QString &method) const
    {
        QList<int> ids;
        for (const auto &message : sent_) {
            if (message.value("method", std::string()) == method.toStdString()
                && message.contains("id") && message["id"].is_number_integer()) {
                ids.append(message["id"].get<int>());
            }
        }
        return ids;
    }

    QList<int> cancelledRequestIds() const
    {
        QList<int> ids;
        for (const auto &message : sent_) {
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

private:
    QList<nlohmann::json> sent_;
    bool                  failToolCallSynchronously_ = false;
};

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

void replyInitialize(FakeTransport *transport, int id)
{
    nlohmann::json resp;
    resp["jsonrpc"]                = "2.0";
    resp["id"]                     = id;
    resp["result"]["capabilities"] = nlohmann::json::object();
    transport->simulateMessage(resp);
}

void replyToolSuccess(FakeTransport *transport, int id, const QString &text)
{
    nlohmann::json resp;
    resp["jsonrpc"]           = "2.0";
    resp["id"]                = id;
    resp["result"]["content"] = nlohmann::json::array(
        {{{"type", "text"}, {"text", text.toStdString()}}});
    transport->simulateMessage(resp);
}

bool driveClientToReady(QSocMcpClient *client, FakeTransport *transport)
{
    client->start();
    replyInitialize(transport, transport->firstSentId());
    return QTest::qWaitFor(
        [client]() { return client->state() == QSocMcpClient::State::Ready; }, 1000);
}

class DeleteOnAbortTool : public QSocTool
{
public:
    DeleteOnAbortTool(QString name, QSocTool *target)
        : name_(std::move(name))
        , target_(target)
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return {}; }
    json    getParametersSchema() const override { return json::object(); }
    QString execute(const json &) override { return {}; }
    void    abort() override { delete target_.data(); }

private:
    QString            name_;
    QPointer<QSocTool> target_;
};

class DeleteRegistryTool : public QSocTool
{
public:
    explicit DeleteRegistryTool(QSocToolRegistry *registry)
        : registry_(registry)
    {}

    QString getName() const override { return QStringLiteral("delete_registry"); }
    QString getDescription() const override { return {}; }
    json    getParametersSchema() const override { return json::object(); }

    QString execute(const json &) override
    {
        delete registry_.data();
        return QStringLiteral("finished");
    }

private:
    QPointer<QSocToolRegistry> registry_;
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void toolReportsCorrectMetadata()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);

        McpToolDescriptor desc;
        desc.serverName  = "svr";
        desc.toolName    = "echo";
        desc.description = "Echoes back input";
        desc.inputSchema
            = {{"type", "object"},
               {"properties", {{"text", {{"type", "string"}}}}},
               {"required", nlohmann::json::array({"text"})}};

        QSocMcpTool tool(&client, desc);
        QCOMPARE(tool.getName(), QStringLiteral("mcp__svr__echo"));
        QCOMPARE(tool.getDescription(), QStringLiteral("Echoes back input"));

        const auto schema = tool.getParametersSchema();
        QCOMPARE(QString::fromStdString(schema["type"].get<std::string>()), QStringLiteral("object"));
        QVERIFY(schema["properties"].contains("text"));
    }

    void toolReturnsServerText()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName  = "svr";
        desc.toolName    = "echo";
        desc.inputSchema = {{"type", "object"}};

        QSocMcpTool tool(&client, desc);

        QTimer::singleShot(0, &client, [transport]() {
            nlohmann::json resp;
            resp["jsonrpc"]        = "2.0";
            resp["id"]             = transport->lastSentId();
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"}, {"text", "echoed: hello"}});
            resp["result"]["content"] = content;
            transport->simulateMessage(resp);
        });

        const QString output = tool.execute({{"text", "hello"}});
        QCOMPARE(output, QStringLiteral("echoed: hello"));
    }

    void toolReportsBusinessError()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName  = "svr";
        desc.toolName    = "fail";
        desc.inputSchema = {{"type", "object"}};

        QSocMcpTool tool(&client, desc);

        QTimer::singleShot(0, &client, [transport]() {
            nlohmann::json resp;
            resp["jsonrpc"]        = "2.0";
            resp["id"]             = transport->lastSentId();
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"}, {"text", "boom"}});
            resp["result"]["isError"] = true;
            resp["result"]["content"] = content;
            transport->simulateMessage(resp);
        });

        const QString output = tool.execute({});
        QVERIFY(output.startsWith(QStringLiteral("[mcp tool error]")));
        QVERIFY(output.contains(QStringLiteral("boom")));
    }

    void toolPropagatesRpcError()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName  = "svr";
        desc.toolName    = "missing";
        desc.inputSchema = {{"type", "object"}};

        QSocMcpTool tool(&client, desc);

        QTimer::singleShot(0, &client, [transport]() {
            nlohmann::json resp;
            resp["jsonrpc"]          = "2.0";
            resp["id"]               = transport->lastSentId();
            resp["error"]["code"]    = -32601;
            resp["error"]["message"] = "Method not found";
            transport->simulateMessage(resp);
        });

        const QString output = tool.execute({});
        QVERIFY(output.contains(QStringLiteral("Method not found")));
        QVERIFY(output.contains(QStringLiteral("32601")));
    }

    void toolHandlesSynchronousTransportFailure()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";

        QSocMcpTool tool(&client, desc);
        transport->setFailToolCallSynchronously(true);

        bool   fallbackFired = false;
        QTimer fallback;
        fallback.setSingleShot(true);
        connect(&fallback, &QTimer::timeout, &fallback, [&]() {
            fallbackFired = true;
            tool.abort();
        });
        fallback.start(500);

        const QString output = tool.execute({});
        fallback.stop();

        QVERIFY(!fallbackFired);
        QCOMPARE(output, QStringLiteral("[mcp error -32000] synchronous send failure"));
    }

    void toolFailsFastWhenServerNotReady()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";

        QSocMcpTool   tool(&client, desc);
        const QString output = tool.execute({});
        QVERIFY(output.contains(QStringLiteral("not ready")));
    }

    void abortStopsEveryNestedCall()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName  = "svr";
        desc.toolName    = "wait";
        desc.inputSchema = {{"type", "object"}};

        QSocMcpTool tool(&client, desc);
        QString     innerOutput;

        QTimer::singleShot(0, &tool, [&]() {
            QTimer::singleShot(0, &tool, &QSocMcpTool::abort);
            innerOutput = tool.execute({{"depth", "inner"}});
        });

        /* Keep a broken implementation bounded: answer any calls that remain
         * after abort so the regression fails quickly instead of timing out. */
        QTimer::singleShot(250, &client, [transport]() {
            const QList<int> ids = transport->requestIdsForMethod(QStringLiteral("tools/call"));
            for (int id : ids) {
                replyToolSuccess(transport, id, QStringLiteral("late"));
            }
        });

        const QString outerOutput = tool.execute({{"depth", "outer"}});
        QCOMPARE(innerOutput, QStringLiteral("[mcp aborted]"));
        QCOMPARE(outerOutput, QStringLiteral("[mcp aborted]"));

        QList<int> requestIds = transport->requestIdsForMethod(QStringLiteral("tools/call"));
        QList<int> cancelled  = transport->cancelledRequestIds();
        std::sort(requestIds.begin(), requestIds.end());
        std::sort(cancelled.begin(), cancelled.end());
        QCOMPARE(requestIds.size(), 2);
        QCOMPARE(cancelled, requestIds);
    }

    void managerCountsZeroToolsBeforeListResponse()
    {
        QSocToolRegistry       registry;
        QList<McpServerConfig> cfgs;
        cfgs << makeConfig("alpha");
        QSocMcpManager manager(cfgs, &registry);
        QCOMPARE(manager.clientCount(), qsizetype(1));
        QCOMPARE(manager.totalToolCount(), qsizetype(0));
        QVERIFY(manager.toolsForClient("alpha").isEmpty());
    }

    void registryDropsDestroyedTool()
    {
        QSocToolRegistry  registry;
        McpToolDescriptor desc;
        desc.serverName = QStringLiteral("svr");
        desc.toolName   = QStringLiteral("echo");

        auto *tool = new QSocMcpTool(nullptr, desc);
        registry.registerTool(tool);
        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), tool);

        delete tool;

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__echo")));
        QCOMPARE(registry.count(), 0);
        QVERIFY(registry.toolNames().isEmpty());
        QVERIFY(registry.getToolDefinitions().empty());
    }

    void destroyedOldToolKeepsReplacement()
    {
        QSocToolRegistry  registry;
        McpToolDescriptor desc;
        desc.serverName = QStringLiteral("svr");
        desc.toolName   = QStringLiteral("echo");

        auto *oldTool = new QSocMcpTool(nullptr, desc);
        auto *newTool = new QSocMcpTool(nullptr, desc);
        registry.registerTool(oldTool);
        registry.registerTool(newTool);

        delete oldTool;

        QCOMPARE(registry.getTool(QStringLiteral("mcp__svr__echo")), newTool);
        QCOMPARE(registry.count(), 1);
        delete newTool;
        QCOMPARE(registry.count(), 0);
    }

    void unregisterIsLocalAndIdentityChecked()
    {
        QSocToolRegistry  firstRegistry;
        QSocToolRegistry  secondRegistry;
        McpToolDescriptor desc;
        desc.serverName = QStringLiteral("svr");
        desc.toolName   = QStringLiteral("echo");

        auto *oldTool = new QSocMcpTool(nullptr, desc);
        auto *newTool = new QSocMcpTool(nullptr, desc);
        firstRegistry.registerTool(oldTool);
        secondRegistry.registerTool(oldTool);
        firstRegistry.registerTool(newTool);

        QVERIFY(!firstRegistry.unregisterTool(oldTool));
        QCOMPARE(firstRegistry.getTool(QStringLiteral("mcp__svr__echo")), newTool);
        QVERIFY(secondRegistry.unregisterTool(oldTool));
        QVERIFY(!secondRegistry.hasTool(QStringLiteral("mcp__svr__echo")));
        QCOMPARE(firstRegistry.getTool(QStringLiteral("mcp__svr__echo")), newTool);

        delete oldTool;
        QVERIFY(firstRegistry.unregisterTool(newTool));
        delete newTool;
    }

    void abortAllToleratesSynchronousRemoval()
    {
        QSocToolRegistry  registry;
        McpToolDescriptor desc;
        desc.serverName = QStringLiteral("svr");
        desc.toolName   = QStringLiteral("target");

        auto *target  = new QSocMcpTool(nullptr, desc);
        auto *remover = new DeleteOnAbortTool(QStringLiteral("a_remover"), target);
        registry.registerTool(remover);
        registry.registerTool(target);

        registry.abortAll();

        QVERIFY(!registry.hasTool(QStringLiteral("mcp__svr__target")));
        QCOMPARE(registry.getTool(QStringLiteral("a_remover")), remover);
        QCOMPARE(registry.count(), 1);
        delete remover;
    }

    void executeToleratesRegistryDestruction()
    {
        QPointer<QSocToolRegistry> registry = new QSocToolRegistry;
        DeleteRegistryTool         tool(registry.data());
        registry->registerTool(&tool);

        const QString output = registry->executeTool(tool.getName(), json::object());

        QCOMPARE(output, QStringLiteral("finished"));
        QVERIFY(registry.isNull());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpregister.moc"
