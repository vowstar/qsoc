// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcphttp.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QtCore>
#include <QtTest>

namespace {

struct MockResponse
{
    QByteArray statusLine = "200 OK";
    QByteArray contentType;
    QByteArray body;
    QByteArray sessionId;
    int        bodyDelayMs          = 0;
    int        delayMs              = 0;
    bool       closeWithoutResponse = false;
    bool       finishSse            = true;
    /* Per-event delays in milliseconds. Empty means send the whole body
     * in one shot. Used to drive SSE chunking from the server side. */
    QList<int> sseChunkDelaysMs;
};

class MockHttpServer : public QObject
{
    Q_OBJECT

public:
    explicit MockHttpServer(QObject *parent = nullptr)
        : QObject(parent)
        , server_(new QTcpServer(this))
    {
        connect(server_, &QTcpServer::newConnection, this, &MockHttpServer::onNewConnection);
    }

    bool listen() { return server_->listen(QHostAddress::LocalHost); }

    quint16 port() const { return server_->serverPort(); }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(port()); }

    void enqueue(const MockResponse &response) { responses_.enqueue(response); }

    void enqueueForRequestId(int requestId, const MockResponse &response)
    {
        responsesByRequestId_.insert(requestId, response);
    }

    QList<QByteArray> requestBodies() const { return requestBodies_; }

    QList<QByteArray> requestSessionIds() const { return requestSessionIds_; }

    QList<QMap<QByteArray, QByteArray>> requestHeaders() const { return requestHeaders_; }

private slots:
    void onNewConnection()
    {
        while (server_->hasPendingConnections()) {
            auto *socket = server_->nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                buffers_.remove(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            buffers_.insert(socket, QByteArray());
        }
    }

    void onReadyRead(QTcpSocket *socket)
    {
        buffers_[socket].append(socket->readAll());
        const QByteArray &buf = buffers_[socket];

        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        QMap<QByteArray, QByteArray> headers;
        for (const QByteArray &line : buf.left(headerEnd).split('\n')) {
            QByteArray trimmed = line;
            if (trimmed.endsWith('\r')) {
                trimmed.chop(1);
            }
            const qsizetype separator = trimmed.indexOf(':');
            if (separator > 0) {
                headers.insert(
                    trimmed.left(separator).trimmed().toLower(),
                    trimmed.mid(separator + 1).trimmed());
            }
        }

        const qsizetype  contentLength = headers.value("content-length").toLongLong();
        const QByteArray sessionId     = headers.value("mcp-session-id");

        if (buf.size() < headerEnd + 4 + contentLength) {
            return;
        }

        const QByteArray body = buf.mid(headerEnd + 4, contentLength);
        requestBodies_ << body;
        requestSessionIds_ << sessionId;
        requestHeaders_ << headers;
        buffers_.remove(socket);

        MockResponse response;
        bool         hasResponse = false;
        try {
            const auto request = nlohmann::json::parse(body.toStdString());
            if (request.is_object() && request.contains("id") && request["id"].is_number_integer()) {
                const int requestId = request["id"].get<int>();
                if (responsesByRequestId_.contains(requestId)) {
                    response    = responsesByRequestId_.take(requestId);
                    hasResponse = true;
                }
            }
        } catch (const std::exception &) {
        }
        if (!hasResponse && !responses_.isEmpty()) {
            response    = responses_.dequeue();
            hasResponse = true;
        }
        if (!hasResponse) {
            socket->disconnectFromHost();
            return;
        }

        if (response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, socket, [this, socket, response]() {
                sendResponse(socket, response);
            });
            return;
        }
        sendResponse(socket, response);
    }

private:
    void sendResponse(QTcpSocket *socket, const MockResponse &response)
    {
        if (response.closeWithoutResponse) {
            socket->abort();
            return;
        }
        if (response.contentType.contains("text/event-stream")
            && !response.sseChunkDelaysMs.isEmpty()) {
            sendChunkedSse(socket, response);
        } else {
            sendOneShot(socket, response);
        }
    }

    void sendOneShot(QTcpSocket *socket, const MockResponse &response)
    {
        QByteArray http = "HTTP/1.1 " + response.statusLine + "\r\n";
        if (!response.contentType.isEmpty()) {
            http += "Content-Type: " + response.contentType + "\r\n";
        }
        if (!response.sessionId.isEmpty()) {
            http += "Mcp-Session-Id: " + response.sessionId + "\r\n";
        }
        http += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        http += "Connection: close\r\n\r\n";
        socket->write(http);
        socket->flush();
        if (response.bodyDelayMs > 0) {
            QTimer::singleShot(response.bodyDelayMs, socket, [socket, body = response.body]() {
                if (socket->state() != QAbstractSocket::ConnectedState) {
                    return;
                }
                socket->write(body);
                socket->flush();
                socket->disconnectFromHost();
            });
            return;
        }
        socket->write(response.body);
        socket->flush();
        socket->disconnectFromHost();
    }

    void sendChunkedSse(QTcpSocket *socket, const MockResponse &response)
    {
        QByteArray header = "HTTP/1.1 " + response.statusLine + "\r\n";
        header += "Content-Type: " + response.contentType + "\r\n";
        header += "Transfer-Encoding: chunked\r\n";
        header += "Connection: close\r\n\r\n";
        socket->write(header);
        socket->flush();

        const QList<QByteArray> events = response.body.split('|');
        for (qsizetype i = 0; i < events.size(); ++i) {
            const int delay = (i < response.sseChunkDelaysMs.size())
                                  ? response.sseChunkDelaysMs.at(i)
                                  : 0;
            QTimer::singleShot(delay, socket, [socket, chunk = events.at(i)]() {
                if (socket->state() != QAbstractSocket::ConnectedState) {
                    return;
                }
                const QByteArray sized = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk
                                         + "\r\n";
                socket->write(sized);
                socket->flush();
            });
        }
        if (!response.finishSse) {
            return;
        }
        const int closeDelay
            = (response.sseChunkDelaysMs.isEmpty()
                   ? 0
                   : response.sseChunkDelaysMs.at(response.sseChunkDelaysMs.size() - 1) + 50);
        QTimer::singleShot(closeDelay, socket, [socket]() {
            if (socket->state() != QAbstractSocket::ConnectedState) {
                return;
            }
            socket->write("0\r\n\r\n");
            socket->flush();
            socket->disconnectFromHost();
        });
    }

    QTcpServer                         *server_ = nullptr;
    QQueue<MockResponse>                responses_;
    QHash<int, MockResponse>            responsesByRequestId_;
    QList<QByteArray>                   requestBodies_;
    QList<QByteArray>                   requestSessionIds_;
    QList<QMap<QByteArray, QByteArray>> requestHeaders_;
    QHash<QTcpSocket *, QByteArray>     buffers_;
};

McpServerConfig httpConfig(const QString &url)
{
    McpServerConfig cfg;
    cfg.name             = "http";
    cfg.type             = "http";
    cfg.url              = url;
    cfg.requestTimeoutMs = 2000;
    cfg.connectTimeoutMs = 2000;
    return cfg;
}

