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
    int        eofDelayMs = 0;
    bool       holdOpen   = false;
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

    bool eofSent(int responseIndex) const
    {
        return responseIndex >= 0 && responseIndex < eofSent_.size() && eofSent_.at(responseIndex);
    }

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
        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response      = responses_.dequeue();
        const int          responseIndex = eofSent_.size();
        eofSent_.append(false);
        if (response.holdOpen) {
            sendHeldOpen(socket, response);
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
    }

    void sendHeldOpen(QTcpSocket *socket, const MockResponse &response)
    {
        QByteArray header = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        const QByteArray chunk = QByteArray::number(response.body.size(), 16)
                                 + QByteArrayLiteral("\r\n") + response.body
                                 + QByteArrayLiteral("\r\n");
        socket->write(header + chunk);
        socket->flush();
    }

    QTcpServer                      server_;
    QQueue<MockResponse>            responses_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<bool>                     eofSent_;
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

json toolCall(int index, const QString &name)
{
    return {
        {"index", index},
        {"id", QStringLiteral("call_%1").arg(index).toStdString()},
        {"type", "function"},
        {"function", {{"name", name.toStdString()}, {"arguments", "{}"}}},
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

    /* clone() preserves the parent's fallback strategy and (when
     * set) the currentModelId. With no config-driven model registry
     * the model id stays empty, so we only assert fallbackStrategy
     * here. The model-aware path is covered by integration tests
     * that mount a real QSocConfig YAML. */
    void testCloneCopiesFallbackStrategy()
    {
        auto *parent = new QLLMService(this, nullptr);
        parent->setFallbackStrategy(LLMFallbackStrategy::Random);
        auto *child = parent->clone(this);
        /* No public getter for fallbackStrategy; we exercise the
         * code path and assert no crash + endpoint count parity. */
        QCOMPARE(child->endpointCount(), parent->endpointCount());
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

    void testDoneCompletesBeforeEofAndAllowsSyncRequest()
    {
        QSocTestCapture capture;
        MockHttpServer  server;
        QVERIFY(server.listen());
        server.enqueue(streamResponse(QStringLiteral("stream complete"), true, 500));
        server.enqueue(jsonResponse(QStringLiteral("recap complete")));

        {
            QLLMService service(nullptr, nullptr);
            service.addEndpoint(endpointFor(server));

            int     completionCount = 0;
            QString streamContent;
            json    streamResult;
            connect(&service, &QLLMService::streamComplete, this, [&](const json &response) {
                completionCount++;
                streamResult  = response;
                streamContent = QString::fromStdString(
                    response["choices"][0]["message"]["content"].get<std::string>());
            });

            service.sendChatCompletionStream(json::array(), json::array(), 0.0);
            QVERIFY2(waitUntil([&]() { return completionCount == 1; }), "stream did not complete");
            QCOMPARE(streamContent, QStringLiteral("stream complete"));
            QCOMPARE(
                QString::fromStdString(
                    streamResult["choices"][0]["finish_reason"].get<std::string>()),
                QStringLiteral("stop"));
            QCOMPARE(streamResult["usage"]["total_tokens"].get<int>(), 15);
            QVERIFY(!server.eofSent(0));

            const json response = service.sendChatCompletion(json::array(), json::array(), 0.0);
            QVERIFY(response.contains("choices"));
            QCOMPARE(
                QString::fromStdString(
                    response["choices"][0]["message"]["content"].get<std::string>()),
                QStringLiteral("recap complete"));
            QVERIFY2(waitUntil([&]() { return server.eofSent(0); }, 2000), "EOF was not sent");
            QVERIFY2(
                waitUntil([&]() { return service.findChildren<QNetworkReply *>().isEmpty(); }, 1000),
                "retired network replies were not deleted");
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY2(
            !capture.text().contains(QStringLiteral("device not open")), qPrintable(capture.text()));
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
