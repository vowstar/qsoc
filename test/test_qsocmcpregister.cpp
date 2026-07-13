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
#include <functional>
#include <nlohmann/json.hpp>
#include <utility>

#include <QSignalSpy>
#include <QTimer>
#include <QtCore>
#include <QtTest>

Q_DECLARE_METATYPE(nlohmann::json)

namespace {

class FakeTransport : public QSocMcpTransport
{
    Q_OBJECT

public:
    using SendHook = std::function<void(const nlohmann::json &)>;

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
        const SendHook hook = sendHook_;
        if (hook) {
            hook(message);
        }
        if (failToolCallSynchronously_ && message.value("method", std::string()) == "tools/call") {
            emit errorOccurred(QStringLiteral("synchronous send failure"));
        }
    }

    void simulateMessage(const nlohmann::json &message) { emit messageReceived(message); }
    void setFailToolCallSynchronously(bool fail) { failToolCallSynchronously_ = fail; }
    void setSendHook(SendHook hook) { sendHook_ = std::move(hook); }

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
    SendHook              sendHook_;
};

struct WireValue
{
    const char    *name;
    nlohmann::json value;
};

QList<WireValue> allWireValues()
{
    return {
        {"null", nullptr},
        {"object", nlohmann::json::object()},
        {"array", nlohmann::json::array()},
        {"string", "wrong"},
        {"boolean", true},
        {"integer", -1},
        {"unsigned", 1U},
        {"float", 1.5},
    };
}

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

void replyToolResult(FakeTransport *transport, int id, const nlohmann::json &result)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = result;
    transport->simulateMessage(resp);
}

