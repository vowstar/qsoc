// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QSignalSpy>
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

    void simulateClosed()
    {
        if (state() == State::Stopped) {
            return;
        }
        setState(State::Stopped);
        emit closed();
    }

    void simulateError(const QString &message) { emit errorOccurred(message); }

    const QList<nlohmann::json> &sent() const { return sent_; }

private:
    QList<nlohmann::json> sent_;
};

McpServerConfig basicConfig()
{
    McpServerConfig cfg;
    cfg.name             = "fake";
    cfg.type             = "stdio";
    cfg.command          = "/bin/true";
    cfg.requestTimeoutMs = 5000;
    cfg.connectTimeoutMs = 5000;
    return cfg;
}

bool waitForState(const QSocMcpClient *client, QSocMcpClient::State target, int timeoutMs = 1000)
{
    QElapsedTimer timer;
    timer.start();
    while (client->state() != target) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
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

    void handshakeSendsInitialize()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);

        client.start();
        QVERIFY(waitForState(&client, QSocMcpClient::State::Initializing));
        QCOMPARE(transport->sent().size(), qsizetype(1));

        const auto &msg = transport->sent().first();
        QCOMPARE(QString::fromStdString(msg["jsonrpc"].get<std::string>()), QStringLiteral("2.0"));
        QCOMPARE(
            QString::fromStdString(msg["method"].get<std::string>()), QStringLiteral("initialize"));
        QVERIFY(msg.contains("id"));
        QCOMPARE(
            QString::fromStdString(msg["params"]["protocolVersion"].get<std::string>()),
            QStringLiteral("2025-03-26"));
        QVERIFY(msg["params"]["clientInfo"].contains("name"));
        QVERIFY(msg["params"]["clientInfo"].contains("version"));
    }

    void readyAfterInitializeResponse()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);

        client.start();
        QVERIFY(waitForState(&client, QSocMcpClient::State::Initializing));

        const int initId = transport->sent().first()["id"].get<int>();

        nlohmann::json response;
        response["jsonrpc"]                         = "2.0";
        response["id"]                              = initId;
        response["result"]["capabilities"]["tools"] = nlohmann::json::object();
        response["result"]["serverInfo"]["name"]    = "fake-server";
        response["result"]["serverInfo"]["version"] = "1.0";
        transport->simulateMessage(response);

        QVERIFY(waitForSignal(readySpy, 1));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        QVERIFY(client.serverCapabilities().contains("tools"));

        /* The initialized notification follows the response. */
        const auto last = transport->sent().last();
        QCOMPARE(
            QString::fromStdString(last["method"].get<std::string>()),
            QStringLiteral("notifications/initialized"));
        QVERIFY(!last.contains("id"));
    }

    void requestIdsAreMonotonic()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        const int idA = client.request("tools/list");
        const int idB = client.request("tools/call", {{"name", "x"}});
        const int idC = client.request("tools/list");

        QVERIFY(idA > 0);
        QVERIFY(idB > idA);
        QVERIFY(idC > idB);
        QCOMPARE(transport->sent().size(), qsizetype(5)); // initialize + initialized + 3 requests
    }

    void responseIsRoutedToRequester()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        const int requestId = client.request("tools/list");

        nlohmann::json response;
        response["jsonrpc"]         = "2.0";
        response["id"]              = requestId;
        response["result"]["tools"] = nlohmann::json::array();
        transport->simulateMessage(response);

        QVERIFY(waitForSignal(responseSpy, 1));
        QCOMPARE(responseSpy.first().at(0).toInt(), requestId);
    }

    void rpcErrorEmitsRequestFailed()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        const int requestId = client.request("tools/list");

        nlohmann::json response;
        response["jsonrpc"]          = "2.0";
        response["id"]               = requestId;
        response["error"]["code"]    = -32601;
        response["error"]["message"] = "Method not found";
        transport->simulateMessage(response);

        QVERIFY(waitForSignal(failureSpy, 1));
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32601);
    }

    void notificationDeliveredToListeners()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    notifSpy(&client, &QSocMcpClient::notificationReceived);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        nlohmann::json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "notifications/tools/list_changed";
        notif["params"]  = nlohmann::json::object();
        transport->simulateMessage(notif);

        QVERIFY(waitForSignal(notifSpy, 1));
        QCOMPARE(
            notifSpy.first().at(0).toString(), QStringLiteral("notifications/tools/list_changed"));
    }

    void timeoutFiresRequestFailed()
    {
        McpServerConfig cfg  = basicConfig();
        cfg.requestTimeoutMs = 80;

        auto         *transport = new FakeTransport;
        QSocMcpClient client(cfg, transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        const int requestId = client.request("tools/list");
        QVERIFY(requestId > 0);
        QVERIFY(waitForSignal(failureSpy, 1, 1000));
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
    }

    void transportClosedFailsPending()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        client.start();

        nlohmann::json initResp;
        initResp["jsonrpc"]                = "2.0";
        initResp["id"]                     = transport->sent().first()["id"].get<int>();
        initResp["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResp);
        QVERIFY(waitForState(&client, QSocMcpClient::State::Ready));

        const int requestId = client.request("tools/list");
        transport->simulateClosed();

        QVERIFY(waitForSignal(failureSpy, 1));
        QVERIFY(waitForSignal(closedSpy, 1));
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void requestRejectedWhenNotReady()
    {
        auto         *transport = new FakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        const int     requestId = client.request("tools/list");
        QCOMPARE(requestId, -1);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpclient.moc"