bool waitForSignal(QSignalSpy &spy, int minCount, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (spy.size() < minCount) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

bool waitForRequests(const MockHttpServer &server, qsizetype minCount, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (server.requestBodies().size() < minCount) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

qsizetype runningReplyCount(const QSocMcpHttpTransport *transport)
{
    qsizetype count = 0;
    for (QNetworkReply *reply : transport->findChildren<QNetworkReply *>()) {
        if (reply->isRunning()) {
            ++count;
        }
    }
    return count;
}

QList<QByteArray> requestSessionsForMethod(const MockHttpServer &server, const QString &method)
{
    QList<QByteArray> sessions;
    const auto        bodies     = server.requestBodies();
    const auto        sessionIds = server.requestSessionIds();
    const qsizetype   count      = qMin(bodies.size(), sessionIds.size());
    for (qsizetype i = 0; i < count; ++i) {
        try {
            const auto request = nlohmann::json::parse(bodies.at(i).toStdString());
            if (request.is_object()
                && request.value("method", std::string()) == method.toStdString()) {
                sessions.append(sessionIds.at(i));
            }
        } catch (const std::exception &) {
        }
    }
    return sessions;
}

bool establishSession(
    MockHttpServer       &server,
    QSocMcpHttpTransport &transport,
    QSignalSpy           &messageSpy,
    const QByteArray     &sessionId)
{
    MockResponse initialize;
    initialize.contentType = "application/json";
    initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    initialize.sessionId   = sessionId;
    server.enqueue(initialize);

    transport.start();
    transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    return waitForSignal(messageSpy, 1, 3000);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void parsesJsonResponse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = R"({"jsonrpc":"2.0","id":1,"result":{"ok":true}})";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        QVERIFY(waitForSignal(startedSpy, 1));

        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "ping";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        const auto msg = messageSpy.first().first().value<nlohmann::json>();
        QCOMPARE(msg["id"].get<int>(), 1);
        QVERIFY(msg["result"]["ok"].get<bool>());
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(failureSpy.size(), qsizetype(0));
    }

    void parsesJsonBatchResponse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body
            = R"([{"jsonrpc":"2.0","id":1,"result":{"ok":true}},{"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"Method not found"}}])";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        QVERIFY(waitForSignal(startedSpy, 1));

        const nlohmann::json request = nlohmann::json::array({
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
            {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
        });
        transport.sendMessage(request);

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        const auto message = messageSpy.first().first().value<nlohmann::json>();
        QVERIFY(message.is_array());
        QCOMPARE(message.size(), std::size_t(2));
        QVERIFY(message.at(0)["result"].value("ok", false));
        QCOMPARE(message.at(1)["error"]["code"].get<int>(), -32601);
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QVERIFY(waitForRequests(server, 1));
        QCOMPARE(nlohmann::json::parse(server.requestBodies().first().toStdString()), request);
    }

    void parsesSseStreamResponse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        QByteArray sseBody;
        sseBody += "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n";
        sseBody += "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n";

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = sseBody;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();

        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "ping";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(messageSpy, 2, 3000));
        QCOMPARE(
            QString::fromStdString(
                messageSpy.at(0).first().value<nlohmann::json>()["method"].get<std::string>()),
            QStringLiteral("notifications/x"));
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(failureSpy.size(), qsizetype(0));
    }

    void completedSseRequestClosesStreamWithoutEof()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n"
                                    "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n";
        response.sseChunkDelaysMs = {0};
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 41);

        QVERIFY(waitForSignal(messageSpy, 2));
        QCOMPARE(
            messageSpy.at(0).first().value<nlohmann::json>()["method"].get<std::string>(),
            std::string("notifications/x"));
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QVERIFY(waitForSignal(sentSpy, 1));
        QCOMPARE(sentSpy.first().first().toULongLong(), quint64(41));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
        QCOMPARE(sentSpy.size(), qsizetype(1));
    }

    void batchSseStreamWaitsForEveryResponse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n|"
                                    "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n\n";
        response.sseChunkDelaysMs = {0, 200};
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        transport.start();
        transport.sendMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            }));

        QVERIFY(waitForSignal(messageSpy, 1));
        QVERIFY(!transport.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(waitForSignal(messageSpy, 2));
        QCOMPARE(messageSpy.at(0).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 2);
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void abandonedBatchReplyClosesAfterLastRequest()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = ": hold\n\n";
        response.sseChunkDelaysMs = {0};
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        transport.start();
        transport.sendTrackedMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            }),
            51);

        QVERIFY(waitForRequests(server, 1));
        QCOMPARE(transport.findChildren<QNetworkReply *>().size(), qsizetype(1));
        transport.abandonRequest(1);
        QCOMPARE(transport.findChildren<QNetworkReply *>().size(), qsizetype(1));
        transport.abandonRequest(2);
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
    }

    void abandonedBatchRequestKeepsSiblingAlive()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = ": hold\n\n|"
                                    "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n\n";
        response.sseChunkDelaysMs = {0, 200};
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        transport.start();
        transport.sendTrackedMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            }),
            53);

        QVERIFY(waitForRequests(server, 1));
        transport.abandonRequest(1);
        QCOMPARE(transport.findChildren<QNetworkReply *>().size(), qsizetype(1));
        QVERIFY(waitForSignal(messageSpy, 1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 2);
        QVERIFY(waitForSignal(sentSpy, 1));
        QCOMPARE(sentSpy.first().first().toULongLong(), quint64(53));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
    }

    void abandonedBatchFailureReportsOnlySibling()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.statusLine  = "503 Service Unavailable";
        response.contentType = "text/plain";
        response.body        = "unavailable";
        response.delayMs     = 200;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        transport.start();
        transport.sendTrackedMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            }),
            52);

        QVERIFY(waitForRequests(server, 1));
        transport.abandonRequest(1);
        QVERIFY(waitForSignal(failureSpy, 1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(52));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{2});
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void abandonedTrackedTokenPreservesRequestOwner()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse notification;
        notification.contentType      = "text/event-stream";
        notification.body             = ": hold\n\n";
        notification.sseChunkDelaysMs = {0};
        notification.finishSse        = false;
        server.enqueue(notification);

        MockResponse request;
        request.contentType = "application/json";
        request.body        = R"({"jsonrpc":"2.0","id":1,"result":{"ok":true}})";
        request.bodyDelayMs = 300;
        server.enqueueForRequestId(1, request);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}}, 61);
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 62);

        QVERIFY(waitForRequests(server, 2));
        QTRY_COMPARE_WITH_TIMEOUT(runningReplyCount(&transport), qsizetype(2), 1000);
        transport.abandonTrackedMessage(60);
        QCOMPARE(runningReplyCount(&transport), qsizetype(2));
        transport.abandonTrackedMessage(61);
        QCOMPARE(runningReplyCount(&transport), qsizetype(1));
        transport.abandonTrackedMessage(62);
        QCOMPARE(runningReplyCount(&transport), qsizetype(1));

        QVERIFY(waitForSignal(messageSpy, 1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
        transport.abandonTrackedMessage(61);
        transport.abandonTrackedMessage(62);
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
    }

    void zeroTrackedTokenDoesNotReleaseReply()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse notification;
        notification.contentType      = "text/event-stream";
        notification.body             = ": hold\n\n";
        notification.sseChunkDelaysMs = {0};
        notification.finishSse        = false;
        server.enqueue(notification);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}}, 0);

        QVERIFY(waitForRequests(server, 1));
        QTRY_COMPARE_WITH_TIMEOUT(runningReplyCount(&transport), qsizetype(1), 1000);
        transport.abandonTrackedMessage(0);
        QCOMPARE(runningReplyCount(&transport), qsizetype(1));
        transport.stop();

        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void abandonedTrackedTokenClearsLateFailureToken()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.statusLine = "503 Service Unavailable";
        response.delayMs    = 300;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, 63);

        QVERIFY(waitForRequests(server, 1));
        QCOMPARE(runningReplyCount(&transport), qsizetype(1));
        transport.abandonTrackedMessage(63);
        QCOMPARE(runningReplyCount(&transport), qsizetype(1));

        QVERIFY(waitForSignal(failureSpy, 1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(0));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{2});
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void responseBeforeParseErrorCompletesSse_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QList<int>>("delays");

        QTest::newRow("same-chunk") << QByteArray(
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"
            "data: not-json\n\n") << QList<int>{0};
        QTest::newRow("later-chunk") << QByteArray(
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n|"
            "data: not-json\n\n") << QList<int>{0, 50};
    }

    void responseBeforeParseErrorCompletesSse()
    {
        QFETCH(QByteArray, body);
        QFETCH(QList<int>, delays);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = body;
        response.sseChunkDelaysMs = delays;
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 42);

        QVERIFY(waitForSignal(sentSpy, 1));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(sentSpy.first().first().toULongLong(), quint64(42));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void parseErrorBeforeResponseFailsSse_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QList<int>>("delays");

        QTest::newRow("same-chunk") << QByteArray(
            "data: not-json\n\n"
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n")
                                    << QList<int>{0};
        QTest::newRow("later-chunk") << QByteArray(
            "data: not-json\n\n|"
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n")
                                     << QList<int>{0, 50};
    }

    void parseErrorBeforeResponseFailsSse()
    {
        QFETCH(QByteArray, body);
        QFETCH(QList<int>, delays);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType      = "text/event-stream";
        response.body             = body;
        response.sseChunkDelaysMs = delays;
        response.finishSse        = false;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 43);

        QVERIFY(waitForSignal(failureSpy, 1));
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(43));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{1});
        QVERIFY(failureSpy.first().at(2).toString().contains(QStringLiteral("parse error")));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void sseEofFallbackUsesFirstTerminalEvent_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("succeeds");

        QTest::newRow("response-before-error") << QByteArray(
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"
            "data: not-json\n\n") << true;
        QTest::newRow("error-before-response") << QByteArray(
            "data: not-json\n\n"
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n")
                                               << false;
    }

    void sseEofFallbackUsesFirstTerminalEvent()
    {
        QFETCH(QByteArray, body);
        QFETCH(bool, succeeds);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = body;
        response.bodyDelayMs = 100;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 44);

        const auto replies = transport.findChildren<QNetworkReply *>();
        QCOMPARE(replies.size(), qsizetype(1));
        QObject::disconnect(replies.first(), nullptr, &transport, nullptr);
        QVERIFY(
            QObject::connect(
                replies.first(), SIGNAL(finished()), &transport, SLOT(onReplyFinished())));

        if (succeeds) {
            QVERIFY(waitForSignal(sentSpy, 1));
            QCOMPARE(messageSpy.size(), qsizetype(1));
            QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 1);
            QCOMPARE(failureSpy.size(), qsizetype(0));
        } else {
            QVERIFY(waitForSignal(failureSpy, 1));
            QCOMPARE(messageSpy.size(), qsizetype(0));
            QCOMPARE(sentSpy.size(), qsizetype(0));
            QVERIFY(failureSpy.first().at(2).toString().contains(QStringLiteral("parse error")));
        }
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void postWithoutJsonRpcRequestsAcceptsEmptyBody_data()
    {
        QTest::addColumn<QByteArray>("statusLine");
        QTest::addColumn<QByteArray>("outboundBody");

        QTest::newRow("notification-compat-200")
            << QByteArray("200 OK")
            << QByteArray(R"({"jsonrpc":"2.0","method":"notifications/x"})");
        QTest::newRow("notification-batch-202")
            << QByteArray("202 Accepted")
            << QByteArray(
                   R"([{"jsonrpc":"2.0","method":"notifications/x"},{"jsonrpc":"2.0","method":"notifications/y"}])");
        QTest::newRow("response-batch-202")
            << QByteArray("202 Accepted")
            << QByteArray(
                   R"([{"jsonrpc":"2.0","id":7,"result":{}},{"jsonrpc":"2.0","id":8,"error":{"code":-32601,"message":"Method not found"}}])");
    }

    void postWithoutJsonRpcRequestsAcceptsEmptyBody()
    {
        QFETCH(QByteArray, statusLine);
        QFETCH(QByteArray, outboundBody);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.statusLine  = statusLine;
        response.contentType = "application/json";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);

        transport.start();
        transport.sendTrackedMessage(nlohmann::json::parse(outboundBody.toStdString()), quint64(11));

        QVERIFY(waitForSignal(sentSpy, 1, 3000));
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.first().at(0).toULongLong(), quint64(11));
    }

    void requestWithoutResponseFailsAtEof_data()
    {
        QTest::addColumn<QByteArray>("contentType");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("messageCount");
        QTest::addColumn<QString>("errorText");

        QTest::newRow("empty-json") << QByteArray("application/json") << QByteArray() << 0
                                    << QStringLiteral("HTTP response ended before all");
        QTest::newRow("whitespace-json") << QByteArray("application/json") << QByteArray(" \r\n\t")
                                         << 0 << QStringLiteral("HTTP response ended before all");
        QTest::newRow("notification-only-json")
            << QByteArray("application/json")
            << QByteArray(R"({"jsonrpc":"2.0","method":"notifications/x"})") << 1
            << QStringLiteral("HTTP response ended before all");
        QTest::newRow("wrong-id-json") << QByteArray("application/json")
                                       << QByteArray(R"({"jsonrpc":"2.0","id":99,"result":{}})")
                                       << 1 << QStringLiteral("HTTP response ended before all");
        QTest::newRow("invalid-version-json")
            << QByteArray("application/json")
            << QByteArray(R"({"jsonrpc":"1.0","id":1,"result":{}})") << 1
            << QStringLiteral("HTTP response ended before all");
        QTest::newRow("invalid-error-json")
            << QByteArray("application/json")
            << QByteArray(R"({"jsonrpc":"2.0","id":1,"error":{"code":"bad","message":"x"}})") << 1
            << QStringLiteral("HTTP response ended before all");
        QTest::newRow("empty-array-json") << QByteArray("application/json") << QByteArray("[]") << 1
                                          << QStringLiteral("HTTP response ended before all");
        QTest::newRow("empty-sse") << QByteArray("text/event-stream") << QByteArray() << 0
                                   << QStringLiteral("SSE stream ended before all");
        QTest::newRow("comment-only-sse")
            << QByteArray("text/event-stream") << QByteArray(": keepalive\n") << 0
            << QStringLiteral("SSE stream ended before all");
        QTest::newRow("incomplete-sse")
            << QByteArray("text/event-stream")
            << QByteArray("data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n") << 0
            << QStringLiteral("SSE stream ended with an incomplete event");
        QTest::newRow("notification-only-sse")
            << QByteArray("text/event-stream")
            << QByteArray("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n") << 1
            << QStringLiteral("SSE stream ended before all");
    }

    void requestWithoutResponseFailsAtEof()
    {
        QFETCH(QByteArray, contentType);
        QFETCH(QByteArray, body);
        QFETCH(int, messageCount);
        QFETCH(QString, errorText);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = contentType;
        response.body        = body;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(messageSpy.size(), qsizetype(messageCount));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(0));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{1});
        QVERIFY(failureSpy.first().at(2).toString().contains(errorText));
    }

    void partialBatchFailsOnlyUnansweredIds_data()
    {
        QTest::addColumn<QByteArray>("contentType");
        QTest::addColumn<QByteArray>("body");

        const QByteArray response = R"([{"jsonrpc":"2.0","id":1,"result":{"ok":true}}])";
        QTest::newRow("json") << QByteArray("application/json") << response;
        QTest::newRow("sse") << QByteArray("text/event-stream")
                             << QByteArray("data: ") + response + "\n\n";
        QTest::newRow("sse-incomplete-tail")
            << QByteArray("text/event-stream")
            << QByteArray("data: ") + response
                   + "\n\ndata: [{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}]\n";
    }

    void partialBatchFailsOnlyUnansweredIds()
    {
        QFETCH(QByteArray, contentType);
        QFETCH(QByteArray, body);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = contentType;
        response.body        = body;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        transport.sendMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "first"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "second"}},
            }));

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        const auto message = messageSpy.first().first().value<nlohmann::json>();
        QVERIFY(message.is_array());
        QCOMPARE(message.size(), std::size_t(1));
        QCOMPARE(message.at(0)["id"].get<int>(), 1);
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{2});
    }

    void incompleteSseFailsTrackedNotification()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);

        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/current"}}, 31);

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(31));
        QVERIFY(failureSpy.first().at(1).value<QList<int>>().isEmpty());
        QVERIFY(failureSpy.first().at(2).toString().contains(QStringLiteral("incomplete event")));
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }

    void eofFailureCallbackCanStop()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);
        connect(&transport, &QSocMcpTransport::messageFailed, &transport, [&transport]() {
            transport.stop();
        });

        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, 17);

        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void eofFailureCallbackCanDeleteTransport()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        server.enqueue(response);

        QPointer<QSocMcpHttpTransport> transport = new QSocMcpHttpTransport(
            httpConfig(server.url()));
        int failureCount = 0;
        int sentCount    = 0;
        connect(transport, &QSocMcpTransport::messageFailed, &server, [&transport, &failureCount]() {
            failureCount++;
            delete transport.data();
        });
        connect(transport, &QSocMcpTransport::messageSent, &server, [&sentCount]() { sentCount++; });

        transport->start();
        transport->sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, 19);

        QTRY_VERIFY_WITH_TIMEOUT(transport.isNull(), 3000);
        QCOMPARE(failureCount, 1);
        QCOMPARE(sentCount, 0);
    }

    void eofFailureCallbackCanPostReentrantly()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse missing;
        missing.contentType = "application/json";
        server.enqueue(missing);

        MockResponse followup;
        followup.contentType = "application/json";
        followup.body        = R"({"jsonrpc":"2.0","id":2,"result":{"ok":true}})";
        server.enqueue(followup);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        bool                 posted = false;
        connect(&transport, &QSocMcpTransport::messageFailed, &transport, [&transport, &posted]() {
            if (posted) {
                return;
            }
            posted = true;
            transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
        });

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{1});
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 2);
    }

    void stopDuringSseNotificationSuppressesEofFailure()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);
        connect(
            &transport,
            &QSocMcpTransport::messageReceived,
            &transport,
            [&transport](const nlohmann::json &) { transport.stop(); });

        transport.start();
        transport.sendMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "first"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "second"}},
            }));

        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void clientFailsEmptyResponseWithDeadlineDisabled()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.statusLine = "202 Accepted";
        server.enqueue(initialized);

        MockResponse missing;
        missing.contentType = "application/json";
        server.enqueue(missing);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 0;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);

        client.start();
        QVERIFY(waitForSignal(readySpy, 1, 3000));
        const int requestId = client.request(QStringLiteral("tools/list"));
        QVERIFY(requestId > 0);

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QVERIFY(failureSpy.first().at(2).toString().contains(
            QStringLiteral("before all JSON-RPC responses arrived")));
        QCOMPARE(responseSpy.size(), qsizetype(0));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void clientRejectsEmptyInitializeResponseWithDeadlineDisabled()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse missing;
        missing.contentType = "application/json";
        server.enqueue(missing);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 0;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);

        client.start();

        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), 1);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QVERIFY(failureSpy.first().at(2).toString().contains(
            QStringLiteral("before all JSON-RPC responses arrived")));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void invalidJsonFailsMessage()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = "not json {{{";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);

        transport.start();
        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "ping";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(failureSpy, 1, 2000));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(0));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{1});
        QVERIFY(failureSpy.first().at(2).toString().startsWith(QStringLiteral("JSON parse error:")));
        QCOMPARE(errorSpy.size(), qsizetype(0));
    }

    void postBodyMatchesSentMessage()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = R"({"jsonrpc":"2.0","id":7,"result":{}})";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);

        transport.start();

        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 7;
        req["method"]  = "tools/list";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        QVERIFY(!server.requestBodies().isEmpty());
        const QByteArray sent = server.requestBodies().first();
        QVERIFY(sent.contains("\"method\":\"tools/list\""));
        QVERIFY(sent.contains("\"id\":7"));
    }

    void configuredHeadersCannotOverrideProtocolHeaders()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray staleSession = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        const QByteArray sessionId    = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        MockResponse     initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
        initialize.sessionId   = sessionId;
        server.enqueue(initialize);

        MockResponse probe;
        probe.contentType = "application/json";
        probe.body        = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        server.enqueue(probe);

        McpServerConfig config = httpConfig(server.url());
        config.headers.insert(QStringLiteral("aCcEpT"), QStringLiteral("text/plain"));
        config.headers.insert(QStringLiteral("Content-tYpE"), QStringLiteral("text/plain"));
        config.headers.insert(QStringLiteral("mCp-SeSsIoN-iD"), QString::fromUtf8(staleSession));
        config.headers.insert(QStringLiteral("X-Test-Mode"), QStringLiteral("enabled"));

        QSocMcpHttpTransport transport(config);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
        QVERIFY(waitForSignal(messageSpy, 2, 3000));

        const auto headers = server.requestHeaders();
        QCOMPARE(headers.size(), qsizetype(2));
        QCOMPARE(headers.at(0).value("accept"), QByteArray("application/json, text/event-stream"));
        QCOMPARE(headers.at(0).value("content-type"), QByteArray("application/json"));
        QVERIFY(!headers.at(0).contains("mcp-session-id"));
        QCOMPARE(headers.at(0).value("x-test-mode"), QByteArray("enabled"));
        QCOMPARE(headers.at(1).value("mcp-session-id"), sessionId);
    }

    void sessionless404RemainsLocalAfterSessionStarts()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse sessionlessFailure;
        sessionlessFailure.statusLine = "404 Not Found";
        sessionlessFailure.delayMs    = 500;
        server.enqueueForRequestId(1, sessionlessFailure);

        const QByteArray sessionId = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        MockResponse     initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        initialize.sessionId   = sessionId;
        server.enqueueForRequestId(2, initialize);

        MockResponse probe;
        probe.contentType = "application/json";
        probe.body        = R"({"jsonrpc":"2.0","id":3,"result":{}})";
        server.enqueueForRequestId(3, probe);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}});

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        QCOMPARE(messageSpy.first().at(0).value<nlohmann::json>()["id"].get<int>(), 2);
        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), (QList<int>{1}));
        QVERIFY(failureSpy.first().at(2).toString().contains(QStringLiteral("HTTP 404")));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(0));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Running);

        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}});
        QVERIFY(waitForSignal(messageSpy, 2, 3000));
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("ping")),
            (QList<QByteArray>{QByteArray(), sessionId}));
    }

    void sseCallbackCanPostReentrantly()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse streamResponse;
        streamResponse.contentType = "text/event-stream";
        streamResponse.body = "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n"
                              "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n";
        server.enqueue(streamResponse);

        MockResponse followupResponse;
        followupResponse.contentType = "application/json";
        followupResponse.body        = R"({"jsonrpc":"2.0","id":2,"result":{"ok":true}})";
        server.enqueue(followupResponse);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        bool                 followupSent = false;
        connect(
            &transport,
            &QSocMcpTransport::messageReceived,
            &transport,
            [&](const nlohmann::json &message) {
                if (followupSent || message.value("id", 0) != 1) {
                    return;
                }
                followupSent = true;
                transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
            });

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});

        QVERIFY(waitForSignal(messageSpy, 3, 3000));
        QVERIFY(followupSent);
        bool sawFollowup = false;
        for (const auto &arguments : messageSpy) {
            const auto message = arguments.first().value<nlohmann::json>();
            if (message.value("id", 0) == 2) {
                sawFollowup = message["result"].value("ok", false);
            }
        }
        QVERIFY(sawFollowup);
    }

    void stopDropsInflightReplySilently()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse delayed;
        delayed.contentType = "application/json";
        delayed.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
        delayed.delayMs     = 500;
        server.enqueue(delayed);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, 7);
        QVERIFY(waitForRequests(server, 1));
        transport.stop();

        MockResponse accepted;
        accepted.statusLine = "202 Accepted";
        server.enqueue(accepted);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/current"}}, 9);

        QVERIFY(waitForSignal(sentSpy, 1, 3000));
        QTest::qWait(600);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.first().at(0).toULongLong(), quint64(9));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Running);
        QVERIFY(transport.findChildren<QNetworkReply *>().isEmpty());
    }

    void replyFinishedReentrantStopClosesOnce()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse hanging;
        hanging.contentType      = "text/event-stream";
        hanging.body             = ": hold\n\n";
        hanging.sseChunkDelaysMs = {0};
        hanging.finishSse        = false;
        server.enqueue(hanging);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
        QVERIFY(waitForRequests(server, 1));

        const QList<QNetworkReply *> replies = transport.findChildren<QNetworkReply *>();
        QCOMPARE(replies.size(), qsizetype(1));
        QVERIFY(replies.first()->isRunning());
        int  reentrantStops       = 0;
        bool reenteredBeforeClose = false;
        connect(
            replies.first(),
            &QNetworkReply::finished,
            this,
            [&]() {
                reentrantStops++;
                reenteredBeforeClose = transport.state() == QSocMcpTransport::State::Stopping
                                       && closedSpy.isEmpty();
                transport.stop();
            },
            Qt::DirectConnection);

        transport.stop();

        QCOMPARE(reentrantStops, 1);
        QVERIFY(reenteredBeforeClose);
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
        QTRY_VERIFY_WITH_TIMEOUT(transport.findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void stopDuringSseCallbackDropsRemainingEvents()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"
                               "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n\n";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        connect(
            &transport,
            &QSocMcpTransport::messageReceived,
            &transport,
            [&transport](const nlohmann::json &) { transport.stop(); });

        transport.start();
        transport.sendTrackedMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            }),
            44);

        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QTest::qWait(100);
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }

    void networkErrorFailsMessageOnce()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        MockResponse response;
        response.closeWithoutResponse = true;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QTest::qWait(100);
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(0));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), QList<int>{1});
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QVERIFY(transport.findChildren<QNetworkReply *>().isEmpty());
    }

    void batchFailureCarriesEveryRequestId()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        MockResponse response;
        response.closeWithoutResponse = true;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        transport.start();
        transport.sendMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "first"}},
                {{"jsonrpc", "2.0"}, {"method", "notification"}},
                {{"jsonrpc", "2.0"}, {"id", 99}, {"result", nlohmann::json::object()}},
                {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "duplicate"}},
                {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "second"}},
            }));

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), (QList<int>{3, 7}));
    }

    void tracked202CompletionFollowsItsPost()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse accepted;
        accepted.statusLine = "202 Accepted";
        accepted.delayMs    = 500;
        server.enqueue(accepted);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/tracked"}}, 17);

        const int    requestId = 23;
        MockResponse response;
        response.contentType = "application/json";
        response.body        = QByteArray::fromStdString(
            nlohmann::json{{"jsonrpc", "2.0"}, {"id", requestId}, {"result", {{"ok", true}}}}.dump());
        server.enqueueForRequestId(requestId, response);
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", requestId}, {"method", "tools/list"}});

        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        QCOMPARE(messageSpy.first().at(0).value<nlohmann::json>()["id"].get<int>(), requestId);
        QCOMPARE(sentSpy.size(), qsizetype(0));

        QVERIFY(waitForSignal(sentSpy, 1, 3000));
        QCOMPARE(sentSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.first().at(0).toULongLong(), quint64(17));
    }

    void requestFailureDoesNotCancelConcurrentRequest_data()
    {
        QTest::addColumn<QByteArray>("statusLine");
        QTest::addColumn<QByteArray>("contentType");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("closeWithoutResponse");
        QTest::addColumn<bool>("seedSession");

        QTest::newRow("network") << QByteArray("200 OK") << QByteArray() << QByteArray() << true
                                 << false;
        QTest::newRow("http-503") << QByteArray("503 Service Unavailable")
                                  << QByteArray("text/plain") << QByteArray("unavailable") << false
                                  << false;
        QTest::newRow("http-404-sse")
            << QByteArray("404 Not Found") << QByteArray("text/event-stream") << QByteArray()
            << false << false;
        QTest::newRow("http-500-sse-session")
            << QByteArray("500 Internal Server Error") << QByteArray("text/event-stream")
            << QByteArray() << false << true;
        QTest::newRow("malformed-json") << QByteArray("200 OK") << QByteArray("application/json")
                                        << QByteArray("not json") << false << false;
    }

    void requestFailureDoesNotCancelConcurrentRequest()
    {
        QFETCH(QByteArray, statusLine);
        QFETCH(QByteArray, contentType);
        QFETCH(QByteArray, body);
        QFETCH(bool, closeWithoutResponse);
        QFETCH(bool, seedSession);

        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        if (seedSession) {
            initialize.sessionId = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        }
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.statusLine = "202 Accepted";
        server.enqueue(initialized);

        auto         *transport = new QSocMcpHttpTransport(httpConfig(server.url()));
        QSocMcpClient client(httpConfig(server.url()), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    wireMessageSpy(transport, &QSocMcpTransport::messageReceived);
        QSignalSpy    wireFailureSpy(transport, &QSocMcpTransport::messageFailed);
        QSignalSpy    transportErrorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy    transportClosedSpy(transport, &QSocMcpTransport::closed);
        QSignalSpy    clientClosedSpy(&client, &QSocMcpClient::closed);

        client.start();
        QVERIFY(waitForSignal(readySpy, 1, 3000));
        wireMessageSpy.clear();
        wireFailureSpy.clear();

        const int    failedId = client.request(QStringLiteral("fails"));
        MockResponse failed;
        failed.statusLine           = statusLine;
        failed.contentType          = contentType;
        failed.body                 = body;
        failed.closeWithoutResponse = closeWithoutResponse;
        if (contentType == QByteArrayLiteral("text/event-stream")) {
            failed.body             = QByteArrayLiteral("data: ")
                                      + QByteArray::fromStdString(
                                          nlohmann::json{
                                              {"jsonrpc", "2.0"},
                                              {"id", failedId},
                                              {"result", {{"accepted", true}}},
                                          }
                                              .dump())
                                      + QByteArrayLiteral("\n\n|: hold\n\n");
            failed.sseChunkDelaysMs = {0, 250};
        }
        server.enqueueForRequestId(failedId, failed);

        const int    successId = client.request(QStringLiteral("succeeds"));
        MockResponse success;
        success.contentType = "application/json";
        success.delayMs     = 500;
        success.body        = QByteArray::fromStdString(
            nlohmann::json{{"jsonrpc", "2.0"}, {"id", successId}, {"result", {{"ok", true}}}}.dump());
        server.enqueueForRequestId(successId, success);

        QVERIFY(waitForSignal(wireFailureSpy, 1, 3000));
        QCOMPARE(wireMessageSpy.size(), qsizetype(0));
        QCOMPARE(responseSpy.size(), qsizetype(0));
        QCOMPARE(wireFailureSpy.size(), qsizetype(1));
        QCOMPARE(wireFailureSpy.first().at(0).toULongLong(), quint64(0));
        QCOMPARE(wireFailureSpy.first().at(1).value<QList<int>>(), (QList<int>{failedId}));
        QVERIFY(!wireFailureSpy.first().at(2).toString().isEmpty());
        QCOMPARE(transportErrorSpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), failedId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QVERIFY(!failureSpy.first().at(2).toString().isEmpty());
        QVERIFY(waitForSignal(responseSpy, 1, 3000));
        QCOMPARE(wireMessageSpy.size(), qsizetype(1));
        QCOMPARE(wireFailureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(responseSpy.first().at(0).toInt(), successId);
        QVERIFY(responseSpy.first().at(1).value<nlohmann::json>().value("ok", false));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int    nextId = client.request(QStringLiteral("after-failure"));
        MockResponse next;
        next.contentType = "application/json";
        next.body        = QByteArray::fromStdString(
            nlohmann::json{{"jsonrpc", "2.0"}, {"id", nextId}, {"result", {{"ok", true}}}}.dump());
        server.enqueueForRequestId(nextId, next);

        QVERIFY(waitForSignal(responseSpy, 2, 3000));
        QCOMPARE(wireMessageSpy.size(), qsizetype(2));
        QCOMPARE(wireFailureSpy.size(), qsizetype(1));
        QCOMPARE(responseSpy.last().at(0).toInt(), nextId);
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        QCOMPARE(transportClosedSpy.size(), qsizetype(0));
        QCOMPARE(clientClosedSpy.size(), qsizetype(0));
    }

    void expiredSessionRebuildsWithoutReplay()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray sessionA = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        const QByteArray sessionB = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);

        MockResponse initializeA;
        initializeA.contentType = "application/json";
        initializeA.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        initializeA.sessionId   = sessionA;
        server.enqueue(initializeA);

        MockResponse initializedA;
        initializedA.statusLine = "202 Accepted";
        server.enqueue(initializedA);

        MockResponse toolsA;
        toolsA.contentType = "application/json";
        toolsA.body        = R"({"jsonrpc":"2.0","id":2,"result":{"tools":[]}})";
        server.enqueue(toolsA);

        const McpServerConfig                 config = httpConfig(server.url());
        QList<QPointer<QSocMcpHttpTransport>> transports;
        QSocMcpManager                        manager({config});
        manager.setTransportFactory([&transports](const McpServerConfig &cfg) {
            auto *transport = new QSocMcpHttpTransport(cfg);
            transports.append(transport);
            return transport;
        });
        manager.setReconnectDelays(10, 10);

        QCOMPARE(transports.size(), qsizetype(1));
        auto *oldTransport = transports.first().data();
        auto *oldClient    = manager.findClient(QStringLiteral("http"));
        QVERIFY(oldTransport != nullptr);
        QVERIFY(oldClient != nullptr);

        QSignalSpy oldReadySpy(oldClient, &QSocMcpClient::ready);
        QSignalSpy oldResponseSpy(oldClient, &QSocMcpClient::responseReceived);
        QSignalSpy oldFailureSpy(oldClient, &QSocMcpClient::requestFailed);
        QSignalSpy oldClientClosedSpy(oldClient, &QSocMcpClient::closed);
        QSignalSpy oldErrorSpy(oldTransport, &QSocMcpTransport::errorOccurred);
        QSignalSpy oldWireFailureSpy(oldTransport, &QSocMcpTransport::messageFailed);
        QSignalSpy oldTransportClosedSpy(oldTransport, &QSocMcpTransport::closed);
        QSignalSpy reconnectSpy(&manager, &QSocMcpManager::reconnectScheduled);

        manager.startAll();
        QVERIFY(waitForSignal(oldReadySpy, 1, 3000));
        QVERIFY(waitForSignal(oldResponseSpy, 1, 3000));
        QVERIFY(waitForRequests(server, 3, 3000));

        const int expiredId = oldClient->request(QStringLiteral("tools/call"));
        QVERIFY(expiredId > 0);
        MockResponse expired;
        expired.statusLine  = "404 Not Found";
        expired.contentType = "text/plain";
        expired.body        = "expired";
        expired.delayMs     = 100;
        expired.bodyDelayMs = 3000;
        server.enqueueForRequestId(expiredId, expired);

        const int concurrentId = oldClient->request(QStringLiteral("concurrent"));
        QVERIFY(concurrentId > 0);
        MockResponse concurrent;
        concurrent.contentType = "application/json";
        concurrent.body        = QByteArray::fromStdString(
            nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", concurrentId},
                {"result", {{"late", true}}},
            }
                .dump());
        concurrent.bodyDelayMs = 800;
        server.enqueueForRequestId(concurrentId, concurrent);

        MockResponse initializeB;
        initializeB.contentType = "application/json";
        initializeB.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        initializeB.sessionId   = sessionB;
        server.enqueue(initializeB);

        MockResponse initializedB;
        initializedB.statusLine = "202 Accepted";
        server.enqueue(initializedB);

        MockResponse toolsB;
        toolsB.contentType = "application/json";
        toolsB.body        = R"({"jsonrpc":"2.0","id":2,"result":{"tools":[]}})";
        server.enqueue(toolsB);

        QVERIFY(waitForRequests(server, 5, 3000));
        QVERIFY(waitForSignal(oldClientClosedSpy, 1, 1500));
        QVERIFY(waitForSignal(reconnectSpy, 1, 1500));
        QVERIFY(
            QTest::qWaitFor(
                [&]() {
                    return transports.size() == 2
                           && manager.findClient(QStringLiteral("http")) != nullptr
                           && manager.findClient(QStringLiteral("http")) != oldClient;
                },
                1000));

        QPointer<QSocMcpClient> newClient = manager.findClient(QStringLiteral("http"));
        QVERIFY(!newClient.isNull());
        QVERIFY(
            QTest::qWaitFor(
                [&]() {
                    return !newClient.isNull() && newClient->state() == QSocMcpClient::State::Ready
                           && server.requestBodies().size() >= 8;
                },
                3000));

        QCOMPARE(oldErrorSpy.size(), qsizetype(1));
        QVERIFY(oldErrorSpy.first().at(0).toString().contains(QStringLiteral("HTTP 404")));
        QVERIFY(oldErrorSpy.first().at(0).toString().contains(QStringLiteral("session expired")));
        QCOMPARE(oldWireFailureSpy.size(), qsizetype(0));
        QCOMPARE(oldTransportClosedSpy.size(), qsizetype(1));
        QCOMPARE(oldClientClosedSpy.size(), qsizetype(1));
        QCOMPARE(reconnectSpy.size(), qsizetype(1));
        QCOMPARE(oldFailureSpy.size(), qsizetype(2));

        QHash<int, int> failureCounts;
        for (const auto &arguments : oldFailureSpy) {
            failureCounts[arguments.at(0).toInt()]++;
            QCOMPARE(arguments.at(1).toInt(), -32000);
            QVERIFY(arguments.at(2).toString().contains(QStringLiteral("HTTP 404")));
            QVERIFY(arguments.at(2).toString().contains(QStringLiteral("session expired")));
        }
        QCOMPARE(failureCounts.value(expiredId), 1);
        QCOMPARE(failureCounts.value(concurrentId), 1);

        QCOMPARE(server.requestBodies().size(), qsizetype(8));
        QCOMPARE(server.requestBodies().size(), server.requestSessionIds().size());
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("initialize")),
            (QList<QByteArray>{QByteArray(), QByteArray()}));
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("notifications/initialized")),
            (QList<QByteArray>{sessionA, sessionB}));
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("tools/list")),
            (QList<QByteArray>{sessionA, sessionB}));
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("tools/call")),
            (QList<QByteArray>{sessionA}));
        QCOMPARE(
            requestSessionsForMethod(server, QStringLiteral("concurrent")),
            (QList<QByteArray>{sessionA}));

        QTest::qWait(900);
        QCOMPARE(server.requestBodies().size(), qsizetype(8));
        QCOMPARE(oldFailureSpy.size(), qsizetype(2));
        QCOMPARE(oldErrorSpy.size(), qsizetype(1));
        QCOMPARE(oldTransportClosedSpy.size(), qsizetype(1));
        QCOMPARE(oldClientClosedSpy.size(), qsizetype(1));
        QCOMPARE(reconnectSpy.size(), qsizetype(1));
    }

    void sessionNotification404RejectsReentrantStart()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
        initialize.sessionId   = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        server.enqueue(initialize);

        MockResponse expired;
        expired.statusLine  = "404 Not Found";
        expired.contentType = "text/plain";
        expired.body        = "expired";
        expired.bodyDelayMs = 3000;
        server.enqueue(expired);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        QSignalSpy           startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        messageSpy.clear();
        connect(&transport, &QSocMcpTransport::errorOccurred, &transport, [&transport]() {
            transport.start();
        });

        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}}, 7);
        QVERIFY(waitForSignal(closedSpy, 1, 1500));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().at(0).toString().contains(QStringLiteral("HTTP 404")));
        QVERIFY(errorSpy.first().at(0).toString().contains(QStringLiteral("session expired")));
        QCOMPARE(startedSpy.size(), qsizetype(1));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void concurrentSession404ClosesOnce()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);
        QVERIFY(establishSession(
            server, transport, messageSpy, QUuid::createUuid().toByteArray(QUuid::WithoutBraces)));
        messageSpy.clear();

        MockResponse first;
        first.statusLine  = "404 Not Found";
        first.body        = "expired";
        first.delayMs     = 100;
        first.bodyDelayMs = 3000;
        server.enqueueForRequestId(2, first);

        MockResponse second = first;
        server.enqueueForRequestId(3, second);

        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "first"}});
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "second"}});
        QVERIFY(waitForRequests(server, 3, 3000));
        QVERIFY(waitForSignal(closedSpy, 1, 1500));
        QTest::qWait(300);

        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
        QVERIFY(transport.findChildren<QNetworkReply *>().isEmpty());
    }

    void deleteFromSessionErrorIsSafe()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        QObject owner;
        auto   *transport = new QSocMcpHttpTransport(httpConfig(server.url()), &owner);
        QPointer<QSocMcpHttpTransport> guard(transport);
        QSignalSpy                     messageSpy(transport, &QSocMcpTransport::messageReceived);
        QSignalSpy                     errorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy                     failureSpy(transport, &QSocMcpTransport::messageFailed);
        QSignalSpy                     closedSpy(transport, &QSocMcpTransport::closed);
        QVERIFY(establishSession(
            server, *transport, messageSpy, QUuid::createUuid().toByteArray(QUuid::WithoutBraces)));
        messageSpy.clear();

        MockResponse expired;
        expired.statusLine  = "404 Not Found";
        expired.body        = "expired";
        expired.bodyDelayMs = 500;
        server.enqueue(expired);

        connect(transport, &QSocMcpTransport::errorOccurred, transport, [&](const QString &) {
            delete transport;
            transport = nullptr;
        });
        transport->sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "delete"}});

        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 3000);
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(0));
    }

    void sessionIdDoesNotCrossRestart()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray sessionId = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        MockResponse     initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
        initialize.sessionId   = sessionId;
        server.enqueue(initialize);

        MockResponse followup;
        followup.contentType = "application/json";
        followup.body        = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        server.enqueue(followup);

        MockResponse restarted;
        restarted.contentType = "application/json";
        restarted.body        = R"({"jsonrpc":"2.0","id":3,"result":{}})";
        server.enqueue(restarted);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
        QVERIFY(waitForSignal(messageSpy, 2, 3000));

        QCOMPARE(server.requestSessionIds().size(), qsizetype(2));
        QVERIFY(server.requestSessionIds().at(0).isEmpty());
        QCOMPARE(server.requestSessionIds().at(1), sessionId);

        transport.stop();
        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "initialize"}});
        QVERIFY(waitForSignal(messageSpy, 3, 3000));

        QCOMPARE(server.requestSessionIds().size(), qsizetype(3));
        QVERIFY(server.requestSessionIds().at(2).isEmpty());
    }

    void sessionIdComesOnlyFromSuccessfulInitialize_data()
    {
        QTest::addColumn<bool>("seedSession");
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("jsonRpcVersion");
        QTest::addColumn<QByteArray>("statusLine");
        QTest::addColumn<bool>("validSessionId");
        QTest::addColumn<bool>("adoptCandidate");
        QTest::addColumn<bool>("candidateFails");

        QTest::newRow("initialize-created")
            << false << QStringLiteral("initialize") << QStringLiteral("2.0")
            << QByteArray("201 Created") << true << true << false;
        QTest::newRow("initialize-existing")
            << true << QStringLiteral("initialize") << QStringLiteral("2.0")
            << QByteArray("201 Created") << true << false << false;
        QTest::newRow("ordinary-empty") << false << QStringLiteral("ping") << QStringLiteral("2.0")
                                        << QByteArray("200 OK") << true << false << false;
        QTest::newRow("ordinary-existing")
            << true << QStringLiteral("ping") << QStringLiteral("2.0") << QByteArray("200 OK")
            << true << false << false;
        QTest::newRow("failed-initialize-empty")
            << false << QStringLiteral("initialize") << QStringLiteral("2.0")
            << QByteArray("500 Internal Server Error") << true << false << true;
        QTest::newRow("failed-initialize-existing")
            << true << QStringLiteral("initialize") << QStringLiteral("2.0")
            << QByteArray("500 Internal Server Error") << true << false << true;
        QTest::newRow("invalid-initialize-empty")
            << false << QStringLiteral("initialize") << QStringLiteral("2.0")
            << QByteArray("200 OK") << false << false << true;
        QTest::newRow("invalid-initialize-existing")
            << true << QStringLiteral("initialize") << QStringLiteral("2.0") << QByteArray("200 OK")
            << false << false << true;
        QTest::newRow("missing-jsonrpc") << false << QStringLiteral("initialize") << QString()
                                         << QByteArray("200 OK") << true << false << false;
        QTest::newRow("wrong-jsonrpc")
            << false << QStringLiteral("initialize") << QStringLiteral("1.0")
            << QByteArray("200 OK") << true << false << false;
    }

    void sessionIdComesOnlyFromSuccessfulInitialize()
    {
        QFETCH(bool, seedSession);
        QFETCH(QString, method);
        QFETCH(QString, jsonRpcVersion);
        QFETCH(QByteArray, statusLine);
        QFETCH(bool, validSessionId);
        QFETCH(bool, adoptCandidate);
        QFETCH(bool, candidateFails);

        MockHttpServer server;
        QVERIFY(server.listen());

        const auto makeSessionId = []() {
            return QByteArrayLiteral("!") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces)
                   + QByteArrayLiteral("~");
        };
        const auto responseBody = [](int id) {
            return QByteArray::fromStdString(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"ok", true}}}}.dump());
        };

        const QByteArray seedId      = makeSessionId();
        QByteArray       candidateId = makeSessionId();
        if (!validSessionId) {
            candidateId.insert(candidateId.size() / 2, ' ');
        }

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);

        transport.start();
        QVERIFY(waitForSignal(startedSpy, 1));

        int nextId       = 1;
        int messageCount = 0;
        if (seedSession) {
            MockResponse seed;
            seed.contentType = "application/json";
            seed.body        = responseBody(nextId);
            seed.sessionId   = seedId;
            server.enqueueForRequestId(nextId, seed);
            transport.sendMessage({{"jsonrpc", "2.0"}, {"id", nextId}, {"method", "initialize"}});
            ++nextId;
            ++messageCount;
            QVERIFY(waitForSignal(messageSpy, messageCount, 3000));
        }

        const int    candidateRequestId = nextId++;
        MockResponse candidate;
        candidate.statusLine  = statusLine;
        candidate.contentType = "application/json";
        candidate.body        = responseBody(candidateRequestId);
        candidate.sessionId   = candidateId;
        server.enqueueForRequestId(candidateRequestId, candidate);
        nlohmann::json candidateMessage{{"id", candidateRequestId}, {"method", method.toStdString()}};
        if (!jsonRpcVersion.isNull()) {
            candidateMessage["jsonrpc"] = jsonRpcVersion.toStdString();
        }
        constexpr quint64 candidateToken = 37;
        transport.sendTrackedMessage(candidateMessage, candidateToken);
        if (candidateFails) {
            QVERIFY(waitForSignal(failureSpy, 1, 3000));
            QCOMPARE(failureSpy.first().at(0).toULongLong(), candidateToken);
            QCOMPARE(failureSpy.first().at(1).value<QList<int>>(), (QList<int>{candidateRequestId}));
            const QString failureMessage = failureSpy.first().at(2).toString();
            QVERIFY(!failureMessage.isEmpty());
            if (!validSessionId) {
                QVERIFY(failureMessage.contains(QStringLiteral("session"), Qt::CaseInsensitive));
                QVERIFY(!failureMessage.contains(QString::fromLatin1(candidateId)));
            }
        } else {
            ++messageCount;
            QVERIFY(waitForSignal(messageSpy, messageCount, 3000));
            QVERIFY(waitForSignal(sentSpy, 1, 3000));
        }

        const int    probeRequestId = nextId;
        MockResponse probe;
        probe.contentType = "application/json";
        probe.body        = responseBody(probeRequestId);
        server.enqueueForRequestId(probeRequestId, probe);
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", probeRequestId}, {"method", "ping"}});
        ++messageCount;
        QVERIFY(waitForSignal(messageSpy, messageCount, 3000));
        QVERIFY(waitForRequests(server, seedSession ? 3 : 2, 3000));

        const qsizetype candidateIndex = seedSession ? 1 : 0;
        const qsizetype probeIndex     = candidateIndex + 1;
        QCOMPARE(server.requestSessionIds().size(), probeIndex + 1);
        if (seedSession) {
            QVERIFY(server.requestSessionIds().first().isEmpty());
            QCOMPARE(server.requestSessionIds().at(candidateIndex), seedId);
        } else {
            QVERIFY(server.requestSessionIds().at(candidateIndex).isEmpty());
        }
        const QByteArray expectedProbeId = adoptCandidate ? candidateId
                                                          : (seedSession ? seedId : QByteArray());
        QCOMPARE(server.requestSessionIds().at(probeIndex), expectedProbeId);
        QCOMPARE(failureSpy.size(), candidateFails ? qsizetype(1) : qsizetype(0));
        QCOMPARE(sentSpy.size(), candidateFails ? qsizetype(0) : qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(messageCount));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Running);
    }

    void sessionIdArrivesBeforeDelayedBody()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray sessionId = QByteArrayLiteral("!")
                                     + QUuid::createUuid().toByteArray(QUuid::WithoutBraces)
                                     + QByteArrayLiteral("~");
        MockResponse     initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{}})";
        initialize.sessionId   = sessionId;
        initialize.bodyDelayMs = 1000;
        server.enqueueForRequestId(1, initialize);

        MockResponse probe;
        probe.contentType = "application/json";
        probe.body        = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        server.enqueueForRequestId(2, probe);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});

        const auto sessionHeaderArrived = [&transport]() {
            const auto replies = transport.findChildren<QNetworkReply *>();
            for (const QNetworkReply *reply : replies) {
                if (reply->hasRawHeader(QByteArrayLiteral("Mcp-Session-Id"))) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(sessionHeaderArrived(), 500);
        QCOMPARE(messageSpy.size(), qsizetype(0));

        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
        QVERIFY(waitForRequests(server, 2, 1000));
        QCOMPARE(server.requestSessionIds().at(1), sessionId);
        QVERIFY(waitForSignal(messageSpy, 2, 3000));
        QCOMPARE(failureSpy.size(), qsizetype(0));
    }

    void fractionalRequestIdCanEstablishSession()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray sessionId = QByteArrayLiteral("!")
                                     + QUuid::createUuid().toByteArray(QUuid::WithoutBraces)
                                     + QByteArrayLiteral("~");
        MockResponse     initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1.5,"result":{}})";
        initialize.sessionId   = sessionId;
        server.enqueue(initialize);

        MockResponse probe;
        probe.contentType = "application/json";
        probe.body        = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        server.enqueue(probe);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1.5}, {"method", "initialize"}});
        QVERIFY(waitForSignal(messageSpy, 1, 3000));
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
        QVERIFY(waitForSignal(messageSpy, 2, 3000));
        QVERIFY(waitForRequests(server, 2, 3000));
        QVERIFY(server.requestSessionIds().first().isEmpty());
        QCOMPARE(server.requestSessionIds().at(1), sessionId);
        QCOMPARE(failureSpy.size(), qsizetype(0));
    }

    void clientAnswersServerPingFromSse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        const QByteArray sessionId = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        MockResponse     initialize;
        initialize.contentType = "text/event-stream";
        initialize.sessionId   = sessionId;
        initialize.body
            = "data: {\"jsonrpc\":\"2.0\",\"id\":\"server-ping\",\"method\":\"ping\"}\n\n"
              "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}\n\n";
        server.enqueue(initialize);

        MockResponse pingResponse;
        pingResponse.statusLine = "202 Accepted";
        server.enqueue(pingResponse);

        MockResponse initialized;
        initialized.statusLine = "202 Accepted";
        server.enqueue(initialized);

        auto         *transport = new QSocMcpHttpTransport(httpConfig(server.url()));
        QSocMcpClient client(httpConfig(server.url()), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);

        client.start();
        QVERIFY(waitForSignal(readySpy, 1, 3000));
        QVERIFY(waitForRequests(server, 3, 3000));

        bool sawPingResponse = false;
        bool sawInitialized  = false;
        for (qsizetype i = 1; i < server.requestBodies().size(); ++i) {
            const auto message = nlohmann::json::parse(server.requestBodies().at(i).toStdString());
            if (message.value("id", std::string()) == "server-ping") {
                sawPingResponse = message["result"].is_object() && message["result"].empty();
            }
            if (message.value("method", std::string()) == "notifications/initialized") {
                sawInitialized = true;
            }
        }
        QVERIFY(sawPingResponse);
        QVERIFY(sawInitialized);
        QCOMPARE(failureSpy.size(), 0);
        QCOMPARE(server.requestSessionIds().size(), qsizetype(3));
        QCOMPARE(server.requestSessionIds().at(1), sessionId);
        QCOMPARE(server.requestSessionIds().at(2), sessionId);
    }

    void clientWaitsForInitializedDelivery()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.contentType = "application/json";
        initialized.delayMs     = 300;
        server.enqueue(initialized);

        auto         *transport = new QSocMcpHttpTransport(httpConfig(server.url()));
        QSocMcpClient client(httpConfig(server.url()), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);

        client.start();
        QVERIFY(waitForRequests(server, 2, 3000));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(readySpy.size(), qsizetype(0));

        QVERIFY(waitForSignal(readySpy, 1, 3000));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void clientRejectsFailedInitializedDelivery()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.closeWithoutResponse = true;
        server.enqueue(initialized);

        auto         *transport = new QSocMcpHttpTransport(httpConfig(server.url()));
        QSocMcpClient client(httpConfig(server.url()), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);

        client.start();
        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void clientTimesOutInitializedDelivery()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.contentType = "application/json";
        initialized.delayMs     = 1000;
        server.enqueue(initialized);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 150;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);

        client.start();
        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void clientTimeoutReleasesInitializedReplyBeforeCallbacks()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.contentType      = "text/event-stream";
        initialized.body             = ": hold\n\n";
        initialized.sseChunkDelaysMs = {0};
        initialized.finishSse        = false;
        server.enqueue(initialized);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 1000;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        bool          stoppedBeforeCallback = false;
        connect(&client, &QSocMcpClient::stateChanged, &client, [&](QSocMcpClient::State state) {
            if (state != QSocMcpClient::State::Failed) {
                return;
            }
            stoppedBeforeCallback = runningReplyCount(transport) == 0;
        });

        client.start();

        QVERIFY(waitForRequests(server, 2, 3000));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QTRY_COMPARE_WITH_TIMEOUT(runningReplyCount(transport), qsizetype(1), 500);
        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QVERIFY(stoppedBeforeCallback);
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(1).toInt(), -32001);
        QCOMPARE(
            failureSpy.first().at(2).toString(),
            QStringLiteral("Initialized notification timed out"));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
        QTRY_VERIFY_WITH_TIMEOUT(transport->findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void clientStopReleasesInitializedReplyBeforeCallbacks()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.contentType      = "text/event-stream";
        initialized.body             = ": hold\n\n";
        initialized.sseChunkDelaysMs = {0};
        initialized.finishSse        = false;
        server.enqueue(initialized);

        McpServerConfig cfg       = httpConfig(server.url());
        auto           *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient   client(cfg, transport);
        QSignalSpy      readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy      failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy      closedSpy(&client, &QSocMcpClient::closed);

        client.start();
        QVERIFY(waitForRequests(server, 2, 3000));
        QTRY_COMPARE_WITH_TIMEOUT(runningReplyCount(transport), qsizetype(1), 1000);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        bool stoppedBeforeCallback = false;
        connect(&client, &QSocMcpClient::stateChanged, &client, [&](QSocMcpClient::State state) {
            if (state != QSocMcpClient::State::Disconnected) {
                return;
            }
            stoppedBeforeCallback = runningReplyCount(transport) == 0;
        });

        client.stop();

        QVERIFY(stoppedBeforeCallback);
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
        QTRY_VERIFY_WITH_TIMEOUT(transport->findChildren<QNetworkReply *>().isEmpty(), 1000);
    }

    void timedOutRequestReleasesHttpReply()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.statusLine = "202 Accepted";
        server.enqueue(initialized);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 150;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);

        client.start();
        QVERIFY(waitForSignal(readySpy, 1, 10000));

        const int requestId = client.request(QStringLiteral("slow"));
        QVERIFY(requestId > 0);

        MockResponse hanging;
        hanging.contentType      = "text/event-stream";
        hanging.body             = ": hold\n\n";
        hanging.sseChunkDelaysMs = {0};
        hanging.finishSse        = false;
        server.enqueueForRequestId(requestId, hanging);

        MockResponse cancelled;
        cancelled.statusLine = "202 Accepted";
        server.enqueue(cancelled);

        QVERIFY(waitForSignal(failureSpy, 1, 10000));
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32001);
        QCOMPARE(responseSpy.size(), qsizetype(0));
        QVERIFY(waitForRequests(server, 4, 10000));
        const auto cancellation = nlohmann::json::parse(server.requestBodies().last().toStdString());
        QCOMPARE(cancellation.value("method", std::string()), "notifications/cancelled");
        QCOMPARE(cancellation["params"].value("requestId", -1), requestId);
        QTRY_VERIFY_WITH_TIMEOUT(transport->findChildren<QNetworkReply *>().isEmpty(), 1000);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void requestOverrideOutlivesTransportDefault()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse initialize;
        initialize.contentType = "application/json";
        initialize.body        = R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})";
        server.enqueue(initialize);

        MockResponse initialized;
        initialized.contentType = "application/json";
        server.enqueue(initialized);

        MockResponse response;
        response.contentType = "application/json";
        response.body        = R"({"jsonrpc":"2.0","id":2,"result":{"ok":true}})";
        response.delayMs     = 300;
        server.enqueue(response);

        McpServerConfig cfg     = httpConfig(server.url());
        cfg.requestTimeoutMs    = 150;
        auto         *transport = new QSocMcpHttpTransport(cfg);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);

        client.start();
        QVERIFY(waitForSignal(readySpy, 1, 10000));
        QVERIFY(client.request(QStringLiteral("slow"), nlohmann::json::object(), 1000) > 0);

        QVERIFY(waitForSignal(responseSpy, 1, 10000));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QVERIFY(responseSpy.first().at(1).value<nlohmann::json>().value("ok", false));
    }

    void trackedJsonParseErrorDoesNotComplete()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = "not json";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport
            .sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, 7);

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QTest::qWait(100);
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(7));
        QVERIFY(failureSpy.first().at(1).value<QList<int>>().isEmpty());
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }

    void trackedSseParseErrorDoesNotComplete()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body = "data: not-json\n\n|data: {\"jsonrpc\":\"2.0\",\"method\":\"x\"}\n\n";
        response.sseChunkDelaysMs = {0, 100};
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           failureSpy(&transport, &QSocMcpTransport::messageFailed);
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport
            .sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, 9);

        QVERIFY(waitForSignal(failureSpy, 1, 3000));
        QTest::qWait(300);
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toULongLong(), quint64(9));
        QVERIFY(failureSpy.first().at(1).value<QList<int>>().isEmpty());
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmcphttp.moc"
