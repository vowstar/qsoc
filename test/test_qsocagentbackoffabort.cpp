// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocgoal.h"
#include "agent/qsochookmanager.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QQueue>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <utility>

using json = nlohmann::json;

namespace {

struct MockResponse
{
    int        statusCode = 200;
    QByteArray contentType;
    QByteArray body;
    bool       holdOpen = false;
};

json buildToolCalls(const QStringList &names)
{
    json calls = json::array();
    for (qsizetype index = 0; index < names.size(); ++index) {
        calls.push_back(
            {{"index", index},
             {"id", QStringLiteral("call_%1").arg(index).toStdString()},
             {"type", "function"},
             {"function", {{"name", names.at(index).toStdString()}, {"arguments", "{}"}}}});
    }
    return calls;
}

class MockServer final : public QObject
{
public:
    explicit MockServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                buffers_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    consumeRequest(socket);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(server_.serverAddress().toString());
        result.setPort(server_.serverPort());
        result.setPath(QStringLiteral("/chat/completions"));
        return result;
    }

    void enqueueError(int statusCode)
    {
        const json body = {{"error", {{"message", "temporarily unavailable"}}}};
        responses_.enqueue(
            {statusCode,
             QByteArrayLiteral("application/json"),
             QByteArray::fromStdString(body.dump())});
    }

    void enqueueStream(const QString &content)
    {
        const json contentChunk = {
            {"choices", json::array({{{"delta", {{"content", content.toStdString()}}}}})}};
        const json finishChunk = {
            {"choices", json::array({{{"delta", json::object()}, {"finish_reason", "stop"}}})}};
        QByteArray body = QByteArrayLiteral("data: ")
                          + QByteArray::fromStdString(contentChunk.dump())
                          + QByteArrayLiteral("\n\ndata: ")
                          + QByteArray::fromStdString(finishChunk.dump())
                          + QByteArrayLiteral("\n\ndata: [DONE]\n\n");
        responses_.enqueue({200, QByteArrayLiteral("text/event-stream"), std::move(body)});
    }

    void enqueueCompletion(const QString &content)
    {
        const json response = {
            {"choices",
             json::array(
                 {{{"message", {{"role", "assistant"}, {"content", content.toStdString()}}}}})},
        };
        responses_.enqueue(
            {200,
             QByteArrayLiteral("application/json"),
             QByteArray::fromStdString(response.dump())});
    }

    void enqueueToolCall(const QString &name) { enqueueToolCalls({name}); }

    void enqueueToolCalls(const QStringList &names)
    {
        const json chunk = {
            {"choices",
             json::array(
                 {{{"delta", {{"tool_calls", buildToolCalls(names)}}},
                   {"finish_reason", "tool_calls"}}})},
        };
        QByteArray body = QByteArrayLiteral("data: ") + QByteArray::fromStdString(chunk.dump())
                          + QByteArrayLiteral("\n\ndata: [DONE]\n\n");
        responses_.enqueue({200, QByteArrayLiteral("text/event-stream"), std::move(body)});
    }

    void enqueueToolCompletion(const QStringList &names)
    {
        const json response = {
            {"choices",
             json::array(
                 {{{"message",
                    {{"role", "assistant"},
                     {"content", nullptr},
                     {"tool_calls", buildToolCalls(names)}}}}})},
        };
        responses_.enqueue(
            {200,
             QByteArrayLiteral("application/json"),
             QByteArray::fromStdString(response.dump())});
    }

    void enqueueHeldRequest() { responses_.enqueue({200, {}, {}, true}); }

    void setRequestObserver(std::function<void(int)> observer)
    {
        requestObserver_ = std::move(observer);
    }

    bool releaseHeldRequest()
    {
        const json response = {
            {"choices",
             json::array({{{"message", {{"role", "assistant"}, {"content", "released"}}}}})}};
        const QByteArray     body = QByteArray::fromStdString(response.dump());
        QPointer<QTcpSocket> held;
        for (const QPointer<QTcpSocket> &socket : std::as_const(heldSockets_)) {
            if (socket.isNull() || socket->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            held = socket;
            break;
        }
        heldSockets_.clear();
        if (held.isNull()) {
            return false;
        }
        QByteArray headers = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
        headers += QByteArray::number(body.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        held->write(headers + body);
        held->flush();
        held->disconnectFromHost();
        return true;
    }

    int requestCount() const { return requestCount_; }

    QByteArray requestBody(int index) const { return requestBodies_.value(index); }

private:
    void consumeRequest(QTcpSocket *socket)
    {
        auto it = buffers_.find(socket);
        if (it == buffers_.end()) {
            return;
        }
        it.value().append(socket->readAll());
        const qsizetype headerEnd = it.value().indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        qsizetype contentLength = 0;
        for (QByteArray line : it.value().left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                contentLength = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
            }
        }
        const qsizetype bodyStart = headerEnd + 4;
        if (it.value().size() < bodyStart + contentLength) {
            return;
        }

        requestBodies_.append(it.value().mid(bodyStart, contentLength));
        buffers_.erase(it);
        const int requestIndex = requestCount_++;
        if (requestObserver_) {
            requestObserver_(requestIndex);
        }
        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response = responses_.dequeue();
        if (response.holdOpen) {
            heldSockets_.append(socket);
            return;
        }
        const QByteArray reason = response.statusCode == 200 ? QByteArrayLiteral("OK")
                                                             : QByteArrayLiteral("Test Error");
        QByteArray headers      = QByteArrayLiteral("HTTP/1.1 ")
                                  + QByteArray::number(response.statusCode) + QByteArrayLiteral(" ")
                                  + reason + QByteArrayLiteral("\r\nContent-Type: ")
                                  + response.contentType + QByteArrayLiteral("\r\nContent-Length: ")
                                  + QByteArray::number(response.body.size())
                                  + QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + response.body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QQueue<MockResponse>            responses_;
    QList<QByteArray>               requestBodies_;
    QList<QPointer<QTcpSocket>>     heldSockets_;
    QTcpServer                      server_;
    std::function<void(int)>        requestObserver_;
    int                             requestCount_ = 0;
};

QString lastUserMessage(const MockServer &server, int requestIndex)
{
    const json payload = json::parse(server.requestBody(requestIndex).toStdString());
    if (!payload.contains("messages") || !payload["messages"].is_array()) {
        return {};
    }
    for (auto it = payload["messages"].rbegin(); it != payload["messages"].rend(); ++it) {
        if (it->value("role", "") == "user" && it->contains("content")
            && (*it)["content"].is_string()) {
            return QString::fromStdString((*it)["content"].get<std::string>());
        }
    }
    return {};
}

bool requestContainsUserMessage(const MockServer &server, int requestIndex, const QString &content)
{
    const json payload = json::parse(server.requestBody(requestIndex).toStdString());
    if (!payload.contains("messages") || !payload["messages"].is_array()) {
        return false;
    }
    for (const auto &message : payload["messages"]) {
        if (message.value("role", "") == "user" && message.contains("content")
            && message["content"].is_string()
            && message["content"].get<std::string>() == content.toStdString()) {
            return true;
        }
    }
    return false;
}

bool requestAdvertisesTool(const MockServer &server, int requestIndex, const QString &name)
{
    const json payload = json::parse(server.requestBody(requestIndex).toStdString());
    if (!payload.contains("tools") || !payload["tools"].is_array()) {
        return false;
    }
    for (const auto &tool : payload["tools"]) {
        if (tool.contains("function")
            && tool["function"].value("name", std::string()) == name.toStdString()) {
            return true;
        }
    }
    return false;
}

bool toolBatchHasExactlyOneResultPerCall(const json &messages, json::size_type batchStart)
{
    if (!messages.is_array() || batchStart >= messages.size()) {
        return false;
    }
    const json &assistant = messages.at(batchStart);
    if (assistant.value("role", std::string()) != "assistant" || !assistant.contains("tool_calls")
        || !assistant["tool_calls"].is_array()) {
        return false;
    }

    QHash<QString, int> expected;
    for (const json &toolCall : assistant["tool_calls"]) {
        if (!toolCall.contains("id") || !toolCall["id"].is_string()) {
            return false;
        }
        const QString id = QString::fromStdString(toolCall["id"].get<std::string>());
        if (expected.contains(id)) {
            return false;
        }
        expected.insert(id, 1);
    }

    QHash<QString, int> actual;
    for (json::size_type index = batchStart + 1; index < messages.size(); ++index) {
        const json &message = messages.at(index);
        if (message.value("role", std::string()) != "tool") {
            continue;
        }
        if (!message.contains("tool_call_id") || !message["tool_call_id"].is_string()) {
            return false;
        }
        const QString id = QString::fromStdString(message["tool_call_id"].get<std::string>());
        actual[id]++;
    }
    return !expected.isEmpty() && actual == expected;
}

json toolCallResponse(const QString &name)
{
    return {
        {"choices",
         json::array(
             {{{"message",
                {{"role", "assistant"},
                 {"content", nullptr},
                 {"tool_calls",
                  json::array(
                      {{{"id", "call_0"},
                        {"type", "function"},
                        {"function", {{"name", name.toStdString()}, {"arguments", "{}"}}}}})}}}}})},
    };
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

class AbortCountingTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("abort_counter"); }
    QString getDescription() const override { return QStringLiteral("Counts abort requests"); }
    json    getParametersSchema() const override { return json::object(); }
    QString execute(const json &) override { return {}; }
    void    abort() override { ++abortCount_; }

    int abortCount() const { return abortCount_; }

private:
    int abortCount_ = 0;
};

class BlockingTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("blocking_tool"); }
    QString getDescription() const override { return QStringLiteral("Waits for cancellation"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }

    QString execute(const json &) override
    {
        ++executeCount_;
        QPointer<QSocToolCallContext> context(currentCallContext());
        if (context.isNull()) {
            return QStringLiteral("missing context");
        }

        QEventLoop loop;
        QTimer     timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
            timedOut_ = true;
            loop.quit();
        });
        QObject::connect(
            context,
            &QSocToolCallContext::cancellationRequested,
            &loop,
            [&]() {
                cancelled_ = true;
                loop.quit();
            },
            Qt::DirectConnection);
        if (entered_) {
            entered_();
        }
        if (context->isCancellationRequested()) {
            cancelled_ = true;
        } else {
            timeout.start(2000);
            loop.exec();
        }
        return cancelled_ ? QStringLiteral("cancelled") : QStringLiteral("timed out");
    }

    void setEntered(std::function<void()> entered) { entered_ = std::move(entered); }
    int  executeCount() const { return executeCount_; }
    bool cancelled() const { return cancelled_; }
    bool timedOut() const { return timedOut_; }