QString executeToolResult(
    QSocMcpTool *tool, FakeTransport *transport, nlohmann::json result, bool *threw)
{
    transport->setSendHook([transport, result = std::move(result)](const nlohmann::json &message) {
        const auto method = message.find("method");
        const auto id     = message.find("id");
        if (method == message.end() || !method->is_string() || *method != "tools/call"
            || id == message.end() || !id->is_number_integer()) {
            return;
        }
        replyToolResult(transport, id->get<int>(), result);
    });

    QString output;
    *threw = false;
    try {
        output = tool->execute(nlohmann::json::object());
    } catch (...) {
        *threw = true;
    }
    transport->setSendHook({});
    return output;
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
        QCOMPARE(output, QStringLiteral("[mcp tool error] boom"));
    }

    void malformedToolResultsAreRejected_data()
    {
        QTest::addColumn<nlohmann::json>("result");

        const auto add = [](const QByteArray &name, const nlohmann::json &result) {
            QTest::newRow(name.constData()) << result;
        };
        const QList<WireValue> values = allWireValues();

        for (const WireValue &wire : values) {
            if (!wire.value.is_object()) {
                add(QByteArrayLiteral("result-") + wire.name, wire.value);
            }
        }
        add("content-missing", nlohmann::json::object());
        for (const WireValue &wire : values) {
            if (!wire.value.is_array()) {
                add(QByteArrayLiteral("content-") + wire.name, {{"content", wire.value}});
            }
            if (!wire.value.is_boolean()) {
                add(QByteArrayLiteral("is-error-") + wire.name,
                    {{"content", nlohmann::json::array()}, {"isError", wire.value}});
            }
            if (!wire.value.is_object()) {
                add(QByteArrayLiteral("item-") + wire.name,
                    {{"content", nlohmann::json::array({wire.value})}});
            }
            if (!wire.value.is_string()) {
                add(QByteArrayLiteral("type-") + wire.name,
                    {{"content",
                      nlohmann::json::array(
                          {{{"type", wire.value}}, {{"type", "text"}, {"text", "unreachable"}}})}});
                add(QByteArrayLiteral("text-") + wire.name,
                    {{"content",
                      nlohmann::json::array(
                          {{{"type", "text"}, {"text", wire.value}},
                           {{"type", "text"}, {"text", "unreachable"}}})}});
            }
        }
        add("type-missing", {{"content", nlohmann::json::array({nlohmann::json::object()})}});
        add("text-missing", {{"content", nlohmann::json::array({{{"type", "text"}}})}});
        add("image-data-missing",
            {{"content", nlohmann::json::array({{{"type", "image"}, {"mimeType", "image/test"}}})}});
        add("image-data-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "image"}, {"data", false}, {"mimeType", "image/test"}}})}});
        add("image-mime-missing",
            {{"content", nlohmann::json::array({{{"type", "image"}, {"data", "payload"}}})}});
        add("image-mime-wrong",
            {{"content",
              nlohmann::json::array({{{"type", "image"}, {"data", "payload"}, {"mimeType", 7}}})}});
        add("audio-data-missing",
            {{"content", nlohmann::json::array({{{"type", "audio"}, {"mimeType", "audio/test"}}})}});
        add("audio-data-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "audio"}, {"data", false}, {"mimeType", "audio/test"}}})}});
        add("audio-mime-missing",
            {{"content", nlohmann::json::array({{{"type", "audio"}, {"data", "payload"}}})}});
        add("audio-mime-wrong",
            {{"content",
              nlohmann::json::array({{{"type", "audio"}, {"data", "payload"}, {"mimeType", 7}}})}});
        add("resource-missing", {{"content", nlohmann::json::array({{{"type", "resource"}}})}});
        add("resource-wrong",
            {{"content", nlohmann::json::array({{{"type", "resource"}, {"resource", false}}})}});
        add("resource-uri-missing",
            {{"content",
              nlohmann::json::array({{{"type", "resource"}, {"resource", {{"text", "body"}}}}})}});
        add("resource-uri-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"}, {"resource", {{"uri", false}, {"text", "body"}}}}})}});
        add("resource-payload-missing",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"}, {"resource", {{"uri", "test:resource"}}}}})}});
        add("resource-text-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource", {{"uri", "test:resource"}, {"text", false}}}}})}});
        add("resource-blob-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource", {{"uri", "test:resource"}, {"blob", false}}}}})}});
        add("resource-mime-wrong",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource",
                     {{"uri", "test:resource"}, {"text", "body"}, {"mimeType", false}}}}})}});
    }

    void malformedToolResultsAreRejected()
    {
        QFETCH(nlohmann::json, result);

        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";
        QSocMcpTool tool(&client, desc);

        bool          threw  = false;
        const QString output = executeToolResult(&tool, transport, result, &threw);
        QVERIFY(!threw);
        QCOMPARE(
            output,
            QStringLiteral(
                "[mcp protocol error] invalid tools/call result; tool may have completed, do not "
                "retry automatically"));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const QString recovered = executeToolResult(
            &tool,
            transport,
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "recovered"}}})}},
            &threw);
        QVERIFY(!threw);
        QCOMPARE(recovered, QStringLiteral("recovered"));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void toolFormatsContentBlocks_data()
    {
        QTest::addColumn<nlohmann::json>("result");
        QTest::addColumn<QString>("expected");

        const auto add = [](const char           *name,
                            const nlohmann::json &result,
                            const QString &expected) { QTest::newRow(name) << result << expected; };
        add("empty-success",
            {{"content", nlohmann::json::array()}},
            QStringLiteral("[mcp result] no content"));
        add("empty-error",
            {{"content", nlohmann::json::array()}, {"isError", true}},
            QStringLiteral("[mcp tool error] no details"));
        add("empty-text",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", ""}}})}},
            QStringLiteral("[mcp result] no content"));
        add("blank-text-blocks",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "text"}, {"text", " "}}, {{"type", "text"}, {"text", "\t"}}})}},
            QStringLiteral("[mcp result] no content"));
        add("empty-text-error",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", ""}}})},
             {"isError", true}},
            QStringLiteral("[mcp tool error] no details"));
        add("explicit-success",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "alpha"}}})},
             {"isError", false}},
            QStringLiteral("alpha"));
        add("multiple-text",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "text"}, {"text", "alpha"}}, {{"type", "text"}, {"text", "beta"}}})}},
            QStringLiteral("alpha\nbeta"));
        add("image",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "image"}, {"data", "payload"}, {"mimeType", "image/test"}}})}},
            QStringLiteral("[mcp unsupported content omitted: image]"));
        add("audio",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "audio"}, {"data", "payload"}, {"mimeType", "audio/test"}}})}},
            QStringLiteral("[mcp unsupported content omitted: audio]"));
        add("text-resource",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource", {{"uri", "test:resource"}, {"text", "resource body"}}}}})}},
            QStringLiteral("resource body"));
        add("text-resource-with-invalid-blob-extension",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource",
                     {{"uri", "test:resource"}, {"text", "resource body"}, {"blob", false}}}}})}},
            QStringLiteral("resource body"));
        add("blob-resource",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource", {{"uri", "test:resource"}, {"blob", "payload"}}}}})}},
            QStringLiteral("[mcp unsupported content omitted: resource]"));
        add("blob-resource-with-invalid-text-extension",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "resource"},
                    {"resource",
                     {{"uri", "test:resource"}, {"text", false}, {"blob", "payload"}}}}})}},
            QStringLiteral("[mcp unsupported content omitted: resource]"));
        add("unknown",
            {{"content", nlohmann::json::array({{{"type", "future"}, {"value", 7}}})}},
            QStringLiteral("[mcp unsupported content omitted: unknown]"));
        add("structured-only",
            {{"content", nlohmann::json::array()}, {"structuredContent", {{"answer", 42}}}},
            QStringLiteral("[mcp unsupported content omitted: structured]"));
        add("structured-with-text",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "alpha"}}})},
             {"structuredContent", {{"answer", 42}}}},
            QStringLiteral("alpha"));
        add("structured-with-blank-text",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", " "}}})},
             {"structuredContent", {{"answer", 42}}}},
            QStringLiteral("[mcp unsupported content omitted: structured]"));
        add("mixed-order",
            {{"content",
              nlohmann::json::array(
                  {{{"type", "text"}, {"text", "alpha"}},
                   {{"type", "image"}, {"data", "payload"}, {"mimeType", "image/test"}},
                   {{"type", "resource"},
                    {"resource", {{"uri", "test:resource"}, {"text", "resource body"}}}},
                   {{"type", "future"}},
                   {{"type", "text"}, {"text", "omega"}}})}},
            QStringLiteral(
                "alpha\n[mcp unsupported content omitted: image]\nresource body\n"
                "[mcp unsupported content omitted: unknown]\nomega"));
        add("business-error",
            {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "boom"}}})},
             {"isError", true}},
            QStringLiteral("[mcp tool error] boom"));
    }

    void toolFormatsContentBlocks()
    {
        QFETCH(nlohmann::json, result);
        QFETCH(QString, expected);

        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";
        QSocMcpTool tool(&client, desc);

        bool          threw  = false;
        const QString output = executeToolResult(&tool, transport, result, &threw);
        QVERIFY(!threw);
        QCOMPARE(output, expected);
    }

    void malformedAsyncToolResultIsRejected()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";
        QSocMcpTool tool(&client, desc);

        QTimer::singleShot(0, &client, [transport]() {
            replyToolResult(
                transport,
                transport->lastSentId(),
                {{"content", nlohmann::json::array()}, {"isError", "wrong"}});
        });
        QCOMPARE(
            tool.execute(nlohmann::json::object()),
            QStringLiteral(
                "[mcp protocol error] invalid tools/call result; tool may have completed, do not "
                "retry automatically"));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        QTimer::singleShot(0, &client, [transport]() {
            replyToolSuccess(transport, transport->lastSentId(), QStringLiteral("recovered"));
        });
        QCOMPARE(tool.execute(nlohmann::json::object()), QStringLiteral("recovered"));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void malformedDuplicateResultIsIgnored()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(makeConfig("svr"), transport);
        QVERIFY(driveClientToReady(&client, transport));

        McpToolDescriptor desc;
        desc.serverName = "svr";
        desc.toolName   = "echo";
        QSocMcpTool tool(&client, desc);

        transport->setSendHook([transport](const nlohmann::json &message) {
            const auto method = message.find("method");
            const auto id     = message.find("id");
            if (method == message.end() || !method->is_string() || *method != "tools/call"
                || id == message.end() || !id->is_number_integer()) {
                return;
            }
            replyToolSuccess(transport, id->get<int>(), QStringLiteral("first"));
            replyToolResult(
                transport, id->get<int>(), {{"content", nlohmann::json::array({{{"type", 7}}})}});
        });
        QCOMPARE(tool.execute(nlohmann::json::object()), QStringLiteral("first"));
        transport->setSendHook({});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
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
