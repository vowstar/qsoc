// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QQueue>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

using json = nlohmann::json;

namespace {

struct MockResponse
{
    int        statusCode = 200;
    QByteArray contentType;
    QByteArray body;
    bool       holdOpen = false;
};

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

    void enqueueHeldRequest() { responses_.enqueue({200, {}, {}, true}); }

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
        ++requestCount_;
        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response = responses_.dequeue();
        if (response.holdOpen) {
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
    QTcpServer                      server_;
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

class Test final : public QObject
{
    Q_OBJECT

private slots:
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