private:
    std::function<void()> entered_;
    int                   executeCount_ = 0;
    bool                  cancelled_    = false;
    bool                  timedOut_     = false;
};

class SideEffectTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("side_effect_tool"); }
    QString getDescription() const override { return QStringLiteral("Records one effect"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        ++executeCount_;
        return QStringLiteral("effect recorded");
    }

    int executeCount() const { return executeCount_; }

private:
    int executeCount_ = 0;
};

class AttachmentTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("attachment_tool"); }
    QString getDescription() const override { return QStringLiteral("Returns a runtime image"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(qRgba(0x32, 0x58, 0x6d, 0xff));
        QByteArray bytes;
        QBuffer    buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            return QStringLiteral("image generation failed");
        }
        const json payload = {
            {"mime", "image/png"},
            {"data", bytes.toBase64().toStdString()},
            {"source_url", "runtime://image"},
            {"width", image.width()},
            {"height", image.height()},
            {"byte_size", bytes.size()},
            {"est_tokens", 1},
            {"resized", false},
        };
        return QStringLiteral("runtime image\n")
               + QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen())
               + QString::fromStdString(payload.dump())
               + QString::fromLatin1(QSocImageAttach::attachmentMarkerClose());
    }
};

class PolicyTool final : public QSocTool
{
public:
    explicit PolicyTool(bool readOnly)
        : readOnly_(readOnly)
    {}

    QString getName() const override { return QStringLiteral("policy_probe"); }
    QString getDescription() const override { return QStringLiteral("Tests registry policy"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        ++executeCount_;
        return QStringLiteral("ok");
    }
    bool isReadOnly() const override { return readOnly_; }
    int  executeCount() const { return executeCount_; }

private:
    bool readOnly_     = false;
    int  executeCount_ = 0;
};

class DefinitionCallbackTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("definition_probe"); }
    QString getDescription() const override
    {
        ++definitionCount_;
        if (definitionCallback_) {
            definitionCallback_(definitionCount_);
        }
        return QStringLiteral("Observes definition reads");
    }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return {}; }

    void setDefinitionCallback(std::function<void(int)> callback)
    {
        definitionCallback_ = std::move(callback);
    }
    int definitionCount() const { return definitionCount_; }

private:
    mutable std::function<void(int)> definitionCallback_;
    mutable int                      definitionCount_ = 0;
};

void configureService(QLLMService &service, const MockServer &server)
{
    LLMEndpoint endpoint;
    endpoint.name    = QStringLiteral("backoff-test");
    endpoint.url     = server.url();
    endpoint.model   = QStringLiteral("test-model");
    endpoint.timeout = 10000;
    service.addEndpoint(endpoint);
}

QSocAgentConfig testConfig()
{
    QSocAgentConfig config;
    config.verbose             = false;
    config.autoLoadMemory      = false;
    config.memoryRecallEnabled = false;
    config.maxIterations       = 2;
    config.maxRetries          = 1;
    return config;
}

json pruningHistory()
{
    json messages = json::array({{{"role", "user"}, {"content", "start"}}});
    for (int i = 0; i < 4; ++i) {
        const std::string id = "call_" + std::to_string(i);
        messages.push_back(
            {{"role", "assistant"},
             {"content", nullptr},
             {"tool_calls",
              json::array(
                  {{{"id", id},
                    {"type", "function"},
                    {"function", {{"name", "probe"}, {"arguments", "{}"}}}}})}});
        messages.push_back(
            {{"role", "tool"}, {"tool_call_id", id}, {"content", std::string(2000, 'x')}});
    }
    messages.push_back({{"role", "assistant"}, {"content", "done"}});
    return messages;
}

QSocAgentConfig pruningConfig()
{
    QSocAgentConfig config     = testConfig();
    config.verbose             = true;
    config.maxContextTokens    = 1000;
    config.pruneThreshold      = 0.01;
    config.pruneProtectTokens  = 1;
    config.pruneMinimumSavings = 1;
    config.compactThreshold    = 0.99;
    config.keepRecentMessages  = 100;
    return config;
}

