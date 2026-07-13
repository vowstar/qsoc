// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcphttp.h"
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
    int        delayMs              = 0;
    bool       closeWithoutResponse = false;
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

    QList<QByteArray> requestBodies() const { return requestBodies_; }

    QList<QByteArray> requestSessionIds() const { return requestSessionIds_; }

private slots:
    void onNewConnection()
    {
        while (server_->hasPendingConnections()) {
            auto *socket = server_->nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
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

        qsizetype  contentLength = 0;
        QByteArray sessionId;
        for (const QByteArray &line : buf.left(headerEnd).split('\n')) {
            QByteArray trimmed = line;
            if (trimmed.endsWith('\r')) {
                trimmed.chop(1);
            }
            if (trimmed.toLower().startsWith("content-length:")) {
                contentLength = trimmed.mid(15).trimmed().toLongLong();
            }
            if (trimmed.toLower().startsWith("mcp-session-id:")) {
                sessionId = trimmed.mid(15).trimmed();
            }
        }

        if (buf.size() < headerEnd + 4 + contentLength) {
            return;
        }

        const QByteArray body = buf.mid(headerEnd + 4, contentLength);
        requestBodies_ << body;
        requestSessionIds_ << sessionId;
        buffers_.remove(socket);

        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response = responses_.dequeue();
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
        http += response.body;
        socket->write(http);
        socket->flush();
        socket->disconnectFromHost();
    }

    void sendChunkedSse(QTcpSocket *socket, const MockResponse &response)
    {
        QByteArray header = "HTTP/1.1 200 OK\r\n";
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

    QTcpServer                     *server_ = nullptr;
    QQueue<MockResponse>            responses_;
    QList<QByteArray>               requestBodies_;
    QList<QByteArray>               requestSessionIds_;
    QHash<QTcpSocket *, QByteArray> buffers_;
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
        QVERIFY(waitForRequests(server, 1));
        QCOMPARE(nlohmann::json::parse(server.requestBodies().first().toStdString()), request);
    }

    void parsesSseStreamResponse()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        QByteArray sseBody;
        sseBody += "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n";
        sseBody += "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n";

        MockResponse response;
        response.contentType = "text/event-stream";
        response.body        = sseBody;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);

        transport.start();

        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "ping";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(messageSpy, 2, 3000));
        QCOMPARE(messageSpy.at(0).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(
            QString::fromStdString(
                messageSpy.at(1).first().value<nlohmann::json>()["method"].get<std::string>()),
            QStringLiteral("notifications/x"));
    }

    void emptyJsonBodyIsTolerated()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = "";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);

        transport.start();
        nlohmann::json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "ping";
        transport.sendMessage(notif);

        QTest::qWait(300);
        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(0));
    }

    void invalidJsonRaisesError()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse response;
        response.contentType = "application/json";
        response.body        = "not json {{{";
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);

        transport.start();
        nlohmann::json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "ping";
        transport.sendMessage(req);

        QVERIFY(waitForSignal(errorSpy, 1, 2000));
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

    void sseCallbackCanPostReentrantly()
    {
        MockHttpServer server;
        QVERIFY(server.listen());

        MockResponse streamResponse;
        streamResponse.contentType = "text/event-stream";
        streamResponse.body        = "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"
                                     "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n";
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

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
        transport.stop();

        QTest::qWait(100);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QVERIFY(transport.findChildren<QNetworkReply *>().isEmpty());
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
        connect(
            &transport,
            &QSocMcpTransport::messageReceived,
            &transport,
            [&transport](const nlohmann::json &) { transport.stop(); });

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});

        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QTest::qWait(100);
        QCOMPARE(messageSpy.size(), qsizetype(1));
    }

    void networkErrorIsEmittedOnce()
    {
        MockHttpServer server;
        QVERIFY(server.listen());
        MockResponse response;
        response.closeWithoutResponse = true;
        server.enqueue(response);

        QSocMcpHttpTransport transport(httpConfig(server.url()));
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);

        transport.start();
        transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

        QVERIFY(waitForSignal(errorSpy, 1, 3000));
        QTest::qWait(100);
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(transport.findChildren<QNetworkReply *>().isEmpty());
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
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport
            .sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, 7);

        QVERIFY(waitForSignal(errorSpy, 1, 3000));
        QTest::qWait(100);
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
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport
            .sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, 9);

        QVERIFY(waitForSignal(errorSpy, 1, 3000));
        QTest::qWait(300);
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmcphttp.moc"
