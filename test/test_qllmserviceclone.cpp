// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkReply>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtCore>
#include <QtTest>

#include <functional>

namespace {

struct MockResponse
{
    QByteArray contentType;
    QByteArray body;
    int        eofDelayMs        = 0;
    bool       holdOpen          = false;
    qsizetype  splitAt           = 0;
    bool       truncateTransport = false;
};

class MockHttpServer : public QObject
{
    Q_OBJECT

public:
    explicit MockHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, &MockHttpServer::handleNewConnection);
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(server_.serverAddress().toString());
        url.setPort(server_.serverPort());
        url.setPath(QStringLiteral("/chat/completions"));
        return url;
    }

    void enqueue(const MockResponse &response) { responses_.enqueue(response); }

    int requestCount() const { return requestCount_; }

    bool eofSent(int responseIndex) const
    {
        return responseIndex >= 0 && responseIndex < eofSent_.size() && eofSent_.at(responseIndex);
    }

    bool releaseSplit()
    {
        const QPointer<QTcpSocket> socket        = splitSocket_;
        const QByteArray           remainder     = splitRemainder_;
        const int                  responseIndex = splitResponseIndex_;
        splitSocket_.clear();
        splitRemainder_.clear();
        splitResponseIndex_ = -1;

        if (socket.isNull() || responseIndex < 0
            || socket->state() != QAbstractSocket::ConnectedState) {
            return false;
        }
        socket->write(remainder);
        socket->flush();
        eofSent_[responseIndex] = true;
        socket->disconnectFromHost();
        return true;
    }

signals:
    void responseSent(int responseIndex);

private slots:
    void handleNewConnection()
    {
        while (server_.hasPendingConnections()) {
            QTcpSocket *socket = server_.nextPendingConnection();
            buffers_.insert(socket, QByteArray());
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                handleRequestData(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

private:
    void handleRequestData(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers_[socket];
        buffer.append(socket->readAll());

        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        qsizetype contentLength = 0;
        for (QByteArray line : buffer.left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                contentLength = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
            }
        }
        if (buffer.size() < headerEnd + 4 + contentLength) {
            return;
        }

        buffers_.remove(socket);
        ++requestCount_;
        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response      = responses_.dequeue();
        const int          responseIndex = eofSent_.size();
        eofSent_.append(false);
        if (response.splitAt > 0) {
            sendSplit(socket, response, responseIndex);
        } else if (response.truncateTransport) {
            sendTruncatedChunked(socket, response, responseIndex);
        } else if (response.holdOpen) {
            sendHeldOpen(socket, response, responseIndex);
        } else if (response.eofDelayMs > 0) {
            sendChunked(socket, response, responseIndex);
        } else {
            sendComplete(socket, response, responseIndex);
        }
    }

    void sendComplete(QTcpSocket *socket, const MockResponse &response, int responseIndex)
    {
        QByteArray header = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ");
        header += response.contentType;
        header += QByteArrayLiteral("\r\nContent-Length: ");
        header += QByteArray::number(response.body.size());
        header += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(header + response.body);
        socket->flush();
        eofSent_[responseIndex] = true;
        socket->disconnectFromHost();
        emit responseSent(responseIndex);
    }

    void sendChunked(QTcpSocket *socket, const MockResponse &response, int responseIndex)
    {
        QByteArray header = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        const QByteArray chunk = QByteArray::number(response.body.size(), 16)
                                 + QByteArrayLiteral("\r\n") + response.body
                                 + QByteArrayLiteral("\r\n");
        socket->write(header + chunk);
        socket->flush();

        QTimer::singleShot(response.eofDelayMs, socket, [this, socket, responseIndex]() {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                socket->write(QByteArrayLiteral("0\r\n\r\n"));
                socket->flush();
                socket->disconnectFromHost();
            }
            eofSent_[responseIndex] = true;
        });
        emit responseSent(responseIndex);
    }

    void sendTruncatedChunked(QTcpSocket *socket, const MockResponse &response, int responseIndex)
    {
        QByteArray header = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        const QByteArray chunk = QByteArray::number(response.body.size(), 16)
                                 + QByteArrayLiteral("\r\n") + response.body
                                 + QByteArrayLiteral("\r\n");
        socket->write(header + chunk);
        socket->flush();
        eofSent_[responseIndex] = true;
        socket->disconnectFromHost();
        emit responseSent(responseIndex);
    }

    void sendHeldOpen(QTcpSocket *socket, const MockResponse &response, int responseIndex)
    {
        QByteArray header = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        const QByteArray chunk = QByteArray::number(response.body.size(), 16)
                                 + QByteArrayLiteral("\r\n") + response.body
                                 + QByteArrayLiteral("\r\n");
        socket->write(header + chunk);
        socket->flush();
        emit responseSent(responseIndex);
    }

    void sendSplit(QTcpSocket *socket, const MockResponse &response, int responseIndex)
    {
        const qsizetype splitAt = qBound<qsizetype>(1, response.splitAt, response.body.size() - 1);
        QByteArray      header  = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ");
        header += response.contentType;
        header += QByteArrayLiteral("\r\nContent-Length: ");
        header += QByteArray::number(response.body.size() + (response.truncateTransport ? 1 : 0));
        header += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(header + response.body.left(splitAt));
        socket->flush();
        splitSocket_        = socket;
        splitRemainder_     = response.body.mid(splitAt);
        splitResponseIndex_ = responseIndex;
        emit responseSent(responseIndex);
    }

    QTcpServer                      server_;
    QQueue<MockResponse>            responses_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<bool>                     eofSent_;
    QPointer<QTcpSocket>            splitSocket_;
    QByteArray                      splitRemainder_;
    int                             splitResponseIndex_ = -1;
    int                             requestCount_       = 0;
};

MockResponse streamResponse(const QString &content, bool includeDone, int eofDelayMs = 0)
{
    const json chunk = {
        {"choices", json::array({{{"delta", {{"content", content.toStdString()}}}}})}};
    QByteArray body = QByteArrayLiteral("data: ") + QByteArray::fromStdString(chunk.dump())
                      + QByteArrayLiteral("\n\n");

    const json finishChunk = {
        {"choices", json::array({{{"delta", json::object()}, {"finish_reason", "stop"}}})}};
    body += QByteArrayLiteral("data: ") + QByteArray::fromStdString(finishChunk.dump())
            + QByteArrayLiteral("\n\n");
    if (includeDone) {
        const json usageChunk
            = {{"choices", json::array()},
               {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}}}};
        body += QByteArrayLiteral("data: ") + QByteArray::fromStdString(usageChunk.dump())
                + QByteArrayLiteral("\n\n");
        body += QByteArrayLiteral("data: [DONE]\n\n");
    }
    return {QByteArrayLiteral("text/event-stream"), body, eofDelayMs};
}

QByteArray dataLine(const json &chunk, bool trailingNewline = true)
{
    QByteArray line = QByteArrayLiteral("data: ") + QByteArray::fromStdString(chunk.dump());
    if (trailingNewline) {
        line += QByteArrayLiteral("\n\n");
    }
    return line;
}

MockResponse deltaResponse(
    const json &delta, bool includeDone = true, bool trailingNewline = true, bool holdOpen = false)
{
    const json chunk = {
        {"choices", json::array({{{"delta", delta}, {"finish_reason", "old_finish"}}})}};
    QByteArray body = dataLine(chunk, trailingNewline);
    if (includeDone) {
        body += QByteArrayLiteral("data: [DONE]\n\n");
    }
    return {QByteArrayLiteral("text/event-stream"), body, 0, holdOpen};
}

MockResponse contentOnlyDoneResponse(const QString &content)
{
    const json chunk = {
        {"choices", json::array({{{"delta", {{"content", content.toStdString()}}}}})}};
    QByteArray body = dataLine(chunk);
    body += QByteArrayLiteral("data: [DONE]\n\n");
    return {QByteArrayLiteral("text/event-stream"), body};
}

MockResponse jsonResponse(const QString &content)
{
    const json body = {
        {"choices", json::array({{{"message", {{"content", content.toStdString()}}}}})}};
    return {QByteArrayLiteral("application/json"), QByteArray::fromStdString(body.dump()), 0};
}

LLMEndpoint endpointFor(const MockHttpServer &server)
{
    LLMEndpoint endpoint;
    endpoint.name    = QStringLiteral("runtime-test");
    endpoint.url     = server.url();
    endpoint.model   = QStringLiteral("test-model");
    endpoint.timeout = 3000;
    return endpoint;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() >= timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    return true;
}

struct StreamEvents : QObject
{
    QStringList content;
    QStringList reasoning;
    QStringList toolNames;
    QStringList errors;
    QList<json> completed;
};

void recordStreamEvents(QLLMService *service, StreamEvents *events)
{
    QObject::connect(
        service,
        &QLLMService::streamChunk,
        events,
        [events](const QString &chunk) { events->content.append(chunk); },
        Qt::DirectConnection);
    QObject::connect(
        service,
        &QLLMService::streamReasoningChunk,
        events,
        [events](const QString &chunk) { events->reasoning.append(chunk); },
        Qt::DirectConnection);
    QObject::connect(
        service,
        &QLLMService::streamToolCall,
        events,
        [events](const QString &, const QString &name, const QString &) {
            events->toolNames.append(name);
        },
        Qt::DirectConnection);
    QObject::connect(
        service,
        &QLLMService::streamError,
        events,
        [events](const QString &error) { events->errors.append(error); },
        Qt::DirectConnection);
    QObject::connect(
        service,
        &QLLMService::streamComplete,
        events,
        [events](const json &response) { events->completed.append(response); },
        Qt::DirectConnection);
}