class Test final : public QObject
{
    Q_OBJECT

private slots:
    void persistenceFailureStopsBeforeRequest()
    {
        MockServer server;
        QVERIFY(server.listen());
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry                   registry;
        QSocAgent                          agent(nullptr, &service, &registry, testConfig());
        QSignalSpy                         errors(&agent, &QSocAgent::runError);
        QList<QSocAgent::PersistencePoint> points;
        bool                               historyReady = false;

        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            points.append(point);
            if (point == QSocAgent::PersistencePoint::BeforeRequest) {
                const json messages = agent.getMessages();
                historyReady = messages.size() == 1
                               && messages[0].value("content", std::string()) == "persist me";
                return false;
            }
            return true;
        });

        agent.runStream(QStringLiteral("persist me"));

        QCOMPARE(errors.count(), 1);
        QCOMPARE(server.requestCount(), 0);
        QVERIFY(historyReady);
        const QList<QSocAgent::PersistencePoint> expected{
            QSocAgent::PersistencePoint::BeforeRequest,
            QSocAgent::PersistencePoint::Error,
        };
        QCOMPARE(points, expected);
    }

    void persistenceFailureStopsBeforeTool()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueToolCall(QStringLiteral("side_effect_tool"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        SideEffectTool   tool;
        registry.registerTool(&tool);
        QSocAgent                          agent(nullptr, &service, &registry, testConfig());
        QSignalSpy                         errors(&agent, &QSocAgent::runError);
        QList<QSocAgent::PersistencePoint> points;
        bool                               toolCheckpointReady = false;

        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point,
                                        const QString              &toolCallId) {
            points.append(point);
            if (point != QSocAgent::PersistencePoint::BeforeTool) {
                return true;
            }
            const json messages = agent.getMessages();
            toolCheckpointReady = toolCallId == QStringLiteral("call_0") && tool.executeCount() == 0
                                  && messages.size() == 2 && messages[1].contains("tool_calls");
            return false;
        });

        agent.runStream(QStringLiteral("use the tool"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 3000);

        QCOMPARE(tool.executeCount(), 0);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(toolCheckpointReady);
        const QList<QSocAgent::PersistencePoint> expected{
            QSocAgent::PersistencePoint::BeforeRequest,
            QSocAgent::PersistencePoint::BeforeTool,
            QSocAgent::PersistencePoint::Error,
        };
        QCOMPARE(points, expected);
    }

    void persistenceBarrierOrdersExternalActions()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueToolCall(QStringLiteral("side_effect_tool"));
        server.enqueueStream(QStringLiteral("done"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        SideEffectTool   tool;
        registry.registerTool(&tool);
        QSocAgent   agent(nullptr, &service, &registry, testConfig());
        QSignalSpy  completed(&agent, &QSocAgent::runComplete);
        QStringList checkpoints;
        bool        toolRanBeforeBarrier = false;

        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            QStringList roles;
            for (const json &message : agent.getMessages()) {
                roles.append(QString::fromStdString(message.value("role", std::string())));
            }
            QString name;
            switch (point) {
            case QSocAgent::PersistencePoint::BeforeRequest:
                name = QStringLiteral("request");
                break;
            case QSocAgent::PersistencePoint::BeforeTool:
                name = QStringLiteral("tool");
                break;
            case QSocAgent::PersistencePoint::Completed:
                name = QStringLiteral("completed");
                break;
            case QSocAgent::PersistencePoint::Error:
                name = QStringLiteral("error");
                break;
            case QSocAgent::PersistencePoint::Aborted:
                name = QStringLiteral("aborted");
                break;
            }
            checkpoints.append(name + QLatin1Char(':') + roles.join(QLatin1Char(',')));
            if (point == QSocAgent::PersistencePoint::BeforeTool) {
                toolRanBeforeBarrier = tool.executeCount() != 0;
            }
            return true;
        });

        agent.runStream(QStringLiteral("perform one action"));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);

        QCOMPARE(tool.executeCount(), 1);
        QVERIFY(!toolRanBeforeBarrier);
        const QStringList expected{
            QStringLiteral("request:user"),
            QStringLiteral("tool:user,assistant"),
            QStringLiteral("request:user,assistant,tool"),
            QStringLiteral("completed:user,assistant,tool,assistant"),
        };
        QCOMPARE(checkpoints, expected);
    }

    void resumeStreamUsesPersistedTail()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("continued"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        agent.setMessages(
            json::array(
                {{{"role", "user"}, {"content", "persisted request"}},
                 {{"role", "assistant"},
                  {"content", nullptr},
                  {"tool_calls",
                   json::array(
                       {{{"id", "call_0"},
                         {"type", "function"},
                         {"function", {{"name", "probe"}, {"arguments", "{}"}}}}})}},
                 {{"role", "tool"},
                  {"tool_call_id", "call_0"},
                  {"content", "completion uncertain"},
                  {"_qsoc_tool_state", "uncertain"}}}));

        agent.resumeStream();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);

        QCOMPARE(server.requestCount(), 1);
        const json request = json::parse(server.requestBody(0).toStdString());
        int        matches = 0;
        for (const json &message : request.at("messages")) {
            QVERIFY(!message.contains("_qsoc_tool_state"));
            if (message.contains("content") && message["content"].is_string()
                && message["content"].get<std::string>() == "persisted request") {
                ++matches;
            }
        }
        QCOMPARE(matches, 1);
        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(4));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[2].value("_qsoc_tool_state", std::string()), std::string("uncertain"));
        QCOMPARE(history[3].value("role", std::string()), std::string("assistant"));
    }

    void beforeRequestBarrierMayDeleteService()
    {
        MockServer server;
        QVERIFY(server.listen());
        auto *service = new QLLMService;
        configureService(*service, server);
        QPointer<QLLMService> serviceOwner(service);
        QSocToolRegistry      registry;
        QSocAgent             agent(nullptr, service, &registry, testConfig());
        QSignalSpy            errors(&agent, &QSocAgent::runError);

        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            if (point == QSocAgent::PersistencePoint::BeforeRequest) {
                delete service;
            }
            return true;
        });

        agent.runStream(QStringLiteral("delete service at checkpoint"));

        QVERIFY(serviceOwner.isNull());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(server.requestCount(), 0);
        QVERIFY(!agent.isRunning());
    }

    void beforeToolBarrierMayDeleteRegistry()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueToolCall(QStringLiteral("side_effect_tool"));
        QLLMService service;
        configureService(service, server);
        auto                      *registry = new QSocToolRegistry;
        QPointer<QSocToolRegistry> registryOwner(registry);
        SideEffectTool             tool;
        registry->registerTool(&tool);
        QSocAgent  agent(nullptr, &service, registry, testConfig());
        QSignalSpy errors(&agent, &QSocAgent::runError);

        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            if (point == QSocAgent::PersistencePoint::BeforeTool) {
                delete registry;
            }
            return true;
        });

        agent.runStream(QStringLiteral("delete registry at checkpoint"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 3000);

        QVERIFY(registryOwner.isNull());
        QCOMPARE(tool.executeCount(), 0);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(!agent.isRunning());
    }

    void terminalBarrierMayDeleteAgent()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("done"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry    registry;
        auto               *agent = new QSocAgent(nullptr, &service, &registry, testConfig());
        QPointer<QSocAgent> owner(agent);

        agent->setPersistenceBarrier([agent](QSocAgent::PersistencePoint point, const QString &) {
            if (point == QSocAgent::PersistencePoint::Completed) {
                delete agent;
            }
            return true;
        });

        agent->runStream(QStringLiteral("delete agent at checkpoint"));
        QTRY_VERIFY_WITH_TIMEOUT(owner.isNull(), 3000);
        QCOMPARE(server.requestCount(), 1);
    }

    void failedCompletedBarrierSkipsStopHook()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("done"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QTemporaryDir    tempDir(QDir::tempPath() + QStringLiteral("/qsoc_hook_XXXXXX"));
        QVERIFY(tempDir.isValid());
        const QString marker = tempDir.filePath(QStringLiteral("stop-ran"));

        HookCommandConfig command;
        command.command = QStringLiteral("printf hook > %1").arg(marker);
        HookMatcherConfig matcher;
        matcher.matcher = QStringLiteral("*");
        matcher.commands.append(command);
        QSocHookConfig hookConfig;
        hookConfig.byEvent[QSocHookEvent::Stop].append(matcher);
        QSocHookManager hooks;
        hooks.setConfig(hookConfig);
        agent.setHookManager(&hooks);
        agent.setPersistenceBarrier([](QSocAgent::PersistencePoint point, const QString &) {
            return point != QSocAgent::PersistencePoint::Completed;
        });

        agent.runStream(QStringLiteral("fail terminal persistence"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 3000);

        QCOMPARE(completed.count(), 0);
        QCOMPARE(aborted.count(), 0);
        QVERIFY(!QFile::exists(marker));
    }

    void stopHookAbortPersistsLatestOutcome()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("done"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);

        HookCommandConfig command;
        command.command    = QStringLiteral("sleep 0.05");
        command.timeoutSec = 1;
        HookMatcherConfig matcher;
        matcher.matcher = QStringLiteral("*");
        matcher.commands.append(command);
        QSocHookConfig hookConfig;
        hookConfig.byEvent[QSocHookEvent::Stop].append(matcher);
        QSocHookManager hooks;
        hooks.setConfig(hookConfig);
        agent.setHookManager(&hooks);
        QList<QSocAgent::PersistencePoint> points;
        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            points.append(point);
            if (point == QSocAgent::PersistencePoint::Completed) {
                QTimer::singleShot(0, &agent, &QSocAgent::abort);
            }
            return true;
        });

        agent.runStream(QStringLiteral("abort from stop hook window"));
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 3000);

        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        const QList<QSocAgent::PersistencePoint> expected{
            QSocAgent::PersistencePoint::BeforeRequest,
            QSocAgent::PersistencePoint::Completed,
            QSocAgent::PersistencePoint::Aborted,
        };
        QCOMPARE(points, expected);
    }

    void restoredHistorySkipsSessionStartHook()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("continued"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QTemporaryDir    tempDir(QDir::tempPath() + QStringLiteral("/qsoc_hook_XXXXXX"));
        QVERIFY(tempDir.isValid());
        const QString marker = tempDir.filePath(QStringLiteral("session-start-ran"));

        HookCommandConfig command;
        command.command = QStringLiteral("printf hook > %1").arg(marker);
        HookMatcherConfig matcher;
        matcher.matcher = QStringLiteral("*");
        matcher.commands.append(command);
        QSocHookConfig hookConfig;
        hookConfig.byEvent[QSocHookEvent::SessionStart].append(matcher);
        QSocHookManager hooks;
        hooks.setConfig(hookConfig);
        agent.setHookManager(&hooks);
        agent.setMessages(json::array({{{"role", "user"}, {"content", "persisted"}}}));

        agent.resumeStream();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);

        QVERIFY(!QFile::exists(marker));
        QCOMPARE(server.requestCount(), 1);
    }

    void restoredContinuationSkipsUserHooks()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("continued"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QTemporaryDir    tempDir(QDir::tempPath() + QStringLiteral("/qsoc_hook_XXXXXX"));
        QVERIFY(tempDir.isValid());
        const QString sessionMarker = tempDir.filePath(QStringLiteral("session-start-ran"));
        const QString promptMarker  = tempDir.filePath(QStringLiteral("prompt-ran"));

        HookMatcherConfig sessionMatcher;
        sessionMatcher.matcher = QStringLiteral("*");
        HookCommandConfig sessionCommand;
        sessionCommand.command = QStringLiteral("printf s >> %1").arg(sessionMarker);
        sessionMatcher.commands.append(sessionCommand);
        HookMatcherConfig promptMatcher;
        promptMatcher.matcher = QStringLiteral("*");
        HookCommandConfig promptCommand;
        promptCommand.command = QStringLiteral("printf p >> %1").arg(promptMarker);
        promptMatcher.commands.append(promptCommand);
        QSocHookConfig hookConfig;
        hookConfig.byEvent[QSocHookEvent::SessionStart].append(sessionMatcher);
        hookConfig.byEvent[QSocHookEvent::UserPromptSubmit].append(promptMatcher);
        QSocHookManager hooks;
        hooks.setConfig(hookConfig);
        agent.setHookManager(&hooks);
        agent.setMessages(
            json::array({{{"role", "user"}, {"content", "continue the restored goal"}}}));

        agent.resumeStream();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);

        QVERIFY(!QFile::exists(sessionMarker));
        QVERIFY(!QFile::exists(promptMarker));
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("continue the restored goal"));
    }

    void compactPruneObserverMayDeleteAgent()
    {
        QSocToolRegistry    registry;
        auto               *agent = new QSocAgent(nullptr, nullptr, &registry, pruningConfig());
        QPointer<QSocAgent> owner(agent);
        agent->setMessages(pruningHistory());
        connect(agent, &QSocAgent::verboseOutput, &registry, [agent](const QString &message) {
            if (message.startsWith(QStringLiteral("[Layer 1 Prune:"))) {
                delete agent;
            }
        });

        QCOMPARE(agent->compact(), 0);
        QVERIFY(owner.isNull());
    }

    void automaticPruneObserverMayDeleteAgent()
    {
        QSocToolRegistry    registry;
        auto               *agent = new QSocAgent(nullptr, nullptr, &registry, pruningConfig());
        QPointer<QSocAgent> owner(agent);
        agent->setMessages(pruningHistory());
        connect(agent, &QSocAgent::verboseOutput, &registry, [agent](const QString &message) {
            if (message.startsWith(QStringLiteral("[Layer 1 Prune:"))) {
                delete agent;
            }
        });

        QCOMPARE(agent->run(QStringLiteral("continue")), QStringLiteral("[Agent aborted]"));
        QVERIFY(owner.isNull());
    }

    void abortDuringBackoff_data()
    {
        QTest::addColumn<int>("statusCode");
        QTest::addColumn<int>("staleTimerWaitMs");

        QTest::newRow("transient") << 500 << 1250;
        QTest::newRow("rate-limit") << 503 << 3500;
    }

    void abortDuringBackoff()
    {
        QFETCH(int, statusCode);
        QFETCH(int, staleTimerWaitMs);

        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(statusCode);
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       retrying(&agent, &QSocAgent::retrying);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(retrying.count(), 1, 5000);

        QElapsedTimer elapsed;
        elapsed.start();
        agent.abort();
        agent.abort();
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 1500);
        QVERIFY2(elapsed.elapsed() < 1000, "abort waited for the retry timer");
        QVERIFY(!agent.isRunning());
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);

        QTest::qWait(staleTimerWaitMs);
        QVERIFY(!agent.isRunning());
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

    void retryingObserverMayAbort()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);
        QElapsedTimer    elapsed;

        connect(&agent, &QSocAgent::retrying, &agent, [&]() {
            elapsed.start();
            agent.abort();
        });

        agent.runStream(QStringLiteral("observer abort"));
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 1500);
        QVERIFY(elapsed.isValid());
        QVERIFY2(elapsed.elapsed() < 1000, "retrying observer abort was delayed");
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);

        QTest::qWait(3500);
        QVERIFY(!agent.isRunning());
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(aborted.count(), 1);
    }

    void retryingObserverMayDeleteAgent()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry    registry;
        auto               *agent = new QSocAgent(nullptr, &service, &registry, testConfig());
        QPointer<QSocAgent> owner(agent);
        QSignalSpy          retrying(agent, &QSocAgent::retrying);

        connect(agent, &QSocAgent::retrying, &service, [agent]() { delete agent; });

        agent->runStream(QStringLiteral("observer deletion"));
        QTRY_VERIFY_WITH_TIMEOUT(owner.isNull(), 5000);
        QCOMPARE(retrying.count(), 1);
        QCOMPARE(server.requestCount(), 1);
    }

    void queuedRequestContinuesWithoutStaleRetry()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       retrying(&agent, &QSocAgent::retrying);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(retrying.count(), 1, 5000);
        agent.queueRequest(QStringLiteral("queued request"));

        QElapsedTimer elapsed;
        elapsed.start();
        agent.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 1500);
        QVERIFY2(elapsed.elapsed() < 1000, "queued request waited for the retry timer");
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("first request"));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);

        QTest::qWait(3500);
        QVERIFY(!agent.isRunning());
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(completed.count(), 1);
    }

    void softQueueBeforeStreamPostRestartsPreparation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.verbose          = true;
        config.maxTurnsOverride = 1;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy errors(&agent, &QSocAgent::runError);
        bool       injected             = false;
        bool       queuedWhilePreparing = false;

        connect(&agent, &QSocAgent::verboseOutput, &agent, [&](const QString &message) {
            if (!injected && message.startsWith(QStringLiteral("[Iteration 1 |"))) {
                injected = true;
                agent.runStream(QStringLiteral("queued request"));
                queuedWhilePreparing = agent.pendingRequestCount() == 1;
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + aborted.count() + errors.count() >= 1, 5000);
        QCOMPARE(completed.count() + aborted.count() + errors.count(), 1);
        QVERIFY(injected);
        QVERIFY(queuedWhilePreparing);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("queued request"));
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

    void softQueueBeforeSynchronousPostRestartsPreparation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueCompletion(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.verbose          = true;
        config.maxTurnsOverride = 1;
        QSocAgent agent(nullptr, &service, &registry, config);
        bool      injected = false;
        bool      queued   = false;

        connect(&agent, &QSocAgent::verboseOutput, &agent, [&](const QString &message) {
            if (!injected && message.startsWith(QStringLiteral("[Iteration 1 |"))) {
                injected = true;
                queued   = agent.queueRequest(QStringLiteral("queued request"));
                agent.abort();
            }
        });

        const QString result = agent.run(QStringLiteral("first request"));
        QVERIFY(injected);
        QVERIFY(queued);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("queued request"));
        QCOMPARE(result, QStringLiteral("queued complete"));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void softQueueDuringSynchronousToolSnapshotRestartsPreparation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueCompletion(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry       registry;
        DefinitionCallbackTool tool;
        registry.registerTool(&tool);
        QSocAgentConfig config  = testConfig();
        config.maxTurnsOverride = 1;
        QSocAgent agent(nullptr, &service, &registry, config);
        bool      injected = false;
        bool      queued   = false;

        tool.setDefinitionCallback([&](int definitionCount) {
            if (!injected && definitionCount == 2) {
                injected = true;
                queued   = agent.queueRequest(QStringLiteral("queued request"));
                agent.abort();
            }
        });

        const QString result = agent.run(QStringLiteral("first request"));
        QVERIFY(injected);
        QVERIFY(queued);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("queued request"));
        QCOMPARE(result, QStringLiteral("queued complete"));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(tool.definitionCount(), 4);
    }

    void softQueueDuringStreamCompactionRequestRestartsPreparation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueCompletion(QStringLiteral("compacted"));
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config     = testConfig();
        config.maxContextTokens     = 10000;
        config.reservedOutputTokens = 0;
        config.pruneThreshold       = 0.99;
        config.compactThreshold     = 0.01;
        config.keepRecentMessages   = 2;
        config.maxTurnsOverride     = 1;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy errors(&agent, &QSocAgent::runError);
        bool       injected = false;

        json initial = json::array();
        for (int index = 0; index < 8; ++index) {
            const QString role = index % 2 == 0 ? QStringLiteral("user")
                                                : QStringLiteral("assistant");
            initial.push_back(
                {{"role", role.toStdString()},
                 {"content", QString(800, QLatin1Char('a' + index)).toStdString()}});
        }
        agent.setMessages(initial);
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                injected = true;
                agent.runStream(QStringLiteral("queued request"));
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QVERIFY(injected);
        server.setRequestObserver({});
        QVERIFY2(server.releaseHeldRequest(), "cancelled compaction reply was not drained");
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + aborted.count() + errors.count() >= 1, 5000);
        QCOMPARE(completed.count() + aborted.count() + errors.count(), 1);
        QCOMPARE(server.requestCount(), 3);
        const json firstCompaction  = json::parse(server.requestBody(0).toStdString());
        const json secondCompaction = json::parse(server.requestBody(1).toStdString());
        const json mainRequest      = json::parse(server.requestBody(2).toStdString());
        QVERIFY(!firstCompaction.value("stream", true));
        QVERIFY(!secondCompaction.value("stream", true));
        QVERIFY(mainRequest.value("stream", false));
        QVERIFY(requestContainsUserMessage(server, 2, QStringLiteral("first request")));
        QVERIFY(requestContainsUserMessage(server, 2, QStringLiteral("queued request")));
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

    void softQueueAfterSynchronousResponseRestartsIteration()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueCompletion(QStringLiteral("first complete"));
        server.enqueueCompletion(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.verbose          = true;
        config.maxTurnsOverride = 2;
        QSocAgent   agent(nullptr, &service, &registry, config);
        QStringList iterations;
        bool        injected = false;
        bool        queued   = false;

        connect(&agent, &QSocAgent::verboseOutput, &agent, [&](const QString &message) {
            if (message.startsWith(QStringLiteral("[Iteration "))) {
                iterations.append(message);
            }
            if (!injected && message == QStringLiteral("[Assistant]: first complete")) {
                injected = true;
                queued   = agent.queueRequest(QStringLiteral("queued request"));
                agent.abort();
            }
        });

        const QString result = agent.run(QStringLiteral("first request"));
        QVERIFY(injected);
        QVERIFY(queued);
        QCOMPARE(server.requestCount(), 2);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QVERIFY(!requestContainsUserMessage(server, 0, QStringLiteral("queued request")));
        QVERIFY(requestContainsUserMessage(server, 1, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(result, QStringLiteral("queued complete"));
        QCOMPARE(iterations.size(), 2);
        QVERIFY(iterations.at(0).startsWith(QStringLiteral("[Iteration 1 |")));
        QVERIFY(iterations.at(1).startsWith(QStringLiteral("[Iteration 2 |")));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void softQueueDuringSynchronousPostContinuesAfterDrain()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueCompletion(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.maxTurnsOverride = 2;
        QSocAgent agent(nullptr, &service, &registry, config);
        bool      injected = false;
        bool      queued   = false;

        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                injected = true;
                queued   = agent.queueRequest(QStringLiteral("queued request"));
                agent.abort();
            }
        });

        const QString result = agent.run(QStringLiteral("first request"));
        QVERIFY(injected);
        QVERIFY(queued);
        QCOMPARE(server.requestCount(), 2);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QVERIFY(!requestContainsUserMessage(server, 0, QStringLiteral("queued request")));
        QVERIFY(requestContainsUserMessage(server, 1, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(result, QStringLiteral("queued complete"));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));

        server.setRequestObserver({});
        QVERIFY2(server.releaseHeldRequest(), "cancelled sync reply was not drained");
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(server.requestCount(), 2);
    }

    void softQueueDuringSynchronousToolBatchPreservesFacts()
    {
        MockServer server;
        QVERIFY(server.listen());
        SideEffectTool effect;
        BlockingTool   pending;
        server.enqueueToolCompletion({effect.getName(), pending.getName()});
        server.enqueueCompletion(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        registry.registerTool(&effect);
        registry.registerTool(&pending);
        QSocAgentConfig config  = testConfig();
        config.maxTurnsOverride = 2;
        QSocAgent agent(nullptr, &service, &registry, config);
        bool      queued = false;

        connect(&agent, &QSocAgent::toolResult, &agent, [&](const QString &name, const QString &) {
            if (name == effect.getName() && !queued) {
                queued = agent.queueRequest(QStringLiteral("queued request"));
                agent.abort();
            }
        });

        const QString result = agent.run(QStringLiteral("tool request"));

        QVERIFY(queued);
        QCOMPARE(result, QStringLiteral("queued complete"));
        QCOMPARE(effect.executeCount(), 1);
        QCOMPARE(pending.executeCount(), 0);
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json messages = json::parse(server.requestBody(1).toStdString()).at("messages");
        const auto assistant
            = std::find_if(messages.begin(), messages.end(), [](const json &message) {
                  return message.value("role", std::string()) == "assistant"
                         && message.contains("tool_calls");
              });
        QVERIFY(assistant != messages.end());
        const auto batchStart = static_cast<json::size_type>(assistant - messages.begin());
        QVERIFY(toolBatchHasExactlyOneResultPerCall(messages, batchStart));
        QCOMPARE(messages[batchStart + 1].value("tool_call_id", std::string()), std::string("call_0"));
        QCOMPARE(
            messages[batchStart + 1].value("content", std::string()),
            std::string("effect recorded"));
        QCOMPARE(messages[batchStart + 2].value("tool_call_id", std::string()), std::string("call_1"));
        QVERIFY(
            QString::fromStdString(messages[batchStart + 2].value("content", std::string()))
                .contains(QStringLiteral("not executed"), Qt::CaseInsensitive));
        QCOMPARE(
            messages[batchStart + 3].value("content", std::string()), std::string("queued request"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void softQueueDuringStreamToolSnapshotRestartsPreparation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry       registry;
        DefinitionCallbackTool tool;
        registry.registerTool(&tool);
        QSocAgentConfig config  = testConfig();
        config.maxTurnsOverride = 1;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy errors(&agent, &QSocAgent::runError);
        bool       injected = false;

        tool.setDefinitionCallback([&](int definitionCount) {
            if (!injected && definitionCount == 2) {
                injected = true;
                agent.runStream(QStringLiteral("queued request"));
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + aborted.count() + errors.count() >= 1, 5000);
        QCOMPARE(completed.count() + aborted.count() + errors.count(), 1);
        QVERIFY(injected);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("queued request"));
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(tool.definitionCount(), 4);
    }

    void softQueueDuringStreamPostContinuesNextIteration()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.verbose          = true;
        config.maxTurnsOverride = 2;
        QSocAgent   agent(nullptr, &service, &registry, config);
        QSignalSpy  completed(&agent, &QSocAgent::runComplete);
        QSignalSpy  aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy  errors(&agent, &QSocAgent::runError);
        QStringList iterations;

        connect(&agent, &QSocAgent::verboseOutput, &agent, [&](const QString &message) {
            if (message.startsWith(QStringLiteral("[Iteration "))) {
                iterations.append(message);
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 3000);
        agent.runStream(QStringLiteral("queued request"));
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + aborted.count() + errors.count() >= 1, 5000);
        QCOMPARE(completed.count() + aborted.count() + errors.count(), 1);
        QCOMPARE(server.requestCount(), 2);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QVERIFY(!requestContainsUserMessage(server, 0, QStringLiteral("queued request")));
        QVERIFY(requestContainsUserMessage(server, 1, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(iterations.size(), 2);
        QVERIFY(iterations.at(0).startsWith(QStringLiteral("[Iteration 1 |")));
        QVERIFY(iterations.at(1).startsWith(QStringLiteral("[Iteration 2 |")));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

    void softQueueAfterStreamResponseRestartsIteration()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueStream(QStringLiteral("first complete"));
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config = testConfig();
        config.verbose          = true;
        config.maxTurnsOverride = 2;
        QSocAgent   agent(nullptr, &service, &registry, config);
        QSignalSpy  completed(&agent, &QSocAgent::runComplete);
        QSignalSpy  aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy  errors(&agent, &QSocAgent::runError);
        QStringList iterations;
        bool        injected = false;

        connect(&agent, &QSocAgent::verboseOutput, &agent, [&](const QString &message) {
            if (message.startsWith(QStringLiteral("[Iteration "))) {
                iterations.append(message);
            }
            if (!injected && message == QStringLiteral("[Assistant]: first complete")) {
                injected = true;
                agent.runStream(QStringLiteral("queued request"));
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + aborted.count() + errors.count() >= 1, 5000);
        QCOMPARE(completed.count() + aborted.count() + errors.count(), 1);
        QVERIFY(injected);
        QCOMPARE(server.requestCount(), 2);
        QVERIFY(requestContainsUserMessage(server, 0, QStringLiteral("first request")));
        QVERIFY(!requestContainsUserMessage(server, 0, QStringLiteral("queued request")));
        QVERIFY(requestContainsUserMessage(server, 1, QStringLiteral("first request")));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(iterations.size(), 2);
        QVERIFY(iterations.at(0).startsWith(QStringLiteral("[Iteration 1 |")));
        QVERIFY(iterations.at(1).startsWith(QStringLiteral("[Iteration 2 |")));
        QVERIFY(!agent.isRunning());
        QCOMPARE(agent.pendingRequestCount(), 0);

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(3));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("user"));
        QCOMPARE(history[1].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[2].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("content", std::string()), std::string("queued complete"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

    void hardStopRejectsQueueAndClaimsOneTerminalPerRun()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        QVERIFY(agent.queueRequest(QStringLiteral("discarded request")));
        agent.abortAndDiscardPendingRequests();
        agent.abortAndDiscardPendingRequests();

        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(agent.pendingRequestCount(), 0);
        QVERIFY(!agent.queueRequest(QStringLiteral("late request")));
        QVERIFY(!agent.queueTaskNotification(QStringLiteral("late notification")));
        const json stoppedHistory = agent.getMessages();
        QCOMPARE(stoppedHistory.size(), size_t(1));
        QCOMPARE(stoppedHistory[0].value("role", std::string()), std::string("user"));
        QCOMPARE(stoppedHistory[0].value("content", std::string()), std::string("first request"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(agent.getMessages() == stoppedHistory);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(!server.requestBody(0).contains("discarded request"));

        agent.runStream(QStringLiteral("second request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 5000);
        QVERIFY(agent.queueRequest(QStringLiteral("second queued request")));
        QVERIFY(agent.queueTaskNotification(QStringLiteral("second notification")));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(agent.isRunning());
        QCOMPARE(aborted.count(), 1);
        agent.abortAndDiscardPendingRequests();
        agent.abortAndDiscardPendingRequests();

        QCOMPARE(aborted.count(), 2);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(agent.pendingRequestCount(), 0);
        QVERIFY(!agent.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(aborted.count(), 2);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(agent.pendingRequestCount(), 0);
        const json finalHistory = agent.getMessages();
        QCOMPARE(finalHistory.size(), size_t(2));
        QCOMPARE(finalHistory[0].value("role", std::string()), std::string("user"));
        QCOMPARE(finalHistory[0].value("content", std::string()), std::string("first request"));
        QCOMPARE(finalHistory[1].value("role", std::string()), std::string("user"));
        QCOMPARE(finalHistory[1].value("content", std::string()), std::string("second request"));
    }

    void destroyedServiceTerminatesCurrentStreamOnce()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        auto *service = new QLLMService;
        configureService(*service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, service, &registry, testConfig());
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("service lifetime"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        delete service;

        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 1000);
        QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("LLM service destroyed"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(completed.count(), 0);
        QVERIFY(!agent.isRunning());
        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(1));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("service lifetime"));

        QTest::qWait(50);
        QCOMPARE(errors.count(), 1);
        QVERIFY(agent.getMessages() == history);
    }

    void lateSignalsFromPreviousServiceDoNotTouchReplacementRun()
    {
        MockServer oldServer;
        MockServer newServer;
        QVERIFY(oldServer.listen());
        QVERIFY(newServer.listen());
        oldServer.enqueueHeldRequest();
        newServer.enqueueHeldRequest();
        QLLMService oldService;
        QLLMService newService;
        configureService(oldService, oldServer);
        configureService(newService, newServer);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &oldService, &registry, testConfig());
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("old run"));
        QTRY_COMPARE_WITH_TIMEOUT(oldServer.requestCount(), 1, 5000);
        agent.abortAndDiscardPendingRequests();
        QCOMPARE(aborted.count(), 1);

        agent.setLLMService(&newService);
        agent.runStream(QStringLiteral("replacement run"));
        QTRY_COMPARE_WITH_TIMEOUT(newServer.requestCount(), 1, 5000);
        const json replacementHistory = agent.getMessages();

        oldService.streamChunk(QStringLiteral("stale content"));
        oldService.streamReasoningChunk(QStringLiteral("stale reasoning"));
        oldService.streamError(QStringLiteral("[HTTP 401] stale error"));
        oldService.streamComplete(
            json{
                {"choices",
                 json::array(
                     {{{"message", {{"role", "assistant"}, {"content", "stale completion"}}}}})}});
        QCoreApplication::processEvents();

        QVERIFY(agent.isRunning());
        QVERIFY(agent.getMessages() == replacementHistory);
        QCOMPARE(oldServer.requestCount(), 1);
        QCOMPARE(newServer.requestCount(), 1);
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);

        agent.abortAndDiscardPendingRequests();
        QCOMPARE(aborted.count(), 2);
        QVERIFY(!agent.isRunning());
    }

    void agentToolCancellationTargetsOnlyCurrentCall()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        QFile configFile(configDir.filePath(QStringLiteral("qsoc.yml")));
        QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray yaml = QByteArrayLiteral(
                                    "llm:\n"
                                    "  model: test-model\n"
                                    "  models:\n"
                                    "    test-model:\n"
                                    "      url: ")
                                + server.url().toString().toUtf8()
                                + QByteArrayLiteral("\n      timeout: 10000\n");
        QCOMPARE(configFile.write(yaml), yaml.size());
        configFile.close();

        const bool       hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
        const bool       hadXdgHome  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        const QByteArray oldQsocHome = qgetenv("QSOC_HOME");
        const QByteArray oldXdgHome  = qgetenv("XDG_CONFIG_HOME");
        qputenv("QSOC_HOME", configDir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", configDir.path().toUtf8());
        const auto  restoreEnvironment = qScopeGuard([=]() {
            hadQsocHome ? qputenv("QSOC_HOME", oldQsocHome) : qunsetenv("QSOC_HOME");
            hadXdgHome ? qputenv("XDG_CONFIG_HOME", oldXdgHome) : qunsetenv("XDG_CONFIG_HOME");
        });
        QSocConfig  serviceConfig;
        QLLMService service(nullptr, &serviceConfig);
        QCOMPARE(service.endpointCount(), 1);
        QSocAgentDefinitionRegistry definitions;
        definitions.registerBuiltins();
        QSocSubAgentTaskSource tasks;
        QSocToolRegistry       registry;
        QSocAgentConfig        config = testConfig();
        config.autoBackgroundMs       = 0;
        QSocToolAgent tool(nullptr, &service, &registry, config, &definitions, &tasks);
        registry.registerTool(&tool);

        const QString unrelatedId = tasks.registerRun(
            QStringLiteral("unrelated"),
            QStringLiteral("general-purpose"),
            new QSocAgent(nullptr, nullptr, nullptr, testConfig()));
        QObject owner;
        bool    cancellationRequested = false;
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                cancellationRequested = true;
                registry.abortCalls(&owner);
            }
        });

        QElapsedTimer elapsed;
        elapsed.start();
        const QString raw = registry.executeTool(
            QStringLiteral("agent"),
            json{
                {"subagent_type", "general-purpose"},
                {"description", "cancel target"},
                {"prompt", "wait for cancellation"},
                {"run_in_background", false}},
            &owner);
        const json response = json::parse(raw.toStdString());

        QVERIFY(cancellationRequested);
        QVERIFY2(elapsed.elapsed() < 2000, "foreground cancellation did not terminate promptly");
        QCOMPARE(response.value("status", std::string()), std::string("ok"));
        QVERIFY(response.contains("task_id"));
        const QString targetId = QString::fromStdString(response["task_id"].get<std::string>());
        QVERIFY(targetId != unrelatedId);
        QSocTask::Row targetRow;
        QSocTask::Row unrelatedRow;
        QVERIFY(tasks.findRow(targetId, &targetRow));
        QVERIFY(tasks.findRow(unrelatedId, &unrelatedRow));
        QCOMPARE(targetRow.status, QSocTask::Status::Failed);
        QCOMPARE(unrelatedRow.status, QSocTask::Status::Pending);
        QVERIFY(unrelatedRow.canKill);
        QCOMPARE(tasks.countRunning(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void partialStreamAbortPreservesChunkOrder()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       chunks(&agent, &QSocAgent::contentChunk);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("partial request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        service.streamChunk(QStringLiteral("first "));
        service.streamChunk(QStringLiteral("second"));

        QCOMPARE(chunks.count(), 2);
        QCOMPARE(chunks.at(0).at(0).toString(), QStringLiteral("first "));
        QCOMPARE(chunks.at(1).at(0).toString(), QStringLiteral("second"));
        agent.abortAndDiscardPendingRequests();

        QCOMPARE(aborted.count(), 1);
        QCOMPARE(aborted.at(0).at(0).toString(), QStringLiteral("first second"));
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(1));
        QCOMPARE(history[0].value("content", std::string()), std::string("partial request"));
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void hardStopDuringBlockingToolPreservesBatch()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        BlockingTool     tool;
        registry.registerTool(&tool);
        QSocAgent  agent(nullptr, &service, &registry, testConfig());
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);
        bool       replacementStarted = false;

        tool.setEntered([&]() {
            QTimer::singleShot(0, &agent, [&]() { agent.abortAndDiscardPendingRequests(); });
        });
        connect(&agent, &QSocAgent::runAborted, &agent, [&]() {
            if (!replacementStarted) {
                replacementStarted = true;
                agent.runStream(QStringLiteral("replacement request"));
            }
        });

        agent.runStream(QStringLiteral("tool request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        service.streamComplete(toolCallResponse(tool.getName()));

        QCOMPARE(tool.executeCount(), 1);
        QVERIFY(tool.cancelled());
        QVERIFY(!tool.timedOut());
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 5000);
        QVERIFY(agent.isRunning());
        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(4));
        QCOMPARE(history[0].value("content", std::string()), std::string("tool request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("tool_call_id", std::string()), std::string("call_0"));
        QVERIFY(toolBatchHasExactlyOneResultPerCall(history, 1));
        const QString interrupted = QString::fromStdString(
            history[2].value("content", std::string()));
        QVERIFY(interrupted.contains(QStringLiteral("Completion is uncertain")));
        QVERIFY(interrupted.contains(QStringLiteral("side effects may have occurred")));
        QVERIFY(interrupted.contains(QStringLiteral("Verify current state before retrying")));
        QCOMPARE(history[2].value("_qsoc_tool_state", std::string()), std::string("uncertain"));
        QVERIFY(!interrupted.contains(QStringLiteral("Tool reported")));
        QVERIFY(!interrupted.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));
        QCOMPARE(history[3].value("content", std::string()), std::string("replacement request"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(agent.getMessages() == history);

        agent.abortAndDiscardPendingRequests();
        QCOMPARE(aborted.count(), 2);
        QVERIFY(!agent.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void queuedRunDuringToolCancellationContinues()
    {
        MockServer server;
        QVERIFY(server.listen());
        BlockingTool tool;
        server.enqueueToolCall(tool.getName());
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QSocAgent  agent(nullptr, &service, &registry, testConfig());
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);

        tool.setEntered([&]() { agent.runStream(QStringLiteral("queued request")); });

        agent.runStream(QStringLiteral("tool request"));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);

        QCOMPARE(tool.executeCount(), 1);
        QVERIFY(tool.cancelled());
        QVERIFY(!tool.timedOut());
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("queued request"));
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("queued complete"));
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QVERIFY(!agent.isRunning());

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(5));
        QCOMPARE(history[0].value("content", std::string()), std::string("tool request"));
        QCOMPARE(history[1].value("role", std::string()), std::string("assistant"));
        QCOMPARE(history[2].value("tool_call_id", std::string()), std::string("call_0"));
        QVERIFY(toolBatchHasExactlyOneResultPerCall(history, 1));
        const QString interrupted = QString::fromStdString(
            history[2].value("content", std::string()));
        QVERIFY(interrupted.contains(QStringLiteral("Completion is uncertain")));
        QVERIFY(interrupted.contains(QStringLiteral("side effects may have occurred")));
        QVERIFY(interrupted.contains(QStringLiteral("Tool reported: cancelled")));
        QVERIFY(interrupted.endsWith(QStringLiteral("Verify current state before retrying.")));
        QCOMPARE(history[2].value("_qsoc_tool_state", std::string()), std::string("uncertain"));
        QCOMPARE(history[3].value("content", std::string()), std::string("queued request"));
        QCOMPARE(history[4].value("content", std::string()), std::string("queued complete"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void queuedRunPreservesCompletedToolBatch()
    {
        MockServer server;
        QVERIFY(server.listen());
        SideEffectTool effect;
        BlockingTool   pending;
        server.enqueueToolCalls({effect.getName(), pending.getName()});
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        registry.registerTool(&effect);
        registry.registerTool(&pending);
        QSocAgent  agent(nullptr, &service, &registry, testConfig());
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);
        bool       queued = false;

        connect(&agent, &QSocAgent::toolResult, &agent, [&](const QString &name, const QString &) {
            if (name == effect.getName() && !queued) {
                queued = true;
                agent.runStream(QStringLiteral("queued request"));
            }
        });
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                return;
            }
            if (requestIndex > 1) {
                server.enqueueStream(QStringLiteral("queued complete"));
                return;
            }
            const json messages
                = json::parse(server.requestBody(requestIndex).toStdString()).at("messages");
            const auto assistant
                = std::find_if(messages.begin(), messages.end(), [](const json &message) {
                      return message.value("role", std::string()) == "assistant"
                             && message.contains("tool_calls");
                  });
            const bool closed = assistant != messages.end()
                                && toolBatchHasExactlyOneResultPerCall(
                                    messages,
                                    static_cast<json::size_type>(assistant - messages.begin()));
            if (closed) {
                server.enqueueStream(QStringLiteral("queued complete"));
            } else {
                server.enqueueToolCall(effect.getName());
            }
        });

        agent.runStream(QStringLiteral("tool request"));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);

        QCOMPARE(effect.executeCount(), 1);
        QCOMPARE(pending.executeCount(), 0);
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);

        const json payload  = json::parse(server.requestBody(1).toStdString());
        const json messages = payload.at("messages");
        const auto assistant
            = std::find_if(messages.begin(), messages.end(), [](const json &message) {
                  return message.value("role", std::string()) == "assistant"
                         && message.contains("tool_calls");
              });
        QVERIFY(assistant != messages.end());
        const auto batchStart = static_cast<json::size_type>(assistant - messages.begin());
        QVERIFY(messages.size() >= batchStart + 4);
        QVERIFY(toolBatchHasExactlyOneResultPerCall(messages, batchStart));
        QCOMPARE(messages[batchStart + 1].value("tool_call_id", std::string()), std::string("call_0"));
        QCOMPARE(
            messages[batchStart + 1].value("content", std::string()),
            std::string("effect recorded"));
        QCOMPARE(messages[batchStart + 2].value("tool_call_id", std::string()), std::string("call_1"));
        QVERIFY(
            QString::fromStdString(messages[batchStart + 2].value("content", std::string()))
                .contains(QStringLiteral("not executed"), Qt::CaseInsensitive));
        QVERIFY(!messages[batchStart + 2].contains("_qsoc_tool_state"));
        QCOMPARE(
            messages[batchStart + 3].value("content", std::string()), std::string("queued request"));
        server.setRequestObserver({});
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(!agent.isRunning());
        QCOMPARE(completed.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(errors.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void queuedRunClosesToolBatchBeforeAttachment()
    {
        MockServer server;
        QVERIFY(server.listen());
        AttachmentTool attachment;
        BlockingTool   pending;
        server.enqueueToolCalls({attachment.getName(), pending.getName()});
        server.enqueueStream(QStringLiteral("queued complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        registry.registerTool(&attachment);
        registry.registerTool(&pending);
        QSocAgent agent(nullptr, &service, &registry, testConfig());

        connect(&agent, &QSocAgent::toolResult, &agent, [&](const QString &name, const QString &) {
            if (name == attachment.getName()) {
                agent.runStream(QStringLiteral("queued request"));
            }
        });

        agent.runStream(QStringLiteral("tool request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 5000);

        const json messages = json::parse(server.requestBody(1).toStdString()).at("messages");
        const auto assistant
            = std::find_if(messages.begin(), messages.end(), [](const json &message) {
                  return message.value("role", std::string()) == "assistant"
                         && message.contains("tool_calls");
              });
        QVERIFY(assistant != messages.end());
        const auto batchStart = static_cast<json::size_type>(assistant - messages.begin());
        QVERIFY(messages.size() >= batchStart + 5);
        QVERIFY(toolBatchHasExactlyOneResultPerCall(messages, batchStart));
        QCOMPARE(messages[batchStart + 1].value("tool_call_id", std::string()), std::string("call_0"));
        QCOMPARE(messages[batchStart + 2].value("tool_call_id", std::string()), std::string("call_1"));
        QCOMPARE(messages[batchStart + 3].value("role", std::string()), std::string("user"));
        QVERIFY(messages[batchStart + 3].at("content").is_array());
        QCOMPARE(
            messages[batchStart + 4].value("content", std::string()), std::string("queued request"));

        agent.abortAndDiscardPendingRequests();
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void registryPolicyUsesRunSnapshot()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry oldRegistry;
        QSocToolRegistry newRegistry;
        PolicyTool       oldTool(true);
        PolicyTool       newTool(false);
        oldRegistry.registerTool(&oldTool);
        newRegistry.registerTool(&newTool);
        QSocAgentConfig config = testConfig();
        config.planMode        = true;
        QSocAgent  agent(nullptr, &service, &oldRegistry, config);
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);

        agent.runStream(QStringLiteral("old registry request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        QVERIFY(requestAdvertisesTool(server, 0, oldTool.getName()));
        agent.setToolRegistry(&newRegistry);
        const auto stopConnection = connect(
            &agent, &QSocAgent::toolResult, &agent, [&](const QString &name, const QString &) {
                if (name == oldTool.getName()) {
                    agent.abortAndDiscardPendingRequests();
                }
            });
        service.streamComplete(toolCallResponse(oldTool.getName()));
        disconnect(stopConnection);

        QCOMPARE(oldTool.executeCount(), 1);
        QCOMPARE(newTool.executeCount(), 0);
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);

        agent.runStream(QStringLiteral("new registry request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 5000);
        QVERIFY(!requestAdvertisesTool(server, 1, newTool.getName()));
        agent.abortAndDiscardPendingRequests();
        QCOMPARE(aborted.count(), 2);
        QVERIFY(!agent.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
    }

    void hardStopCancelsSynchronousRunWithoutMutation()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                agent.abortAndDiscardPendingRequests();
            }
        });

        QCOMPARE(agent.run(QStringLiteral("sync request")), QStringLiteral("[Agent aborted]"));
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(!agent.isRunning());
        QVERIFY(!agent.queueRequest(QStringLiteral("late request")));

        const json history = agent.getMessages();
        QCOMPARE(history.size(), size_t(1));
        QCOMPARE(history[0].value("role", std::string()), std::string("user"));
        QCOMPARE(history[0].value("content", std::string()), std::string("sync request"));

        server.setRequestObserver({});
        QVERIFY2(server.releaseHeldRequest(), "sync transport was closed instead of drained");
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(agent.getMessages() == history);
        QCOMPARE(server.requestCount(), 1);
    }

    void hardStopDoesNotAccountGoalUsage()
    {
        QTemporaryDir goalDir;
        QVERIFY(goalDir.isValid());
        QSocGoalCatalog catalog;
        catalog.load(goalDir.path());
        QString goalError;
        QVERIFY(catalog.create(QStringLiteral("verify cancellation"), 0, &goalError));
        const auto before = catalog.current();
        QVERIFY(before.has_value());
        const QByteArray yamlBefore = readFile(catalog.projectFilePath());
        const QByteArray logBefore  = readFile(catalog.logFilePath());
        QSignalSpy       goalChanged(&catalog, &QSocGoalCatalog::goalChanged);
        goalChanged.clear();

        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        agent.setGoalCatalog(&catalog);
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                QTimer::singleShot(1100, &agent, [&]() { agent.abortAndDiscardPendingRequests(); });
            }
        });

        QCOMPARE(agent.run(QStringLiteral("goal request")), QStringLiteral("[Agent aborted]"));
        const auto after = catalog.current();
        QVERIFY(after.has_value());
        QCOMPARE(after->tokensUsed, before->tokensUsed);
        QCOMPARE(after->secondsUsed, before->secondsUsed);
        QCOMPARE(after->updatedAt, before->updatedAt);
        QCOMPARE(goalChanged.count(), 0);
        QCOMPARE(readFile(catalog.projectFilePath()), yamlBefore);
        QCOMPARE(readFile(catalog.logFilePath()), logBefore);

        server.setRequestObserver({});
        QVERIFY2(server.releaseHeldRequest(), "sync transport was closed instead of drained");
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(goalChanged.count(), 0);
        QCOMPARE(readFile(catalog.projectFilePath()), yamlBefore);
        QCOMPARE(readFile(catalog.logFilePath()), logBefore);
    }

    void hardStopDuringCompactionDoesNotCommitOrSendMainRequest()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgentConfig  config     = testConfig();
        config.maxContextTokens     = 10000;
        config.reservedOutputTokens = 0;
        config.pruneThreshold       = 0.99;
        config.compactThreshold     = 0.01;
        config.keepRecentMessages   = 2;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);

        json initial = json::array();
        for (int index = 0; index < 8; ++index) {
            const QString role = index % 2 == 0 ? QStringLiteral("user")
                                                : QStringLiteral("assistant");
            initial.push_back(
                {{"role", role.toStdString()},
                 {"content", QString(800, QLatin1Char('a' + index)).toStdString()}});
        }
        agent.setMessages(initial);
        server.setRequestObserver([&](int requestIndex) {
            if (requestIndex == 0) {
                agent.abortAndDiscardPendingRequests();
            }
        });

        agent.runStream(QStringLiteral("current request"));
        QCOMPARE(server.requestCount(), 1);
        const json compactRequest = json::parse(server.requestBody(0).toStdString());
        QVERIFY(compactRequest.contains("stream"));
        QVERIFY(!compactRequest["stream"].get<bool>());
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QVERIFY(!agent.isRunning());

        json expected = initial;
        expected.push_back({{"role", "user"}, {"content", "current request"}});
        QVERIFY(agent.getMessages() == expected);
        server.setRequestObserver({});
        QVERIFY2(server.releaseHeldRequest(), "compaction transport was closed instead of drained");
        QTRY_VERIFY_WITH_TIMEOUT(service.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY(agent.getMessages() == expected);
        QCOMPARE(server.requestCount(), 1);
    }

    void retryingObserverMayStartNewRun()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       retrying(&agent, &QSocAgent::retrying);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);
        bool             startedReplacement = false;

        connect(&agent, &QSocAgent::retrying, &agent, [&]() {
            if (!startedReplacement) {
                startedReplacement = true;
                agent.runStream(QStringLiteral("replacement request"));
            }
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 5000);
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("first request"));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("replacement request"));

        QTest::qWait(3500);
        QCOMPARE(server.requestCount(), 2);
        QVERIFY(agent.isRunning());
        QCOMPARE(retrying.count(), 1);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(errors.count(), 0);

        agent.abort();
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 1500);
        QVERIFY(!agent.isRunning());
    }

    void abortedObserverMayStartNewRun()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        server.enqueueStream(QStringLiteral("second complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry registry;
        QSocAgent        agent(nullptr, &service, &registry, testConfig());
        QSignalSpy       retrying(&agent, &QSocAgent::retrying);
        QSignalSpy       aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy       completed(&agent, &QSocAgent::runComplete);
        QSignalSpy       errors(&agent, &QSocAgent::runError);

        connect(&agent, &QSocAgent::retrying, &agent, [&]() {
            QTimer::singleShot(0, &agent, &QSocAgent::abort);
        });
        connect(&agent, &QSocAgent::runAborted, &agent, [&]() {
            agent.runStream(QStringLiteral("second request"));
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(retrying.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("second complete"));
        QCOMPARE(lastUserMessage(server, 0), QStringLiteral("first request"));
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("second request"));
        QCOMPARE(errors.count(), 0);

        QTest::qWait(3500);
        QVERIFY(!agent.isRunning());
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(errors.count(), 0);
    }

    void activeAbortObserverMayDeleteAgent()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry  registry;
        AbortCountingTool tool;
        registry.registerTool(&tool);
        auto               *agent = new QSocAgent(nullptr, &service, &registry, testConfig());
        QPointer<QSocAgent> owner(agent);
        QSignalSpy          aborted(agent, &QSocAgent::runAborted);

        connect(agent, &QSocAgent::runAborted, &service, [agent]() { delete agent; });

        agent->runStream(QStringLiteral("active request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        agent->abort();

        QCOMPARE(aborted.count(), 1);
        QVERIFY(owner.isNull());
        QCOMPARE(tool.abortCount(), 0);
    }

    void activeAbortObserverMayStartNewRun()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueHeldRequest();
        server.enqueueStream(QStringLiteral("second complete"));
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry  registry;
        AbortCountingTool tool;
        registry.registerTool(&tool);
        QSocAgent  agent(nullptr, &service, &registry, testConfig());
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy completed(&agent, &QSocAgent::runComplete);
        QSignalSpy errors(&agent, &QSocAgent::runError);

        connect(&agent, &QSocAgent::runAborted, &agent, [&]() {
            agent.runStream(QStringLiteral("second request"));
        });

        agent.runStream(QStringLiteral("first request"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
        agent.abort();

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("second complete"));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(lastUserMessage(server, 1), QStringLiteral("second request"));
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(tool.abortCount(), 0);
    }

    void deletingAgentCancelsPendingRetry()
    {
        MockServer server;
        QVERIFY(server.listen());
        server.enqueueError(503);
        QLLMService service;
        configureService(service, server);
        QSocToolRegistry    registry;
        auto               *agent = new QSocAgent(nullptr, &service, &registry, testConfig());
        QPointer<QSocAgent> owner(agent);
        QSignalSpy          retrying(agent, &QSocAgent::retrying);

        agent->runStream(QStringLiteral("pending retry"));
        QTRY_COMPARE_WITH_TIMEOUT(retrying.count(), 1, 5000);
        delete agent;

        QVERIFY(owner.isNull());
        QTest::qWait(3500);
        QCOMPARE(server.requestCount(), 1);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentbackoffabort.moc"
