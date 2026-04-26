// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

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

    void sendMessage(const nlohmann::json &message) override { sent_ << message; }

    void simulateMessage(const nlohmann::json &message) { emit messageReceived(message); }

    int firstSentId() const { return sent_.first()["id"].get<int>(); }
    int lastSentId() const { return sent_.last()["id"].get<int>(); }

private:
    QList<nlohmann::json> sent_;
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

bool driveClientToReady(QSocMcpClient *client, FakeTransport *transport)
{
    client->start();
    replyInitialize(transport, transport->firstSentId());
    return QTest::qWaitFor(
        [client]() { return client->state() == QSocMcpClient::State::Ready; }, 1000);
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
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpregister.moc"