QString completionContent(const json &response)
{
    return QString::fromStdString(response["choices"][0]["message"].value("content", std::string()));
}

struct SyncResult
{
    QString content;
    QString error;
};

SyncResult sendSyncRequest(QLLMService *service, bool chatCompletion)
{
    if (chatCompletion) {
        const json response = service->sendChatCompletion(json::array(), json::array(), 0.0);
        if (response.contains("error") && response["error"].is_string()) {
            return {{}, QString::fromStdString(response["error"].get<std::string>())};
        }
        return {completionContent(response), {}};
    }

    const LLMResponse response = service->sendRequest(QStringLiteral("runtime prompt"));
    return {response.content, response.success ? QString() : response.errorMessage};
}

json toolCall(int index, const QString &name)
{
    return {
        {"index", index},
        {"id", QStringLiteral("call_%1").arg(index).toStdString()},
        {"type", "function"},
        {"function", {{"name", name.toStdString()}, {"arguments", "{}"}}},
    };
}

json assistantResponse(const json &message)
{
    return {{"choices", json::array({{{"message", message}}})}};
}

QByteArray encodedJson(const json &value)
{
    return QByteArray::fromStdString(value.dump());
}

json assistantToolCall(const QString &id = QStringLiteral("call_0"))
{
    return {
        {"id", id.toStdString()},
        {"type", "function"},
        {"function", {{"name", "runtime_tool"}, {"arguments", "{}"}}},
    };
}

QNetworkReply *runningReply(QLLMService *service)
{
    const auto replies = service->findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : replies) {
        if (reply->isRunning()) {
            return reply;
        }
    }
    return nullptr;
}

