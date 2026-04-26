// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcphttp.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtCore>
#include <QtTest>

namespace {

struct MockResponse
{
    QByteArray contentType;
    QByteArray body;
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

        qsizetype contentLength = 0;
        for (const QByteArray &line : buf.left(headerEnd).split('\n')) {
            QByteArray trimmed = line;
            if (trimmed.endsWith('\r')) {
                trimmed.chop(1);
            }
            if (trimmed.toLower().startsWith("content-length:")) {
                contentLength = trimmed.mid(15).trimmed().toLongLong();
            }
        }

        if (buf.size() < headerEnd + 4 + contentLength) {
            return;
        }

        const QByteArray body = buf.mid(headerEnd + 4, contentLength);
        requestBodies_ << body;
        buffers_.remove(socket);

        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const MockResponse response = responses_.dequeue();
        if (response.contentType.contains("text/event-stream")
            && !response.sseChunkDelaysMs.isEmpty()) {
            sendChunkedSse(socket, response);
        } else {
            sendOneShot(socket, response);
        }
    }

private:
    void sendOneShot(QTcpSocket *socket, const MockResponse &response)
    {
        QByteArray http = "HTTP/1.1 200 OK\r\n";
        http += "Content-Type: " + response.contentType + "\r\n";
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
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcphttp.moc"