bool waitForNoReplies(QLLMService *service)
{
    return waitUntil([service]() { return service->findChildren<QNetworkReply *>().isEmpty(); });
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* clone() returns a distinct, non-null instance. */
    void testCloneReturnsDistinctPointer()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *child  = parent->clone(this);
        QVERIFY(child != nullptr);
        QVERIFY(child != parent);
        delete child;
        delete parent;
    }

    /* Endpoints added directly are not copied automatically (clone
     * re-parses from QSocConfig). With no config, both are empty. */
    void testCloneWithoutConfigYieldsEmptyEndpoints()
    {
        auto       *parent = new QLLMService(this, nullptr);
        LLMEndpoint manual;
        manual.name = QStringLiteral("local");
        manual.url  = QUrl(QStringLiteral("http://localhost:1234/v1/chat"));
        parent->addEndpoint(manual);
        QCOMPARE(parent->endpointCount(), 1);

        auto *child = parent->clone(this);
        /* Clone has no config → loadConfigSettings did not pick up
         * endpoints. Manual endpoints are NOT carried; that is the
         * documented contract (configuration source-of-truth is
         * QSocConfig). */
        QCOMPARE(child->endpointCount(), 0);

        delete child;
        delete parent;
    }

    /* Empty clones remain independent across destruction orders. */
    void testIndependentDestructionOrderIsClean()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *childA = parent->clone(this);
        auto *childB = parent->clone(this);
        delete childB;
        delete childA;
        delete parent;
        /* If we reached here, no double-free / use-after-free. */
        QVERIFY(true);
    }

    /* Reverse destruction order is also safe (parent dies before
     * children). Children are parented to `this`, so they are
     * cleaned up by Qt's parent-child mechanism — the destructor
     * must not depend on the parent service still being alive. */
    void testParentDestroyedBeforeClones()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *clone  = parent->clone(this);
        delete parent;
        /* clone now stands alone */
        QCOMPARE(clone->endpointCount(), 0);
        delete clone;
    }

    /* clone() preserves fallback selection independently of endpoint config. */
    void testCloneCopiesFallbackStrategy()
    {
        MockHttpServer firstServer;
        MockHttpServer secondServer;
        QVERIFY(firstServer.listen());
        QVERIFY(secondServer.listen());
        firstServer.enqueue(jsonResponse(QStringLiteral("first")));
        firstServer.enqueue(jsonResponse(QStringLiteral("first again")));
        secondServer.enqueue(jsonResponse(QStringLiteral("second")));

        auto *parent = new QLLMService(this, nullptr);
        parent->setFallbackStrategy(LLMFallbackStrategy::RoundRobin);
        auto *child = parent->clone(this);
        child->addEndpoint(endpointFor(firstServer));
        child->addEndpoint(endpointFor(secondServer));

        const SyncResult first  = sendSyncRequest(child, false);
        const SyncResult second = sendSyncRequest(child, false);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
        QCOMPARE(first.content, QStringLiteral("first"));
        QCOMPARE(second.content, QStringLiteral("second"));
        QVERIFY2(waitForNoReplies(child), "clone replies were not deleted");

        delete child;
        delete parent;
    }

    void testAbortFromContentStopsSameDelta()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        json delta;
        delta["content"]           = "old content";
        delta["reasoning_content"] = "stale reasoning";
        delta["tool_calls"]        = json::array({toolCall(0, QStringLiteral("stale_tool"))});
        server.enqueue(deltaResponse(delta, true, true, true));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         abortRequested = false;
        recordStreamEvents(&service, &events);
        connect(
            &service,
            &QLLMService::streamChunk,
            this,
            [&](const QString &) {
                if (!abortRequested) {
                    abortRequested = true;
                    service.abortStream();
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "content abort produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "aborted reply was not deleted");

        QVERIFY(abortRequested);
        QCOMPARE(events.content, QStringList({QStringLiteral("old content")}));
        QCOMPARE(events.errors, QStringList({QStringLiteral("Aborted by user")}));
        QVERIFY(events.reasoning.isEmpty());
        QVERIFY(events.toolNames.isEmpty());
        QVERIFY(events.completed.isEmpty());
    }

    void testAbortFromEofProgressSuppressesCompletion()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const json delta = {{"content", "eof content"}};
        server.enqueue(deltaResponse(delta, false, false));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         abortRequested = false;
        recordStreamEvents(&service, &events);
        connect(
            &service,
            &QLLMService::streamChunk,
            this,
            [&](const QString &) {
                if (!abortRequested) {
                    abortRequested = true;
                    service.abortStream();
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "EOF progress produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "EOF reply was not deleted");

        QVERIFY(abortRequested);
        QCOMPARE(events.content, QStringList({QStringLiteral("eof content")}));
        QCOMPARE(events.errors, QStringList({QStringLiteral("Aborted by user")}));
        QVERIFY(events.completed.isEmpty());
    }

    void testReasoningCanRestartWithoutOldStateLeak_data()
    {
        QTest::addColumn<bool>("detailsFormat");
        QTest::newRow("reasoning-content") << false;
        QTest::newRow("reasoning-details") << true;
    }

    void testReasoningCanRestartWithoutOldStateLeak()
    {
        QFETCH(bool, detailsFormat);

        MockHttpServer server;
        QVERIFY(server.listen());

        json delta;
        if (detailsFormat) {
            delta["reasoning_details"] = json::array(
                {{{"text", "old reasoning"}}, {{"text", "must not escape"}}});
        } else {
            delta["reasoning_content"] = "old reasoning";
        }
        delta["tool_calls"] = json::array({toolCall(0, QStringLiteral("old_tool"))});
        server.enqueue(deltaResponse(delta, true, true, true));
        server.enqueue(contentOnlyDoneResponse(QStringLiteral("new content")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         restarted = false;
        recordStreamEvents(&service, &events);
        connect(
            &service,
            &QLLMService::streamReasoningChunk,
            this,
            [&](const QString &) {
                if (!restarted) {
                    restarted = true;
                    service.sendChatCompletionStream(json::array(), json::array(), 0.0);
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || !events.errors.isEmpty(); }),
            "reasoning restart did not finish");
        QVERIFY2(waitForNoReplies(&service), "restarted replies were not deleted");

        QVERIFY(restarted);
        QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
        QCOMPARE(events.reasoning, QStringList({QStringLiteral("old reasoning")}));
        QVERIFY(events.toolNames.isEmpty());
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("new content"));
        const json &choice  = events.completed.first()["choices"][0];
        const json &message = choice["message"];
        QVERIFY(!choice.contains("finish_reason"));
        QVERIFY(!message.contains("reasoning_content"));
        QVERIFY(!message.contains("reasoning_details"));
        QVERIFY(!message.contains("tool_calls"));
    }

    void testToolCallCanRestartWithoutFollowingOldTool()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        json delta;
        delta["tool_calls"] = json::array(
            {toolCall(0, QStringLiteral("first_tool")), toolCall(1, QStringLiteral("second_tool"))});
        server.enqueue(deltaResponse(delta, true, true, true));
        server.enqueue(contentOnlyDoneResponse(QStringLiteral("new content")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         restarted = false;
        recordStreamEvents(&service, &events);
        connect(
            &service,
            &QLLMService::streamToolCall,
            this,
            [&](const QString &, const QString &, const QString &) {
                if (!restarted) {
                    restarted = true;
                    service.sendChatCompletionStream(json::array(), json::array(), 0.0);
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || !events.errors.isEmpty(); }),
            "tool-call restart did not finish");
        QVERIFY2(waitForNoReplies(&service), "restarted replies were not deleted");

        QVERIFY(restarted);
        QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
        QCOMPARE(events.toolNames, QStringList({QStringLiteral("first_tool")}));
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("new content"));
        const json &choice  = events.completed.first()["choices"][0];
        const json &message = choice["message"];
        QVERIFY(!choice.contains("finish_reason"));
        QVERIFY(!message.contains("tool_calls"));
    }

    void testProgressCanDeleteServiceDirectly()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        json delta;
        delta["content"]           = "delete service";
        delta["reasoning_content"] = "must not escape";
        delta["tool_calls"]        = json::array({toolCall(0, QStringLiteral("must_not_run"))});
        server.enqueue(deltaResponse(delta, true, true, true));

        QPointer<QLLMService> service = new QLLMService(nullptr, nullptr);
        service->addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         deleted = false;
        recordStreamEvents(service.data(), &events);
        connect(
            service.data(),
            &QLLMService::streamChunk,
            this,
            [&](const QString &) {
                if (!deleted) {
                    deleted = true;
                    delete service.data();
                }
            },
            Qt::DirectConnection);

        service->sendChatCompletionStream(json::array(), json::array(), 0.0);
        const bool actionObserved = waitUntil([&]() { return deleted || service.isNull(); });
        if (!service.isNull()) {
            service->abortStream();
            delete service.data();
        }

        QVERIFY2(actionObserved, "progress deletion slot did not run");
        QVERIFY(deleted);
        QVERIFY(service.isNull());
        QCOMPARE(events.content, QStringList({QStringLiteral("delete service")}));
        QVERIFY(events.reasoning.isEmpty());
        QVERIFY(events.toolNames.isEmpty());
        QVERIFY(events.errors.isEmpty());
        QVERIFY(events.completed.isEmpty());
    }

    void testTerminalCanDeleteServiceDirectly_data()
    {
        QTest::addColumn<bool>("fail");
        QTest::newRow("complete") << false;
        QTest::newRow("provider-error") << true;
    }

    void testTerminalCanDeleteServiceDirectly()
    {
        QFETCH(bool, fail);

        MockHttpServer server;
        QVERIFY(server.listen());
        if (fail) {
            server.enqueue(
                {QByteArrayLiteral("text/event-stream"), dataLine({{"error", "provider failed"}})});
        } else {
            server.enqueue(contentOnlyDoneResponse(QStringLiteral("complete")));
        }

        QPointer<QLLMService> service = new QLLMService(nullptr, nullptr);
        service->addEndpoint(endpointFor(server));

        int     completionCount = 0;
        int     errorCount      = 0;
        QString content;
        QString error;
        connect(
            service.data(),
            &QLLMService::streamComplete,
            this,
            [&](const json &response) {
                completionCount++;
                content = completionContent(response);
                delete service.data();
            },
            Qt::DirectConnection);
        connect(
            service.data(),
            &QLLMService::streamError,
            this,
            [&](const QString &message) {
                errorCount++;
                error = message;
                delete service.data();
            },
            Qt::DirectConnection);

        service->sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return service.isNull(); }), "terminal slot did not delete service");

        if (fail) {
            QCOMPARE(error, QStringLiteral("provider failed"));
            QCOMPARE(errorCount, 1);
            QCOMPARE(completionCount, 0);
        } else {
            QCOMPARE(content, QStringLiteral("complete"));
            QCOMPARE(completionCount, 1);
            QCOMPARE(errorCount, 0);
        }
    }

    void testReplyDeletionFailsOnceAndAllowsRestart()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const json delta = {{"content", "delete reply"}};
        server.enqueue(deltaResponse(delta, false, true, true));
        server.enqueue(contentOnlyDoneResponse(QStringLiteral("recovered")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents            events;
        QPointer<QNetworkReply> reply;
        bool                    restarted = false;
        recordStreamEvents(&service, &events);
        connect(
            &service,
            &QLLMService::streamChunk,
            this,
            [&](const QString &chunk) {
                if (chunk != QStringLiteral("delete reply") || !reply.isNull()) {
                    return;
                }
                reply = runningReply(&service);
            },
            Qt::DirectConnection);
        connect(
            &service,
            &QLLMService::streamError,
            this,
            [&](const QString &) {
                if (!restarted) {
                    restarted = true;
                    service.sendChatCompletionStream(json::array(), json::array(), 0.0);
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(waitUntil([&]() { return !reply.isNull(); }), "stream reply was not found");
        delete reply.data();
        QVERIFY(reply.isNull());
        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || events.errors.size() > 1; }),
            "reply deletion recovery did not finish");
        QVERIFY2(waitForNoReplies(&service), "recovery replies were not deleted");

        QVERIFY(restarted);
        QCOMPARE(events.errors, QStringList({QStringLiteral("[HTTP 0] Network reply destroyed")}));
        QCOMPARE(
            events.content,
            QStringList({QStringLiteral("delete reply"), QStringLiteral("recovered")}));
        QVERIFY(events.reasoning.isEmpty());
        QVERIFY(events.toolNames.isEmpty());
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("recovered"));
    }

    void testDestroyedStreamErrorDoesNotLeakIntoReplacement()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(deltaResponse({{"content", "retired"}}, false, true, true));
        server.enqueue(contentOnlyDoneResponse(QStringLiteral("replacement")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        StreamEvents events;
        bool         replaced = false;
        recordStreamEvents(&service, &events);
        connect(
            &server,
            &MockHttpServer::responseSent,
            this,
            [&](int responseIndex) {
                if (responseIndex != 0 || replaced) {
                    return;
                }
                QNetworkReply *reply = runningReply(&service);
                if (reply == nullptr) {
                    return;
                }
                replaced = true;
                delete reply;
                service.sendChatCompletionStream(json::array(), json::array(), 0.0);
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || !events.errors.isEmpty(); }),
            "replacement stream did not finish");
        QVERIFY2(waitForNoReplies(&service), "replacement stream replies were not deleted");
        QCoreApplication::processEvents();

        QVERIFY(replaced);
        QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("replacement"));
    }

    void testInvalidStreamStartKeepsActiveTerminal()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(deltaResponse({{"content", "old stream"}}, false, true, true));

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(waitUntil([&]() { return runningReply(&service) != nullptr; }), "no active reply");
        QPointer<QNetworkReply> oldReply = runningReply(&service);

        service.clearEndpoints();
        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QCOMPARE(events.errors, QStringList({QStringLiteral("No LLM endpoint configured")}));

        delete oldReply.data();
        QVERIFY2(waitUntil([&]() { return events.errors.size() == 2; }), "old stream had no terminal");
        QVERIFY2(waitForNoReplies(&service), "old stream reply was not deleted");
        QCOMPARE(
            events.errors,
            QStringList(
                {QStringLiteral("No LLM endpoint configured"),
                 QStringLiteral("[HTTP 0] Network reply destroyed")}));
        QVERIFY(events.completed.isEmpty());
    }

    void testMalformedStreamFailsAtomically_data()
    {
        QTest::addColumn<QByteArray>("malformedLine");

        QTest::newRow("syntax") << QByteArrayLiteral("data: {\n\n");

        json wrongChoices;
        wrongChoices["choices"] = 1;
        QTest::newRow("choices-type") << dataLine(wrongChoices);

        QTest::newRow("number-range") << QByteArrayLiteral("data: 1e10000\n\n");

        json badToolCall;
        badToolCall["choices"] = json::array();
        badToolCall["choices"].push_back(json::object());
        badToolCall["choices"][0]["delta"]["content"]           = "must not escape";
        badToolCall["choices"][0]["delta"]["reasoning_content"] = "must not escape";
        badToolCall["choices"][0]["delta"]["tool_calls"] = json::array({{{"index", "invalid"}}});
        QTest::newRow("tool-index") << dataLine(badToolCall);

        badToolCall["choices"][0]["delta"]["tool_calls"] = json::array(
            {{{"index", 0}, {"type", "command"}}});
        QTest::newRow("tool-type") << dataLine(badToolCall);

        QByteArray invalidUtf8 = QByteArrayLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"");
        invalidUtf8.append(char(0xff));
        invalidUtf8 += QByteArrayLiteral("\"}}]}\n\n");
        QTest::newRow("invalid-utf8") << invalidUtf8;
    }

    void testMalformedStreamFailsAtomically()
    {
        QFETCH(QByteArray, malformedLine);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response = contentOnlyDoneResponse(QStringLiteral("must not escape"));
        response.body.prepend(malformedLine);
        server.enqueue(response);

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "malformed stream produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "malformed stream reply was not deleted");

        QCOMPARE(
            events.errors, QStringList({QStringLiteral("Malformed streaming response from LLM")}));
        QVERIFY(events.content.isEmpty());
        QVERIFY(events.reasoning.isEmpty());
        QVERIFY(events.toolNames.isEmpty());
        QVERIFY(events.completed.isEmpty());
    }

    void testConflictingStreamToolIdentityFails_data()
    {
        QTest::addColumn<QByteArray>("secondCallBody");

        json call  = toolCall(0, QStringLiteral("runtime_tool"));
        call["id"] = "different_id";
        QTest::newRow("id") << encodedJson(call);

        call                     = toolCall(0, QStringLiteral("runtime_tool"));
        call["function"]["name"] = "different_tool";
        QTest::newRow("name") << encodedJson(call);
    }

    void testConflictingStreamToolIdentityFails()
    {
        QFETCH(QByteArray, secondCallBody);

        MockHttpServer server;
        QVERIFY(server.listen());
        const auto chunk = [](const json &call) {
            return json{
                {"choices", json::array({{{"delta", {{"tool_calls", json::array({call})}}}}})},
            };
        };
        const json secondCall = json::parse(secondCallBody.constBegin(), secondCallBody.constEnd());
        QByteArray body       = dataLine(chunk(toolCall(0, QStringLiteral("runtime_tool"))));
        body += dataLine(chunk(secondCall));
        body += QByteArrayLiteral("data: [DONE]\n\n");
        server.enqueue({QByteArrayLiteral("text/event-stream"), body});

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "conflicting tool identity produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "conflicting stream reply was not deleted");

        QCOMPARE(
            events.errors, QStringList({QStringLiteral("Malformed streaming response from LLM")}));
        QCOMPARE(events.toolNames, QStringList({QStringLiteral("runtime_tool")}));
        QVERIFY(events.completed.isEmpty());
    }

    void testNonSseSuccessBodyFails()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(jsonResponse(QStringLiteral("must not become an empty reply")));

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "non-SSE response produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "non-SSE stream reply was not deleted");

        QCOMPARE(
            events.errors, QStringList({QStringLiteral("Malformed streaming response from LLM")}));
        QVERIFY(events.content.isEmpty());
        QVERIFY(events.completed.isEmpty());
    }

    void testSseControlFieldsAreIgnored()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        MockResponse response = contentOnlyDoneResponse(QStringLiteral("complete"));
        response.body.prepend(QByteArrayLiteral(
            ": keepalive\n"
            "event\n"
            "id\n"
            "retry\n"
            "data\n"
            "vendor-control: value\n\n"));
        server.enqueue(response);

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "SSE control fields produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "SSE control reply was not deleted");

        QVERIFY(events.errors.isEmpty());
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("complete"));
    }

    void testStreamWithoutAssistantChoiceFails_data()
    {
        QTest::addColumn<QByteArray>("body");

        QTest::newRow("empty") << QByteArray();
        QTest::newRow("control-only")
            << QByteArrayLiteral(": keepalive\nevent: ping\nvendor-control: value\n\n");
        QTest::newRow("usage-only")
            << dataLine(
                   {{"choices", json::array()},
                    {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 0}}}})
                   + QByteArrayLiteral("data: [DONE]\n\n");
        QTest::newRow("done-only") << QByteArrayLiteral("data: [DONE]\n\n");
    }

    void testStreamWithoutAssistantChoiceFails()
    {
        QFETCH(QByteArray, body);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue({QByteArrayLiteral("text/event-stream"), body});

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "stream without assistant choice produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "empty stream reply was not deleted");

        QCOMPARE(
            events.errors, QStringList({QStringLiteral("Malformed streaming response from LLM")}));
        QVERIFY(events.completed.isEmpty());
    }

    void testStreamProviderError_data()
    {
        QTest::addColumn<QByteArray>("errorLine");
        QTest::addColumn<QString>("expectedError");

        QTest::newRow("string") << dataLine({{"error", "service overloaded"}})
                                << QStringLiteral("service overloaded");
        QTest::newRow("blank-string") << dataLine({{"error", "   "}})
                                      << QStringLiteral("LLM provider returned a streaming error");
        QTest::newRow("object-message") << dataLine({{"error", {{"message", "quota exhausted"}}}})
                                        << QStringLiteral("quota exhausted");
        QTest::newRow("object-without-message")
            << dataLine({{"error", {{"code", "runtime_error"}}}})
            << QStringLiteral("LLM provider returned a streaming error");
    }

    void testStreamProviderError()
    {
        QFETCH(QByteArray, errorLine);
        QFETCH(QString, expectedError);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response = contentOnlyDoneResponse(QStringLiteral("must not escape"));
        response.body.prepend(errorLine);
        server.enqueue(response);

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "provider stream error produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "provider error reply was not deleted");

        QCOMPARE(events.errors, QStringList({expectedError}));
        QVERIFY(events.content.isEmpty());
        QVERIFY(events.reasoning.isEmpty());
        QVERIFY(events.toolNames.isEmpty());
        QVERIFY(events.completed.isEmpty());
    }

    void testStreamTransportErrorFailsOnce()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "transport failure produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "transport failure reply was not deleted");
        QCoreApplication::processEvents();

        QCOMPARE(events.errors.size(), 1);
        QVERIFY(events.errors.first().startsWith(QStringLiteral("[HTTP 0] ")));
        QVERIFY(events.completed.isEmpty());
    }

    void testBufferedChunkPrecedesTransportError_data()
    {
        QTest::addColumn<bool>("split");
        QTest::newRow("single-read") << false;
        QTest::newRow("split-read") << true;
    }

    void testBufferedChunkPrecedesTransportError()
    {
        QFETCH(bool, split);

        MockHttpServer server;
        QVERIFY(server.listen());

        const json   chunk = {{"choices", json::array({{{"delta", {{"content", "received"}}}}})}};
        MockResponse response      = {QByteArrayLiteral("text/event-stream"), dataLine(chunk)};
        response.truncateTransport = true;
        if (split) {
            const qsizetype contentStart = response.body.indexOf("received");
            QVERIFY(contentStart >= 0);
            response.splitAt = contentStart + 3;
        }
        server.enqueue(response);

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        QObject                 callbacks;
        QPointer<QNetworkReply> splitReply;
        bool                    firstSegmentConsumed = !split;
        bool                    remainderReleased    = !split;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (!split || responseIndex != 0 || !splitReply.isNull()) {
                    return;
                }
                splitReply = runningReply(&service);
                if (splitReply.isNull()) {
                    return;
                }
                connect(
                    splitReply.data(),
                    &QNetworkReply::readyRead,
                    &callbacks,
                    [&]() {
                        if (remainderReleased) {
                            return;
                        }
                        firstSegmentConsumed = splitReply->bytesAvailable() == 0;
                        remainderReleased    = server.releaseSplit();
                    },
                    Qt::DirectConnection);
            },
            Qt::DirectConnection);

        QStringList order;
        connect(&service, &QLLMService::streamChunk, this, [&](const QString &) {
            order.append(QStringLiteral("content"));
        });
        connect(&service, &QLLMService::streamError, this, [&](const QString &) {
            order.append(QStringLiteral("error"));
        });

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "truncated stream produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "truncated stream reply was not deleted");

        QCOMPARE(events.content, QStringList({QStringLiteral("received")}));
        QCOMPARE(order, QStringList({QStringLiteral("content"), QStringLiteral("error")}));
        QCOMPARE(events.errors.size(), 1);
        QVERIFY(events.completed.isEmpty());
        QVERIFY(firstSegmentConsumed);
        QVERIFY(remainderReleased);
    }

    void testSplitUtf8AndCompactDataFieldComplete()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QString expected = QStringLiteral("汉字🙂");
        MockResponse  response = contentOnlyDoneResponse(expected);
        response.body.replace(QByteArrayLiteral("data: "), QByteArrayLiteral("data:"));
        const QByteArray firstCharacter = QStringLiteral("汉").toUtf8();
        const qsizetype  characterStart = response.body.indexOf(firstCharacter);
        QVERIFY(characterStart >= 0);
        response.splitAt = characterStart + 1;
        server.enqueue(response);

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        QObject                 callbacks;
        QPointer<QNetworkReply> splitReply;
        bool                    firstReadyRead       = false;
        bool                    firstSegmentConsumed = false;
        bool                    remainderReleased    = false;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || !splitReply.isNull()) {
                    return;
                }
                splitReply = runningReply(&service);
                if (splitReply.isNull()) {
                    return;
                }
                connect(
                    splitReply.data(),
                    &QNetworkReply::readyRead,
                    &callbacks,
                    [&]() {
                        if (remainderReleased) {
                            return;
                        }
                        firstReadyRead       = true;
                        firstSegmentConsumed = splitReply->bytesAvailable() == 0;
                        remainderReleased    = server.releaseSplit();
                    },
                    Qt::DirectConnection);
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || !events.errors.isEmpty(); }),
            "split UTF-8 stream did not finish");
        QVERIFY2(waitForNoReplies(&service), "split UTF-8 reply was not deleted");

        QVERIFY(firstReadyRead);
        QVERIFY(firstSegmentConsumed);
        QVERIFY(remainderReleased);
        QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
        QCOMPARE(events.content, QStringList({expected}));
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), expected);
    }

    void testNestedWaitKeepsStreamEventOrder()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const json firstChunk = {
            {"choices",
             json::array({{{"delta", {{"content", "one"}, {"reasoning_content", "one"}}}}})}};
        const json secondChunk = {
            {"choices",
             json::array({{{"delta", {{"content", "two"}, {"reasoning_content", "two"}}}}})}};
        const QByteArray firstLine = dataLine(firstChunk);
        MockResponse     stream
            = {QByteArrayLiteral("text/event-stream"),
               firstLine + dataLine(secondChunk) + QByteArrayLiteral("data: [DONE]\n\n")};
        stream.splitAt = firstLine.size();
        server.enqueue(stream);
        server.enqueue(jsonResponse(QStringLiteral("nested complete")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        QStringList eventOrder;
        bool        nestedStarted     = false;
        bool        remainderReleased = false;
        SyncResult  nestedResult;
        connect(
            &service,
            &QLLMService::streamChunk,
            this,
            [&](const QString &content) {
                eventOrder.append(QStringLiteral("content:") + content);
                if (content != QStringLiteral("one") || nestedStarted) {
                    return;
                }
                nestedStarted     = true;
                remainderReleased = server.releaseSplit();
                nestedResult      = sendSyncRequest(&service, true);
            },
            Qt::DirectConnection);
        connect(
            &service,
            &QLLMService::streamReasoningChunk,
            this,
            [&](const QString &reasoning) {
                eventOrder.append(QStringLiteral("reasoning:") + reasoning);
            },
            Qt::DirectConnection);

        StreamEvents events;
        recordStreamEvents(&service, &events);
        service.sendChatCompletionStream(json::array(), json::array(), 0.0);

        QVERIFY2(
            waitUntil([&]() { return !events.completed.isEmpty() || !events.errors.isEmpty(); }),
            "reentrant stream did not finish");
        QVERIFY2(waitForNoReplies(&service), "reentrant stream replies were not deleted");

        QVERIFY(nestedStarted);
        QVERIFY(remainderReleased);
        QVERIFY2(nestedResult.error.isEmpty(), qPrintable(nestedResult.error));
        QCOMPARE(nestedResult.content, QStringLiteral("nested complete"));
        QCOMPARE(
            eventOrder,
            QStringList(
                {QStringLiteral("content:one"),
                 QStringLiteral("reasoning:one"),
                 QStringLiteral("content:two"),
                 QStringLiteral("reasoning:two")}));
        QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
        QCOMPARE(events.completed.size(), 1);
        QCOMPARE(completionContent(events.completed.first()), QStringLiteral("onetwo"));
    }

    void testAssistantMessageValidation_data()
    {
        QTest::addColumn<QByteArray>("responseBody");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("expectedError");

        const auto add = [](const char    *name,
                            const json    &response,
                            bool           valid,
                            const QString &error = QString()) {
            QTest::newRow(name) << encodedJson(response) << valid << error;
        };

        add("root-array", json::array(), false, QStringLiteral("Invalid response from LLM"));
        add("provider-string",
            {{"error", "service unavailable"}},
            false,
            QStringLiteral("service unavailable"));
        add("provider-object",
            {{"error", {{"message", "quota exhausted"}}}},
            false,
            QStringLiteral("quota exhausted"));
        add("provider-generic",
            {{"error", 7}},
            false,
            QStringLiteral("LLM provider returned an error"));
        add("choices-missing", json::object(), false, QStringLiteral("Invalid response from LLM"));
        add("choices-type",
            {{"choices", json::object()}},
            false,
            QStringLiteral("Invalid response from LLM"));
        add("choices-empty",
            {{"choices", json::array()}},
            false,
            QStringLiteral("Invalid response from LLM"));
        add("choice-type",
            {{"choices", json::array({7})}},
            false,
            QStringLiteral("Invalid response from LLM"));
        add("message-missing",
            {{"choices", json::array({json::object()})}},
            false,
            QStringLiteral("Invalid response from LLM"));
        add("message-type",
            {{"choices", json::array({{{"message", 7}}})}},
            false,
            QStringLiteral("Invalid response from LLM"));
        add("content-type",
            assistantResponse({{"content", 7}}),
            false,
            QStringLiteral("Invalid response from LLM"));
        add("content-missing",
            assistantResponse(json::object()),
            false,
            QStringLiteral("Invalid response from LLM"));
        add("content-null",
            assistantResponse({{"content", nullptr}}),
            false,
            QStringLiteral("Invalid response from LLM"));
        add("tool-calls-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::object()}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));
        add("tool-call-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({7})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        json call  = assistantToolCall();
        call["id"] = "";
        add("tool-id-empty",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        call       = assistantToolCall();
        call["id"] = 7;
        add("tool-id-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        call         = assistantToolCall();
        call["type"] = "command";
        add("tool-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        call             = assistantToolCall();
        call["function"] = 7;
        add("tool-function-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        call                     = assistantToolCall();
        call["function"]["name"] = "";
        add("tool-name-empty",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        call                          = assistantToolCall();
        call["function"]["arguments"] = json::object();
        add("tool-arguments-type",
            assistantResponse({{"content", nullptr}, {"tool_calls", json::array({call})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        const json duplicate = assistantToolCall(QStringLiteral("call_0"));
        add("tool-id-duplicate",
            assistantResponse(
                {{"content", nullptr}, {"tool_calls", json::array({duplicate, duplicate})}}),
            false,
            QStringLiteral("Invalid tool call from LLM"));

        add("content-valid", assistantResponse({{"content", "ready"}}), true);
        add("tool-call-valid",
            assistantResponse(
                {{"content", nullptr}, {"tool_calls", json::array({assistantToolCall()})}}),
            true);
    }

    void testAssistantMessageValidation()
    {
        QFETCH(QByteArray, responseBody);
        QFETCH(bool, valid);
        QFETCH(QString, expectedError);

        const json response = json::parse(responseBody.constBegin(), responseBody.constEnd());
        json       message;
        QString    error;

        QCOMPARE(QLLMService::extractAssistantMessage(response, &message, &error), valid);
        QCOMPARE(error, expectedError);
        if (valid) {
            QVERIFY(message == response.at("choices").front().at("message"));
        }
    }

    void testInvalidCompletedStreamFails_data()
    {
        QTest::addColumn<QByteArray>("toolCallsBody");

        json call  = toolCall(0, QStringLiteral("runtime_tool"));
        call["id"] = "";
        QTest::newRow("empty-id") << encodedJson(json::array({call}));

        call                     = toolCall(0, QStringLiteral("runtime_tool"));
        call["function"]["name"] = "";
        QTest::newRow("empty-name") << encodedJson(json::array({call}));

        call             = toolCall(0, QStringLiteral("first_tool"));
        json secondCall  = toolCall(1, QStringLiteral("second_tool"));
        secondCall["id"] = call["id"];
        QTest::newRow("duplicate-id") << encodedJson(json::array({call, secondCall}));
    }

    void testInvalidCompletedStreamFails()
    {
        QFETCH(QByteArray, toolCallsBody);

        MockHttpServer server;
        QVERIFY(server.listen());

        const json toolCalls = json::parse(toolCallsBody.constBegin(), toolCallsBody.constEnd());
        server.enqueue(deltaResponse({{"tool_calls", toolCalls}}));

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return !events.errors.isEmpty() || !events.completed.isEmpty(); }),
            "invalid completed stream produced no terminal signal");
        QVERIFY2(waitForNoReplies(&service), "invalid stream reply was not deleted");

        QCOMPARE(events.errors, QStringList({QStringLiteral("Invalid tool call from LLM")}));
        QVERIFY(events.completed.isEmpty());
    }

    void testMalformedSyncResponseFallsBack_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::addColumn<QByteArray>("malformedBody");

        QTest::newRow("request-syntax") << false << QByteArrayLiteral("{");

        json wrongContent;
        wrongContent["choices"] = json::array();
        wrongContent["choices"].push_back(json::object());
        wrongContent["choices"][0]["message"]["content"] = 7;
        QTest::newRow("request-type") << false << QByteArray::fromStdString(wrongContent.dump());

        QTest::newRow("request-range") << false << QByteArrayLiteral("1e10000");
        QTest::newRow("chat-syntax") << true << QByteArrayLiteral("{");
        QTest::newRow("chat-range") << true << QByteArrayLiteral("1e10000");

        json invalidToolCall  = assistantToolCall();
        invalidToolCall["id"] = "";
        QTest::newRow("chat-invalid-tool-call")
            << true
            << encodedJson(assistantResponse(
                   {{"content", nullptr}, {"tool_calls", json::array({invalidToolCall})}}));
    }

    void testMalformedSyncResponseFallsBack()
    {
        QFETCH(bool, chatCompletion);
        QFETCH(QByteArray, malformedBody);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue({QByteArrayLiteral("application/json"), malformedBody});
        server.enqueue(jsonResponse(QStringLiteral("recovered")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        service.addEndpoint(endpointFor(server));

        const SyncResult result = sendSyncRequest(&service, chatCompletion);
        QVERIFY2(waitForNoReplies(&service), "fallback replies were not deleted");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.content, QStringLiteral("recovered"));
        QCOMPARE(server.requestCount(), 2);
    }

    void testSyncToolCallWithNullContentSucceeds()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        json responseBody;
        responseBody["choices"] = json::array();
        responseBody["choices"].push_back(json::object());
        responseBody["choices"][0]["message"]["content"]    = nullptr;
        responseBody["choices"][0]["message"]["tool_calls"] = json::array(
            {toolCall(0, QStringLiteral("runtime_tool"))});
        server.enqueue(
            {QByteArrayLiteral("application/json"), QByteArray::fromStdString(responseBody.dump())});

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        const LLMResponse response = service.sendRequest(QStringLiteral("runtime prompt"));
        QVERIFY2(waitForNoReplies(&service), "null-content reply was not deleted");
        QVERIFY2(response.success, qPrintable(response.errorMessage));
        QVERIFY(response.jsonData["choices"][0]["message"]["content"].is_null());
        QVERIFY(response.content.contains(QStringLiteral("tool_calls")));
    }

    void testAsyncMissingManagerReportsFailure()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        QNetworkAccessManager *manager = service.findChild<QNetworkAccessManager *>();
        QVERIFY(manager != nullptr);
        delete manager;

        int     callbackCount = 0;
        QString error;
        service.sendRequestAsync(QStringLiteral("runtime prompt"), [&](LLMResponse &response) {
            callbackCount++;
            error = response.errorMessage;
        });

        QCOMPARE(callbackCount, 1);
        QCOMPARE(error, QStringLiteral("Network manager destroyed"));
    }

    void testAsyncEmptyCallbackIsSafe()
    {
        const std::function<void(LLMResponse &)> emptyCallback;
        QLLMService                              emptyService(nullptr, nullptr);
        emptyService.sendRequestAsync(QStringLiteral("runtime prompt"), emptyCallback);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(jsonResponse(QStringLiteral("ignored")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        service.sendRequestAsync(QStringLiteral("runtime prompt"), emptyCallback);
        QVERIFY(waitForNoReplies(&service));

        delete service.findChild<QNetworkAccessManager *>();
        service.sendRequestAsync(QStringLiteral("runtime prompt"), emptyCallback);
    }

    void testStreamMissingManagerReportsFailure()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));
        recordStreamEvents(&service, &events);

        QNetworkAccessManager *manager = service.findChild<QNetworkAccessManager *>();
        QVERIFY(manager != nullptr);
        delete manager;

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);

        QCOMPARE(events.errors, QStringList({QStringLiteral("Network manager destroyed")}));
        QVERIFY(events.completed.isEmpty());
        QVERIFY(service.findChildren<QNetworkReply *>().isEmpty());
    }

    void testAsyncTerminalPathsCompleteOnce_data()
    {
        QTest::addColumn<QString>("terminalPath");
        QTest::newRow("reply-destroyed") << QStringLiteral("reply");
        QTest::newRow("manager-destroyed") << QStringLiteral("manager");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("finished") << QStringLiteral("finished");
    }

    void testAsyncTerminalPathsCompleteOnce()
    {
        QFETCH(QString, terminalPath);

        MockHttpServer server;
        QVERIFY(server.listen());
        if (terminalPath == QStringLiteral("finished")) {
            server.enqueue(jsonResponse(QStringLiteral("async finished")));
        } else {
            server.enqueue(
                {QByteArrayLiteral("application/json"), QByteArrayLiteral("{}"), 0, true});
        }

        QLLMService service(nullptr, nullptr);
        LLMEndpoint endpoint = endpointFor(server);
        endpoint.timeout     = terminalPath == QStringLiteral("timeout") ? 20 : 500;
        service.addEndpoint(endpoint);

        QObject callbacks;
        bool    actionTaken = false;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || actionTaken || terminalPath == QStringLiteral("timeout")
                    || terminalPath == QStringLiteral("finished")) {
                    return;
                }
                actionTaken = true;
                if (terminalPath == QStringLiteral("reply")) {
                    delete runningReply(&service);
                    return;
                }
                delete service.findChild<QNetworkAccessManager *>();
            },
            Qt::DirectConnection);

        int     callbackCount = 0;
        bool    success       = false;
        QString content;
        QString error;
        service.sendRequestAsync(QStringLiteral("runtime prompt"), [&](LLMResponse &response) {
            callbackCount++;
            success = response.success;
            content = response.content;
            error   = response.errorMessage;
        });

        QVERIFY(waitUntil([&]() { return callbackCount == 1; }));
        QVERIFY(waitForNoReplies(&service));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(callbackCount, 1);

        if (terminalPath == QStringLiteral("finished")) {
            QVERIFY(!actionTaken);
            QVERIFY(success);
            QCOMPARE(content, QStringLiteral("async finished"));
            QVERIFY(error.isEmpty());
        } else if (terminalPath == QStringLiteral("timeout")) {
            QVERIFY(!actionTaken);
            QVERIFY(!success);
            QCOMPARE(error, QStringLiteral("Request timeout"));
        } else if (terminalPath == QStringLiteral("manager")) {
            QVERIFY(actionTaken);
            QVERIFY(!success);
            QCOMPARE(error, QStringLiteral("Network reply destroyed"));
        } else {
            QVERIFY(actionTaken);
            QVERIFY(!success);
            QCOMPARE(error, QStringLiteral("Network reply destroyed"));
        }
    }

    void testAsyncCallbackCanDeleteService()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(jsonResponse(QStringLiteral("async complete")));

        QPointer<QLLMService> service = new QLLMService(nullptr, nullptr);
        service->addEndpoint(endpointFor(server));

        int     callbackCount = 0;
        bool    success       = false;
        QString content;
        service->sendRequestAsync(QStringLiteral("runtime prompt"), [&](LLMResponse &response) {
            callbackCount++;
            success = response.success;
            content = response.content;
            delete service.data();
        });

        QVERIFY(waitUntil([&]() { return service.isNull(); }));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(callbackCount, 1);
        QVERIFY(success);
        QCOMPARE(content, QStringLiteral("async complete"));
    }

    void testAsyncDestroyedCallbackCanDeleteService_data()
    {
        QTest::addColumn<bool>("deleteManager");
        QTest::newRow("reply") << false;
        QTest::newRow("manager") << true;
    }

    void testAsyncDestroyedCallbackCanDeleteService()
    {
        QFETCH(bool, deleteManager);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{}"), 0, true});

        QPointer<QLLMService> service = new QLLMService(nullptr, nullptr);
        service->addEndpoint(endpointFor(server));

        QObject callbacks;
        bool    actionTaken = false;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || actionTaken || service.isNull()) {
                    return;
                }
                actionTaken = true;
                if (deleteManager) {
                    delete service->findChild<QNetworkAccessManager *>();
                } else {
                    delete runningReply(service.data());
                }
            },
            Qt::DirectConnection);

        int     callbackCount = 0;
        QString error;
        service->sendRequestAsync(QStringLiteral("runtime prompt"), [&](LLMResponse &response) {
            callbackCount++;
            error = response.errorMessage;
            delete service.data();
        });

        QVERIFY(waitUntil([&]() { return service.isNull(); }));
        QCoreApplication::processEvents();
        QVERIFY(actionTaken);
        QCOMPARE(callbackCount, 1);
        QCOMPARE(error, QStringLiteral("Network reply destroyed"));
    }

    void testStreamDestroyedErrorCanDeleteService_data()
    {
        QTest::addColumn<bool>("deleteManager");
        QTest::newRow("reply") << false;
        QTest::newRow("manager") << true;
    }

    void testStreamDestroyedErrorCanDeleteService()
    {
        QFETCH(bool, deleteManager);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(deltaResponse({{"content", "pending"}}, false, true, true));

        QPointer<QLLMService> service = new QLLMService(nullptr, nullptr);
        service->addEndpoint(endpointFor(server));

        QObject callbacks;
        bool    actionTaken = false;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || actionTaken || service.isNull()) {
                    return;
                }
                actionTaken = true;
                if (deleteManager) {
                    delete service->findChild<QNetworkAccessManager *>();
                } else {
                    delete runningReply(service.data());
                }
            },
            Qt::DirectConnection);

        int     errorCount = 0;
        QString error;
        connect(
            service.data(),
            &QLLMService::streamError,
            &callbacks,
            [&](const QString &message) {
                errorCount++;
                error = message;
                delete service.data();
            },
            Qt::DirectConnection);

        service->sendChatCompletionStream(json::array(), json::array(), 0.0);

        QVERIFY(waitUntil([&]() { return service.isNull(); }));
        QCoreApplication::processEvents();
        QVERIFY(actionTaken);
        QCOMPARE(errorCount, 1);
        QCOMPARE(error, QStringLiteral("[HTTP 0] Network reply destroyed"));
    }

    void testSyncDeletionQuitsNestedWait_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::addColumn<QString>("deletedObject");

        for (const bool chatCompletion : {false, true}) {
            const QByteArray prefix = chatCompletion ? QByteArrayLiteral("chat-")
                                                     : QByteArrayLiteral("request-");
            QTest::newRow((prefix + "reply").constData())
                << chatCompletion << QStringLiteral("reply");
            QTest::newRow((prefix + "manager").constData())
                << chatCompletion << QStringLiteral("manager");
            QTest::newRow((prefix + "service").constData())
                << chatCompletion << QStringLiteral("service");
        }
    }

    void testSyncDeletionQuitsNestedWait()
    {
        QFETCH(bool, chatCompletion);
        QFETCH(QString, deletedObject);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{"), 0, true});

        QPointer<QLLMService> service  = new QLLMService(nullptr, nullptr);
        LLMEndpoint           endpoint = endpointFor(server);
        endpoint.timeout               = 3000;
        service->addEndpoint(endpoint);

        QObject                         callbacks;
        QPointer<QNetworkReply>         observedReply;
        QPointer<QNetworkAccessManager> observedManager;
        bool                            actionTaken    = false;
        bool                            delayedMarker  = false;
        int                             destroyedCount = 0;
        int                             finishedCount  = 0;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || actionTaken || service.isNull()) {
                    return;
                }
                observedReply = runningReply(service.data());
                if (observedReply.isNull()) {
                    return;
                }
                actionTaken = true;
                connect(
                    observedReply.data(),
                    &QNetworkReply::finished,
                    &callbacks,
                    [&]() { finishedCount++; },
                    Qt::DirectConnection);
                connect(
                    observedReply.data(),
                    &QObject::destroyed,
                    &callbacks,
                    [&]() { destroyedCount++; },
                    Qt::DirectConnection);
                QTimer::singleShot(0, &callbacks, [&]() { delayedMarker = true; });

                if (deletedObject == QStringLiteral("reply")) {
                    delete observedReply.data();
                } else if (deletedObject == QStringLiteral("manager")) {
                    observedManager = service->findChild<QNetworkAccessManager *>();
                    delete observedManager.data();
                } else {
                    delete service.data();
                }
            },
            Qt::DirectConnection);

        const SyncResult result = sendSyncRequest(service.data(), chatCompletion);

        QVERIFY(actionTaken);
        QVERIFY2(!delayedMarker, "nested wait did not quit on object destruction");
        QVERIFY(observedReply.isNull());
        QCOMPARE(destroyedCount, 1);
        QCOMPARE(finishedCount, 0);
        if (deletedObject == QStringLiteral("service")) {
            QVERIFY(service.isNull());
            QCOMPARE(result.error, QStringLiteral("LLM service destroyed"));
            return;
        }

        QVERIFY(!service.isNull());
        QVERIFY2(waitForNoReplies(service.data()), "destroyed sync reply remained in the tree");
        if (deletedObject == QStringLiteral("manager")) {
            QVERIFY(observedManager.isNull());
            QCOMPARE(result.error, QStringLiteral("Network manager destroyed"));
        } else {
            QVERIFY(service->findChild<QNetworkAccessManager *>() != nullptr);
            QCOMPARE(result.error, QStringLiteral("All LLM endpoints failed"));
        }
        delete service.data();
    }

    void testDestroyedSyncReplyFallsBack_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::newRow("request") << false;
        QTest::newRow("chat") << true;
    }

    void testDestroyedSyncReplyFallsBack()
    {
        QFETCH(bool, chatCompletion);

        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{"), 0, true});
        server.enqueue(jsonResponse(QStringLiteral("recovered")));

        QLLMService service(nullptr, nullptr);
        LLMEndpoint endpoint = endpointFor(server);
        endpoint.timeout     = 3000;
        service.addEndpoint(endpoint);
        service.addEndpoint(endpoint);

        QObject                 callbacks;
        QPointer<QNetworkReply> deletedReply;
        bool                    replyDeleted = false;
        connect(
            &server,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || !deletedReply.isNull()) {
                    return;
                }
                deletedReply = runningReply(&service);
                replyDeleted = !deletedReply.isNull();
                delete deletedReply.data();
            },
            Qt::DirectConnection);

        const SyncResult result = sendSyncRequest(&service, chatCompletion);
        QVERIFY(replyDeleted);
        QVERIFY(deletedReply.isNull());
        QVERIFY2(waitForNoReplies(&service), "fallback sync replies were not deleted");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.content, QStringLiteral("recovered"));
    }

    void testReentrantSyncFallbackUsesStableOrder_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::newRow("request") << false;
        QTest::newRow("chat") << true;
    }

    void testReentrantSyncFallbackUsesStableOrder()
    {
        QFETCH(bool, chatCompletion);

        MockHttpServer firstServer;
        MockHttpServer secondServer;
        QVERIFY(firstServer.listen());
        QVERIFY(secondServer.listen());

        firstServer.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{")});
        firstServer.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{")});
        firstServer.enqueue(jsonResponse(QStringLiteral("wrong endpoint")));
        secondServer.enqueue(jsonResponse(QStringLiteral("inner")));
        secondServer.enqueue(jsonResponse(QStringLiteral("outer")));

        QLLMService service(nullptr, nullptr);
        LLMEndpoint first  = endpointFor(firstServer);
        first.name         = QStringLiteral("first");
        LLMEndpoint second = endpointFor(secondServer);
        second.name        = QStringLiteral("second");
        service.addEndpoint(first);
        service.addEndpoint(second);

        QObject    callbacks;
        bool       nestedStarted = false;
        SyncResult innerResult;
        connect(
            &firstServer,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || nestedStarted) {
                    return;
                }
                nestedStarted = true;
                innerResult   = sendSyncRequest(&service, chatCompletion);
            },
            Qt::DirectConnection);

        const SyncResult outerResult = sendSyncRequest(&service, chatCompletion);
        QVERIFY2(waitForNoReplies(&service), "reentrant fallback replies were not deleted");

        QVERIFY(nestedStarted);
        QVERIFY2(innerResult.error.isEmpty(), qPrintable(innerResult.error));
        QCOMPARE(innerResult.content, QStringLiteral("inner"));
        QVERIFY2(outerResult.error.isEmpty(), qPrintable(outerResult.error));
        QCOMPARE(outerResult.content, QStringLiteral("outer"));

        secondServer.enqueue(jsonResponse(QStringLiteral("next")));
        const SyncResult nextResult = sendSyncRequest(&service, chatCompletion);
        QVERIFY2(nextResult.error.isEmpty(), qPrintable(nextResult.error));
        QCOMPARE(nextResult.content, QStringLiteral("next"));
        QVERIFY(!firstServer.eofSent(2));
    }

    void testReconfiguredEndpointsRejectStaleCommit_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::newRow("request") << false;
        QTest::newRow("chat") << true;
    }

    void testReconfiguredEndpointsRejectStaleCommit()
    {
        QFETCH(bool, chatCompletion);

        MockHttpServer oldFirst;
        MockHttpServer oldSecond;
        MockHttpServer newFirst;
        MockHttpServer newSecond;
        QVERIFY(oldFirst.listen());
        QVERIFY(oldSecond.listen());
        QVERIFY(newFirst.listen());
        QVERIFY(newSecond.listen());

        oldFirst.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{")});
        oldSecond.enqueue(jsonResponse(QStringLiteral("old success")));
        newFirst.enqueue(jsonResponse(QStringLiteral("new first")));
        newSecond.enqueue(jsonResponse(QStringLiteral("new second")));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(oldFirst));
        service.addEndpoint(endpointFor(oldSecond));

        QObject callbacks;
        bool    rebuilt = false;
        connect(
            &oldSecond,
            &MockHttpServer::responseSent,
            &callbacks,
            [&](int responseIndex) {
                if (responseIndex != 0 || rebuilt) {
                    return;
                }
                rebuilt = true;
                service.clearEndpoints();
                service.addEndpoint(endpointFor(newFirst));
                service.addEndpoint(endpointFor(newSecond));
            },
            Qt::DirectConnection);

        const SyncResult oldResult = sendSyncRequest(&service, chatCompletion);
        QVERIFY(rebuilt);
        QVERIFY2(oldResult.error.isEmpty(), qPrintable(oldResult.error));
        QCOMPARE(oldResult.content, QStringLiteral("old success"));

        const SyncResult newResult = sendSyncRequest(&service, chatCompletion);
        QVERIFY2(waitForNoReplies(&service), "reconfigured endpoint replies were not deleted");
        QVERIFY2(newResult.error.isEmpty(), qPrintable(newResult.error));
        QCOMPARE(newResult.content, QStringLiteral("new first"));
        QVERIFY(!newSecond.eofSent(0));
    }

    void testRandomFallbackTriesEachEndpoint_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::newRow("request") << false;
        QTest::newRow("chat") << true;
    }

    void testRandomFallbackTriesEachEndpoint()
    {
        QFETCH(bool, chatCompletion);

        constexpr int  requestCount = 32;
        MockHttpServer failedServer;
        MockHttpServer healthyServer;
        QVERIFY(failedServer.listen());
        QVERIFY(healthyServer.listen());
        for (int index = 0; index < requestCount * 2; ++index) {
            failedServer.enqueue({QByteArrayLiteral("application/json"), QByteArrayLiteral("{")});
        }
        for (int index = 0; index < requestCount; ++index) {
            healthyServer.enqueue(jsonResponse(QStringLiteral("healthy")));
        }

        int healthyRequests = 0;
        connect(
            &healthyServer,
            &MockHttpServer::responseSent,
            this,
            [&](int) { ++healthyRequests; },
            Qt::DirectConnection);

        QSocTestCapture capture;
        QLLMService     service(nullptr, nullptr);
        service.addEndpoint(endpointFor(failedServer));
        service.addEndpoint(endpointFor(healthyServer));
        service.setFallbackStrategy(LLMFallbackStrategy::Random);

        for (int index = 0; index < requestCount; ++index) {
            const SyncResult result = sendSyncRequest(&service, chatCompletion);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.content, QStringLiteral("healthy"));
        }
        QVERIFY2(waitForNoReplies(&service), "random fallback replies were not deleted");
        QCOMPARE(healthyRequests, requestCount);
    }

    void testRoundRobinSyncRotates_data()
    {
        QTest::addColumn<bool>("chatCompletion");
        QTest::newRow("request") << false;
        QTest::newRow("chat") << true;
    }

    void testRoundRobinSyncRotates()
    {
        QFETCH(bool, chatCompletion);

        MockHttpServer firstServer;
        MockHttpServer secondServer;
        QVERIFY(firstServer.listen());
        QVERIFY(secondServer.listen());
        for (int index = 0; index < 2; ++index) {
            firstServer.enqueue(jsonResponse(QStringLiteral("first")));
            secondServer.enqueue(jsonResponse(QStringLiteral("second")));
        }

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(firstServer));
        service.addEndpoint(endpointFor(secondServer));
        service.setFallbackStrategy(LLMFallbackStrategy::RoundRobin);

        const QStringList expected = {
            QStringLiteral("first"),
            QStringLiteral("second"),
            QStringLiteral("first"),
            QStringLiteral("second"),
        };
        for (const QString &content : expected) {
            const SyncResult result = sendSyncRequest(&service, chatCompletion);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.content, content);
        }
        QVERIFY2(waitForNoReplies(&service), "round-robin sync replies were not deleted");
    }

    void testRoundRobinAsyncRotates()
    {
        MockHttpServer firstServer;
        MockHttpServer secondServer;
        QVERIFY(firstServer.listen());
        QVERIFY(secondServer.listen());
        for (int index = 0; index < 2; ++index) {
            firstServer.enqueue(jsonResponse(QStringLiteral("first")));
            secondServer.enqueue(jsonResponse(QStringLiteral("second")));
        }

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(firstServer));
        service.addEndpoint(endpointFor(secondServer));
        service.setFallbackStrategy(LLMFallbackStrategy::RoundRobin);

        const QStringList expected = {
            QStringLiteral("first"),
            QStringLiteral("second"),
            QStringLiteral("first"),
            QStringLiteral("second"),
        };
        for (const QString &content : expected) {
            bool        completed = false;
            LLMResponse response;
            service.sendRequestAsync(QStringLiteral("runtime prompt"), [&](LLMResponse &result) {
                response  = result;
                completed = true;
            });
            QVERIFY2(waitUntil([&]() { return completed; }), "async request did not finish");
            QVERIFY2(response.success, qPrintable(response.errorMessage));
            QCOMPARE(response.content, content);
        }
        QVERIFY2(waitForNoReplies(&service), "round-robin async replies were not deleted");
    }

    void testRoundRobinStreamRotates()
    {
        MockHttpServer firstServer;
        MockHttpServer secondServer;
        QVERIFY(firstServer.listen());
        QVERIFY(secondServer.listen());
        for (int index = 0; index < 2; ++index) {
            firstServer.enqueue(contentOnlyDoneResponse(QStringLiteral("first")));
            secondServer.enqueue(contentOnlyDoneResponse(QStringLiteral("second")));
        }

        StreamEvents events;
        QLLMService  service(nullptr, nullptr);
        service.addEndpoint(endpointFor(firstServer));
        service.addEndpoint(endpointFor(secondServer));
        service.setFallbackStrategy(LLMFallbackStrategy::RoundRobin);
        recordStreamEvents(&service, &events);

        const QStringList expected = {
            QStringLiteral("first"),
            QStringLiteral("second"),
            QStringLiteral("first"),
            QStringLiteral("second"),
        };
        for (const QString &content : expected) {
            const qsizetype completed = events.completed.size();
            service.sendChatCompletionStream(json::array(), json::array(), 0.0);
            QVERIFY2(
                waitUntil([&]() {
                    return events.completed.size() > completed || !events.errors.isEmpty();
                }),
                "stream request did not finish");
            QVERIFY2(events.errors.isEmpty(), qPrintable(events.errors.join(QLatin1Char('\n'))));
            QCOMPARE(completionContent(events.completed.last()), content);
            QVERIFY2(waitForNoReplies(&service), "round-robin stream reply was not deleted");
        }
    }

    void testDoneCompletesBeforeEofAndAllowsSyncRequest()
    {
        QSocTestCapture capture;
        MockHttpServer  server;
        QVERIFY(server.listen());
        MockResponse     stream = streamResponse(QStringLiteral("stream complete"), true);
        const QByteArray tail   = QByteArrayLiteral(": transport remains open\n\n");
        stream.body += tail;
        stream.splitAt = stream.body.size() - tail.size();
        server.enqueue(stream);
        server.enqueue(jsonResponse(QStringLiteral("recap complete")));

        {
            QLLMService service(nullptr, nullptr);
            service.addEndpoint(endpointFor(server));

            int        completionCount = 0;
            QString    streamContent;
            json       streamResult;
            bool       completedBeforeEof    = false;
            bool       transportStillRunning = false;
            bool       tailReleased          = false;
            SyncResult recapResult;
            connect(
                &service,
                &QLLMService::streamComplete,
                this,
                [&](const json &response) {
                    completionCount++;
                    streamResult  = response;
                    streamContent = QString::fromStdString(
                        response["choices"][0]["message"]["content"].get<std::string>());
                    completedBeforeEof    = !server.eofSent(0);
                    transportStillRunning = runningReply(&service) != nullptr;
                    recapResult           = sendSyncRequest(&service, true);
                    tailReleased          = server.releaseSplit();
                },
                Qt::DirectConnection);

            service.sendChatCompletionStream(json::array(), json::array(), 0.0);
            QVERIFY2(waitUntil([&]() { return completionCount == 1; }), "stream did not complete");
            QCOMPARE(streamContent, QStringLiteral("stream complete"));
            QCOMPARE(
                QString::fromStdString(
                    streamResult["choices"][0]["finish_reason"].get<std::string>()),
                QStringLiteral("stop"));
            QCOMPARE(streamResult["usage"]["total_tokens"].get<int>(), 15);
            QVERIFY(completedBeforeEof);
            QVERIFY(transportStillRunning);
            QVERIFY2(recapResult.error.isEmpty(), qPrintable(recapResult.error));
            QCOMPARE(recapResult.content, QStringLiteral("recap complete"));
            QVERIFY2(tailReleased, "stream transport closed before recap returned");
            QVERIFY(server.eofSent(0));
            QVERIFY2(
                waitUntil([&]() { return service.findChildren<QNetworkReply *>().isEmpty(); }, 1000),
                "retired network replies were not deleted");
            QCOMPARE(completionCount, 1);
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY2(
            !capture.text().contains(QStringLiteral("device not open")), qPrintable(capture.text()));
    }

    void testCompletionNestedLoopIsSafe()
    {
        QSocTestCapture capture;
        MockHttpServer  server;
        QVERIFY(server.listen());
        server.enqueue(contentOnlyDoneResponse(QStringLiteral("complete")));

        {
            QLLMService service(nullptr, nullptr);
            service.addEndpoint(endpointFor(server));

            int     completionCount = 0;
            int     errorCount      = 0;
            QString content;
            connect(
                &service,
                &QLLMService::streamComplete,
                this,
                [&](const json &response) {
                    completionCount++;
                    content = completionContent(response);
                    QEventLoop loop;
                    QTimer::singleShot(100, &loop, &QEventLoop::quit);
                    loop.exec();
                },
                Qt::DirectConnection);
            connect(&service, &QLLMService::streamError, this, [&](const QString &) {
                errorCount++;
            });

            service.sendChatCompletionStream(json::array(), json::array(), 0.0);
            QVERIFY2(waitUntil([&]() { return completionCount == 1; }), "stream did not complete");
            QCOMPARE(content, QStringLiteral("complete"));
            QCOMPARE(errorCount, 0);
            QVERIFY2(waitForNoReplies(&service), "retired network reply was not deleted");
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY2(
            !capture.text().contains(QStringLiteral("device not open")), qPrintable(capture.text()));
    }

    void testAbortNestedLoopIsSafe()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(deltaResponse({{"content", "pending"}}, false, true, true));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        int completionCount = 0;
        int errorCount      = 0;
        connect(&service, &QLLMService::streamComplete, this, [&](const json &) {
            completionCount++;
        });
        connect(
            &service,
            &QLLMService::streamError,
            this,
            [&](const QString &error) {
                QCOMPARE(error, QStringLiteral("Aborted by user"));
                errorCount++;
                QEventLoop loop;
                QTimer::singleShot(20, &loop, &QEventLoop::quit);
                loop.exec();
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(waitUntil([&]() { return runningReply(&service) != nullptr; }), "no active reply");
        service.abortStream();

        QCOMPARE(errorCount, 1);
        QCOMPARE(completionCount, 0);
        QVERIFY2(waitForNoReplies(&service), "aborted network reply was not deleted");
    }

    void testFailureNestedLoopIsSafe_data()
    {
        QTest::addColumn<bool>("timeout");
        QTest::addColumn<QString>("expectedError");
        QTest::newRow("provider") << false << QStringLiteral("provider failed");
        QTest::newRow("timeout") << true << QStringLiteral("Request timeout");
    }

    void testFailureNestedLoopIsSafe()
    {
        QFETCH(bool, timeout);
        QFETCH(QString, expectedError);

        MockHttpServer server;
        QVERIFY(server.listen());
        if (timeout) {
            server.enqueue(deltaResponse({{"content", "pending"}}, false, true, true));
        } else {
            server.enqueue(
                {QByteArrayLiteral("text/event-stream"),
                 dataLine({{"error", expectedError.toStdString()}})});
        }

        QLLMService service(nullptr, nullptr);
        LLMEndpoint endpoint = endpointFor(server);
        endpoint.timeout     = timeout ? 20 : 3000;
        service.addEndpoint(endpoint);

        int     completionCount = 0;
        int     errorCount      = 0;
        QString actualError;
        connect(&service, &QLLMService::streamComplete, this, [&](const json &) {
            completionCount++;
        });
        connect(
            &service,
            &QLLMService::streamError,
            this,
            [&](const QString &error) {
                errorCount++;
                actualError = error;
                QEventLoop loop;
                QTimer::singleShot(20, &loop, &QEventLoop::quit);
                loop.exec();
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(waitUntil([&]() { return errorCount == 1; }), "stream did not fail");

        QCOMPARE(actualError, expectedError);
        QCOMPARE(errorCount, 1);
        QCOMPARE(completionCount, 0);
        QVERIFY2(waitForNoReplies(&service), "failed network reply was not deleted");
    }

    void testEofCompletionCanStartNextStreamDirectly()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        server.enqueue(streamResponse(QStringLiteral("first"), false));
        server.enqueue(streamResponse(QStringLiteral("second"), true, 1000));

        QLLMService service(nullptr, nullptr);
        service.addEndpoint(endpointFor(server));

        QStringList completedContents;
        QStringList errors;
        connect(&service, &QLLMService::streamError, this, [&](const QString &error) {
            errors.append(error);
        });
        connect(
            &service,
            &QLLMService::streamComplete,
            this,
            [&](const json &response) {
                completedContents.append(
                    QString::fromStdString(
                        response["choices"][0]["message"]["content"].get<std::string>()));
                if (completedContents.size() == 1) {
                    service.sendChatCompletionStream(json::array(), json::array(), 0.0);
                }
            },
            Qt::DirectConnection);

        service.sendChatCompletionStream(json::array(), json::array(), 0.0);
        QVERIFY2(
            waitUntil([&]() { return completedContents.size() == 2 || !errors.isEmpty(); }),
            "second stream did not complete after direct re-entry");
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(completedContents, QStringList({QStringLiteral("first"), QStringLiteral("second")}));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmserviceclone.moc"
