// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"
#include "qsocmcp_fake_transport.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>

#include <QSignalSpy>
#include <QtCore>
#include <QtTest>

namespace {

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
    void handshakeSendsInitialize()
    {
        auto         *transport = new QsocMcpFakeTransport;
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
        auto         *transport = new QsocMcpFakeTransport;
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
        auto         *transport = new QsocMcpFakeTransport;
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
        auto         *transport = new QsocMcpFakeTransport;
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
        auto         *transport = new QsocMcpFakeTransport;
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
        auto         *transport = new QsocMcpFakeTransport;
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

    void batchRoutesEveryMessage()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    notificationSpy(&client, &QSocMcpClient::notificationReceived);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int            successId       = client.request(QStringLiteral("success"));
        const int            failureId       = client.request(QStringLiteral("failure"));
        const qsizetype      sentBeforeBatch = transport->sentCount();
        const nlohmann::json batch           = nlohmann::json::array({
            {{"jsonrpc", "2.0"}, {"id", successId}, {"result", {{"ok", true}}}},
            {{"jsonrpc", "2.0"}, {"method", 7}},
            {{"jsonrpc", "2.0"},
             {"method", "notifications/tools/list_changed"},
             {"params", nlohmann::json::object()}},
            {{"jsonrpc", "2.0"},
             {"id", failureId},
             {"error", {{"code", -32601}, {"message", "Method not found"}}}},
        });
        transport->simulateMessage(batch);

        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), successId);
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.at(0).at(0).toInt(), failureId);
        QCOMPARE(failureSpy.at(0).at(1).toInt(), -32601);
        QCOMPARE(transport->sentCount(), sentBeforeBatch + 1);
        const auto &batchResponse = transport->sent().last();
        QVERIFY(batchResponse.is_array());
        QCOMPARE(batchResponse.size(), std::size_t(1));
        QVERIFY(batchResponse.at(0)["id"].is_null());
        QCOMPARE(batchResponse.at(0)["error"]["code"], -32600);
        QCOMPARE(notificationSpy.size(), 1);
        QCOMPARE(
            notificationSpy.first().at(0).toString(),
            QStringLiteral("notifications/tools/list_changed"));
    }

    void batchClaimsResponsesBeforeCallbacks()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        const int thirdId  = client.request(QStringLiteral("third"));
        connect(
            &client,
            &QSocMcpClient::responseReceived,
            &client,
            [transport, firstId, secondId, thirdId](int id, const nlohmann::json &) {
                if (id != firstId) {
                    return;
                }
                transport->simulateMessage(
                    {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "reentrant"}}}});
                transport->simulateMessage(
                    {{"jsonrpc", "2.0"},
                     {"id", thirdId},
                     {"error", {{"code", -32000}, {"message", "reentrant"}}}});
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", firstId}, {"result", {{"source", "first"}}}},
                {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "batch"}}}},
                {{"jsonrpc", "2.0"},
                 {"id", thirdId},
                 {"error", {{"code", -32601}, {"message", "batch"}}}},
            }));

        QCOMPARE(responseSpy.size(), 2);
        QCOMPARE(responseSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(responseSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(
            responseSpy.at(1).at(1).value<nlohmann::json>().value("source", std::string()), "batch");
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), thirdId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32601);
        QCOMPARE(failureSpy.first().at(2).toString(), QStringLiteral("batch"));
    }

    void batchFailureCallbackCannotStealLaterResponse()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        connect(
            &client,
            &QSocMcpClient::requestFailed,
            &client,
            [transport, firstId, secondId](int id, int, const QString &) {
                if (id == firstId) {
                    transport->simulateMessage(
                        {{"jsonrpc", "2.0"},
                         {"id", secondId},
                         {"result", {{"source", "reentrant"}}}});
                }
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"},
                 {"id", firstId},
                 {"error", {{"code", -32601}, {"message", "first"}}}},
                {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "batch"}}}},
            }));

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), firstId);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), secondId);
        QCOMPARE(
            responseSpy.first().at(1).value<nlohmann::json>().value("source", std::string()),
            "batch");
    }

    void batchClaimsSurviveCallbackRestart()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        const int thirdId  = client.request(QStringLiteral("third"));
        connect(
            &client,
            &QSocMcpClient::responseReceived,
            &client,
            [&client, firstId](int id, const nlohmann::json &) {
                if (id == firstId) {
                    client.stop();
                    client.start();
                }
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", firstId}, {"result", {{"source", "first"}}}},
                {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "second"}}}},
                {{"jsonrpc", "2.0"},
                 {"id", thirdId},
                 {"error", {{"code", -32601}, {"message", "batch"}}}},
            }));

        QCOMPARE(responseSpy.size(), 2);
        QCOMPARE(responseSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(responseSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), thirdId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32601);
        QCOMPARE(failureSpy.first().at(2).toString(), QStringLiteral("batch"));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transport->lastSentId(), thirdId + 1);
    }

    void batchClaimBeatsReentrantMessageFailure()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        connect(
            &client,
            &QSocMcpClient::responseReceived,
            &client,
            [transport, firstId, secondId](int id, const nlohmann::json &) {
                if (id == firstId) {
                    transport
                        ->simulateMessageFailure(0, {secondId}, QStringLiteral("reentrant failure"));
                }
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", firstId}, {"result", {{"source", "first"}}}},
                {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "second"}}}},
            }));

        QCOMPARE(responseSpy.size(), 2);
        QCOMPARE(responseSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(responseSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(failureSpy.size(), 0);
    }

    void batchClaimStopsLaterTimeout()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"), nlohmann::json::object(), 200);
        const int secondId = client.request(QStringLiteral("second"), nlohmann::json::object(), 20);
        connect(
            &client,
            &QSocMcpClient::responseReceived,
            &client,
            [firstId](int id, const nlohmann::json &) {
                if (id != firstId) {
                    return;
                }
                QEventLoop wait;
                QTimer::singleShot(60, &wait, &QEventLoop::quit);
                wait.exec();
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", firstId}, {"result", {{"source", "first"}}}},
                {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"source", "second"}}}},
            }));

        QCOMPARE(responseSpy.size(), 2);
        QCOMPARE(responseSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(responseSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(failureSpy.size(), 0);
        for (const auto &sent : transport->sent()) {
            QVERIFY(sent.value("method", std::string()) != "notifications/cancelled");
        }
    }

    void batchStopsAtNewLifecycle()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    notificationSpy(&client, &QSocMcpClient::notificationReceived);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int requestId  = client.request(QStringLiteral("restart"));
        const int nextInitId = requestId + 1;
        bool      restarted  = false;
        connect(
            &client, &QSocMcpClient::responseReceived, &client, [&](int id, const nlohmann::json &) {
                if (id != requestId || restarted) {
                    return;
                }
                restarted = true;
                client.stop();
                client.start();
            });

        const nlohmann::json batch = nlohmann::json::array({
            {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", nlohmann::json::object()}},
            {{"jsonrpc", "2.0"}, {"method", "notifications/stale"}},
        });
        transport->simulateMessage(batch);

        QVERIFY(restarted);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(notificationSpy.size(), 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QVERIFY(client.serverCapabilities().empty());
        QCOMPARE(transport->lastSentId(), nextInitId);
    }

    void batchCannotCompleteRequestCreatedByCallback()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId           = client.request(QStringLiteral("first"));
        int       callbackRequestId = -1;
        connect(
            &client, &QSocMcpClient::responseReceived, &client, [&](int id, const nlohmann::json &) {
                if (id == firstId) {
                    callbackRequestId = client.request(QStringLiteral("callback"));
                }
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", firstId}, {"result", nlohmann::json::object()}},
                {{"jsonrpc", "2.0"}, {"id", firstId + 1}, {"result", {{"stale", true}}}},
            }));

        QCOMPARE(callbackRequestId, firstId + 1);
        QCOMPARE(responseSpy.size(), 1);

        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", callbackRequestId}, {"result", {{"ok", true}}}});
        QCOMPARE(responseSpy.size(), 2);
        QCOMPARE(responseSpy.last().at(0).toInt(), callbackRequestId);
    }

    void batchCallbackMayDeleteClient()
    {
        auto                      *transport = new QsocMcpFakeTransport;
        auto                      *client    = new QSocMcpClient(basicConfig(), transport);
        QPointer<QSocMcpClient>    clientGuard(client);
        QPointer<QSocMcpTransport> transportGuard(transport);
        client->start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);

        const int requestId = client->request(QStringLiteral("delete"));
        const int laterId   = client->request(QStringLiteral("later"));
        connect(
            client,
            &QSocMcpClient::responseReceived,
            client,
            [client, requestId](int id, const nlohmann::json &) {
                if (id == requestId) {
                    delete client;
                }
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", nlohmann::json::object()}},
                {{"jsonrpc", "2.0"}, {"id", laterId}, {"result", nlohmann::json::object()}},
                {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}},
            }));

        QVERIFY(clientGuard.isNull());
        QVERIFY(transportGuard.isNull());
    }

    void malformedMessagesDoNotEscape()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int  requestId           = client.request(QStringLiteral("malformed"));
        const auto sentBeforeMalformed = transport->sentCount();
        bool       threw               = false;
        try {
            transport->simulateMessage({{"jsonrpc", "2.0"}, {"method", 7}});
            transport->simulateMessage({{"jsonrpc", "2.0"}, {"id", requestId}, {"error", "broken"}});
            transport->simulateMessage(
                {{"jsonrpc", "1.0"}, {"id", requestId}, {"result", nlohmann::json::object()}});
            transport->simulateMessage(
                {{"jsonrpc", "2.0"},
                 {"id", requestId},
                 {"result", nlohmann::json::object()},
                 {"error", {{"code", -32603}, {"message", "broken"}}}});
            transport->simulateMessage(
                {{"jsonrpc", "2.0"},
                 {"id", std::numeric_limits<std::uint64_t>::max()},
                 {"result", nlohmann::json::object()}});
        } catch (const std::exception &) {
            threw = true;
        }

        QVERIFY(!threw);
        QCOMPARE(transport->sentCount(), sentBeforeMalformed + 1);
        const auto &invalidNotification = transport->sent().last();
        QVERIFY(invalidNotification["id"].is_null());
        QCOMPARE(invalidNotification["error"]["code"], -32600);
        QCOMPARE(failureSpy.size(), 3);
        for (const auto &failure : failureSpy) {
            QCOMPARE(failure.at(0).toInt(), -1);
            QCOMPARE(failure.at(1).toInt(), -32600);
        }

        QSignalSpy responseSpy(&client, &QSocMcpClient::responseReceived);
        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", {{"ok", true}}}});
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), requestId);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void emptyAndNestedBatchesAreInvalid()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);

        QCOMPARE(transport->sentCount(), qsizetype(2));
        transport->simulateMessage(nlohmann::json::array());
        QCOMPARE(transport->sentCount(), qsizetype(3));
        const auto &emptyBatchError = transport->sent().last();
        QVERIFY(emptyBatchError["id"].is_null());
        QCOMPARE(emptyBatchError["error"]["code"], -32600);

        transport->simulateMessage(
            nlohmann::json::array(
                {nlohmann::json::array({{{"jsonrpc", "2.0"}, {"method", "notifications/x"}}})}));
        QCOMPARE(transport->sentCount(), qsizetype(4));
        const auto &nestedBatchError = transport->sent().last();
        QVERIFY(nestedBatchError.is_array());
        QCOMPARE(nestedBatchError.size(), std::size_t(1));
        QVERIFY(nestedBatchError.at(0)["id"].is_null());
        QCOMPARE(nestedBatchError.at(0)["error"]["code"], -32600);
        QCOMPARE(failureSpy.size(), 0);
    }

    void serverPingIsAnsweredDuringInitialization()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transport->sentCount(), qsizetype(1));
        const int initializeId = transport->firstSentId();
        transport->simulateMessage({{"jsonrpc", "2.0"}, {"id", initializeId}, {"method", "ping"}});

        QCOMPARE(transport->sentCount(), qsizetype(2));
        const auto &response = transport->sent().last();
        QCOMPARE(response["jsonrpc"], "2.0");
        QCOMPARE(response["id"], initializeId);
        QVERIFY(response["result"].is_object());
        QVERIFY(response["result"].empty());
        QVERIFY(!response.contains("method"));
        QCOMPARE(failureSpy.size(), 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", initializeId},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void serverRequestIdsStayOpaque()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        QCOMPARE(transport->sentCount(), qsizetype(2));

        const auto requestId = std::numeric_limits<std::uint64_t>::max();
        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", requestId}, {"method", "roots/list"}});

        QCOMPARE(transport->sentCount(), qsizetype(3));
        const auto &response = transport->sent().last();
        QCOMPARE(response["jsonrpc"], "2.0");
        QCOMPARE(response["id"], requestId);
        QCOMPARE(response["error"]["code"], -32601);
        QCOMPARE(response["error"]["message"], "Method not found");
        QVERIFY(!response.contains("result"));

        const double fractionalId = 1.5;
        transport->simulateMessage({{"jsonrpc", "2.0"}, {"id", fractionalId}, {"method", "ping"}});

        QCOMPARE(transport->sentCount(), qsizetype(4));
        const auto &pingResponse = transport->sent().last();
        QCOMPARE(pingResponse["id"], fractionalId);
        QVERIFY(pingResponse["result"].is_object());
        QVERIFY(pingResponse["result"].empty());
        QCOMPARE(failureSpy.size(), 0);
    }

    void malformedServerRequestsGetInvalidRequest()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        transport->simulateMessage({{"jsonrpc", "2.0"}, {"id", true}, {"method", "ping"}});
        const auto invalidId = transport->sent().last();
        QVERIFY(invalidId["id"].is_null());
        QCOMPARE(invalidId["error"]["code"], -32600);

        transport->simulateMessage(
            {{"jsonrpc", "1.0"}, {"id", "wrong-version"}, {"method", "ping"}});
        const auto wrongVersion = transport->sent().last();
        QCOMPARE(wrongVersion["id"], "wrong-version");
        QCOMPARE(wrongVersion["error"]["code"], -32600);

        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", -9}, {"method", 7}, {"params", "bad"}});
        const auto invalidMethod = transport->sent().last();
        QCOMPARE(invalidMethod["id"], -9);
        QCOMPARE(invalidMethod["error"]["code"], -32600);

        transport->simulateMessage(nlohmann::json::object());
        const auto emptyObject = transport->sent().last();
        QVERIFY(emptyObject["id"].is_null());
        QCOMPARE(emptyObject["error"]["code"], -32600);

        const qsizetype sentBeforeUnrelated = transport->sentCount();
        transport->simulateMessage({{"foo", "boo"}});
        QCOMPARE(transport->sentCount(), sentBeforeUnrelated + 1);
        const auto unrelatedObject = transport->sent().last();
        QVERIFY(unrelatedObject["id"].is_null());
        QCOMPARE(unrelatedObject["error"]["code"], -32600);

        const qsizetype sentBeforeScalar = transport->sentCount();
        transport->simulateMessage(7);
        QCOMPARE(transport->sentCount(), sentBeforeScalar + 1);
        const auto scalarMessage = transport->sent().last();
        QVERIFY(scalarMessage["id"].is_null());
        QCOMPARE(scalarMessage["error"]["code"], -32600);

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", "valid-ping"}, {"method", "ping"}},
                {{"foo", "boo"}},
            }));
        const auto malformedBatch = transport->sent().last();
        QVERIFY(malformedBatch.is_array());
        QCOMPARE(malformedBatch.size(), std::size_t(2));
        QCOMPARE(malformedBatch.at(0)["id"], "valid-ping");
        QVERIFY(malformedBatch.at(1)["id"].is_null());
        QCOMPARE(malformedBatch.at(1)["error"]["code"], -32600);
        QCOMPARE(failureSpy.size(), 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void serverRequestBatchGetsOneReplyBatch()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    notificationSpy(&client, &QSocMcpClient::notificationReceived);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        const int localRequestId = client.request(QStringLiteral("echo"));
        QCOMPARE(transport->sentCount(), qsizetype(3));
        QSignalSpy responseSpy(&client, &QSocMcpClient::responseReceived);

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", localRequestId}, {"result", nlohmann::json::object()}},
                {{"jsonrpc", "2.0"}, {"id", localRequestId}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}},
                {{"jsonrpc", "2.0"}, {"id", 77}, {"method", "sampling/createMessage"}},
            }));

        QCOMPARE(transport->sentCount(), qsizetype(4));
        const auto &responses = transport->sent().last();
        QVERIFY(responses.is_array());
        QCOMPARE(responses.size(), std::size_t(2));
        QCOMPARE(responses.at(0)["id"], localRequestId);
        QVERIFY(responses.at(0)["result"].is_object());
        QCOMPARE(responses.at(1)["id"], 77);
        QCOMPARE(responses.at(1)["error"]["code"], -32601);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), localRequestId);
        QCOMPARE(notificationSpy.size(), 1);
        QCOMPARE(failureSpy.size(), 0);

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"method", "notifications/one"}},
                {{"jsonrpc", "2.0"}, {"method", "notifications/two"}},
            }));
        QCOMPARE(transport->sentCount(), qsizetype(4));
        QCOMPARE(notificationSpy.size(), 3);
        QCOMPARE(failureSpy.size(), 0);
    }

    void serverRequestReplyMayDeleteClient()
    {
        auto                      *transport = new QsocMcpFakeTransport;
        auto                      *client    = new QSocMcpClient(basicConfig(), transport);
        QPointer<QSocMcpClient>    clientGuard(client);
        QPointer<QSocMcpTransport> transportGuard(transport);
        client->start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);

        transport->setSendHook([client](const nlohmann::json &message) {
            if (message.value("id", std::string()) == "delete-client") {
                delete client;
            }
        });
        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", "delete-client"}, {"method", "ping"}});

        QVERIFY(clientGuard.isNull());
        QVERIFY(transportGuard.isNull());
    }

    void batchDropsServerRepliesAtNewLifecycle()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        client.start();

        nlohmann::json initialize;
        initialize["jsonrpc"]                = "2.0";
        initialize["id"]                     = transport->firstSentId();
        initialize["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initialize);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int requestId = client.request(QStringLiteral("restart"));
        bool      restarted = false;
        connect(
            &client, &QSocMcpClient::responseReceived, &client, [&](int id, const nlohmann::json &) {
                if (id != requestId || restarted) {
                    return;
                }
                restarted = true;
                client.stop();
                client.start();
            });

        transport->simulateMessage(
            nlohmann::json::array({
                {{"jsonrpc", "2.0"}, {"id", "stale-ping"}, {"method", "ping"}},
                {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", nlohmann::json::object()}},
            }));

        QVERIFY(restarted);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transport->sentCount(), qsizetype(4));
        QCOMPARE(transport->sent().last().value("method", std::string()), "initialize");
    }

    void timeoutFiresRequestFailed()
    {
        McpServerConfig cfg  = basicConfig();
        cfg.requestTimeoutMs = 80;

        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(cfg, transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
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
        QCOMPARE(failureSpy.first().at(1).toInt(), -32001);
        QCOMPARE(failureSpy.first().at(2).toString(), QStringLiteral("Request timed out: tools/list"));

        const nlohmann::json cancellation = transport->sent().last();
        QCOMPARE(cancellation.value("method", std::string()), "notifications/cancelled");
        QCOMPARE(cancellation["params"].value("requestId", -1), requestId);

        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", {{"late", true}}}});
        QCOMPARE(responseSpy.size(), 0);
    }

    void transportClosedFailsPending()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        bool          failureSawDisconnected = false;
        connect(
            &client,
            &QSocMcpClient::requestFailed,
            &client,
            [&client, &failureSawDisconnected](int, int, const QString &) {
                failureSawDisconnected = client.state() == QSocMcpClient::State::Disconnected;
            });
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
        QVERIFY(failureSawDisconnected);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void requestRejectedWhenNotReady()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        const int     requestId = client.request("tools/list");
        QCOMPARE(requestId, -1);
    }

    void handshakeErrorWaitsForOneRealClose()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    clientClosedSpy(&client, &QSocMcpClient::closed);
        QSignalSpy    transportClosedSpy(transport, &QSocMcpTransport::closed);

        client.start();
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        transport->simulateError(QStringLiteral("handshake failed"));

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(clientClosedSpy.size(), 0);
        QCOMPARE(transportClosedSpy.size(), 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);

        transport->simulateClosed();
        QCOMPARE(transportClosedSpy.size(), 1);
        QCOMPARE(clientClosedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);

        transport->simulateDuplicateClosed();
        QCOMPARE(transportClosedSpy.size(), 2);
        QCOMPARE(clientClosedSpy.size(), 1);
    }

    void synchronousHandshakeErrorStopsCleanly()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        transport->setSendHook([transport](const nlohmann::json &message) {
            if (message.value("method", std::string()) == "initialize") {
                transport->simulateError(QStringLiteral("synchronous failure"));
            }
        });

        client.start();

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(closedSpy.size(), 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void synchronousHandshakeErrorClosesOnce()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        transport->setSendHook([transport](const nlohmann::json &message) {
            if (message.value("method", std::string()) == "initialize") {
                transport->simulateError(QStringLiteral("synchronous failure"));
            }
        });

        client.start();

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
        transport->simulateDuplicateClosed();
        QCOMPARE(closedSpy.size(), 1);
    }

    void initializeErrorTerminatesLifecycle()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);

        client.start();
        nlohmann::json response;
        response["jsonrpc"]          = "2.0";
        response["id"]               = transport->firstSentId();
        response["error"]["code"]    = -32602;
        response["error"]["message"] = "Unsupported protocol";
        transport->simulateMessage(response);

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(closedSpy.size(), 0);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void initializeTimeoutTerminatesLifecycle()
    {
        McpServerConfig cfg  = basicConfig();
        cfg.requestTimeoutMs = 20;
        auto *transport      = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(cfg, transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);

        client.start();
        QVERIFY(waitForSignal(failureSpy, 1, 2000));

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(closedSpy.size(), 0);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void readyTransportErrorRemainsUsable()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int failedId = client.request(QStringLiteral("first"));
        transport->simulateError(QStringLiteral("request failed"));
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), failedId);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        QCOMPARE(closedSpy.size(), 0);

        const int secondId = client.request(QStringLiteral("second"));
        QVERIFY(secondId > failedId);
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"]      = secondId;
        response["result"]  = {"ok"};
        transport->simulateMessage(response);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), secondId);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void messageFailureOnlyFailsCorrelatedRequest()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        client.start();

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", transport->firstSentId()},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int failedId  = client.request(QStringLiteral("fails"));
        const int successId = client.request(QStringLiteral("succeeds"));
        transport->simulateMessageFailure(
            0, {failedId, failedId, successId + 1000}, QStringLiteral("request failed"));

        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), failedId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QCOMPARE(failureSpy.first().at(2).toString(), QStringLiteral("request failed"));

        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", failedId}, {"result", {{"late", true}}}});
        QCOMPARE(responseSpy.size(), qsizetype(0));

        transport->simulateMessage(
            {{"jsonrpc", "2.0"}, {"id", successId}, {"result", {{"ok", true}}}});
        QCOMPARE(responseSpy.size(), qsizetype(1));
        QCOMPARE(responseSpy.first().at(0).toInt(), successId);

        transport->simulateMessageFailure(0, {successId}, QStringLiteral("late failure"));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void messageFailureClaimsEveryIdBeforeCallbacks()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        client.start();

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", transport->firstSentId()},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        connect(
            &client,
            &QSocMcpClient::requestFailed,
            &client,
            [transport, firstId, secondId](int id, int, const QString &) {
                if (id == firstId) {
                    transport->simulateMessage(
                        {{"jsonrpc", "2.0"}, {"id", secondId}, {"result", {{"tooLate", true}}}});
                }
            });

        transport->simulateMessageFailure(0, {firstId, secondId}, QStringLiteral("request failed"));

        QCOMPARE(failureSpy.size(), 2);
        QCOMPARE(failureSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(failureSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(responseSpy.size(), 0);
    }

    void messageFailureCompletesEveryClaimAcrossRestart()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", transport->firstSentId()},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int firstId  = client.request(QStringLiteral("first"));
        const int secondId = client.request(QStringLiteral("second"));
        connect(
            &client,
            &QSocMcpClient::requestFailed,
            &client,
            [&client, firstId](int id, int, const QString &) {
                if (id == firstId) {
                    client.stop();
                    client.start();
                }
            });

        transport->simulateMessageFailure(0, {firstId, secondId}, QStringLiteral("request failed"));

        QCOMPARE(failureSpy.size(), 2);
        QCOMPARE(failureSpy.at(0).at(0).toInt(), firstId);
        QCOMPARE(failureSpy.at(1).at(0).toInt(), secondId);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transport->lastSentId(), secondId + 1);
    }

    void messageFailureCallbackMayDeleteClient()
    {
        auto                      *transport = new QsocMcpFakeTransport;
        auto                      *client    = new QSocMcpClient(basicConfig(), transport);
        QPointer<QSocMcpClient>    guard(client);
        QPointer<QSocMcpTransport> transportGuard(transport);
        client->start();

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", transport->firstSentId()},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);

        const int firstId  = client->request(QStringLiteral("first"));
        const int secondId = client->request(QStringLiteral("second"));
        int       failures = 0;
        connect(
            client,
            &QSocMcpClient::requestFailed,
            client,
            [client, &failures](int, int, const QString &) {
                failures++;
                delete client;
            });

        transport->simulateMessageFailure(0, {firstId, secondId}, QStringLiteral("request failed"));
        QVERIFY(guard.isNull());
        QVERIFY(transportGuard.isNull());
        QCOMPARE(failures, 1);
    }

    void messageFailureCallbackCannotTouchNewLifecycle()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        client.start();

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", transport->firstSentId()},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
        readySpy.clear();

        const int failedId       = client.request(QStringLiteral("restart"));
        const int nextInitialize = failedId + 1;
        connect(&client, &QSocMcpClient::requestFailed, &client, [&](int id, int, const QString &) {
            if (id == failedId) {
                client.stop();
                client.start();
            }
        });

        transport
            ->simulateMessageFailure(0, {failedId, nextInitialize}, QStringLiteral("request failed"));

        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QCOMPARE(transport->lastSentId(), nextInitialize);

        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", nextInitialize},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        QCOMPARE(readySpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void initializeMessageFailureTerminatesLifecycle()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        client.start();

        const int initializeId = transport->firstSentId();
        transport->simulateMessageFailure(0, {initializeId + 1}, QStringLiteral("unrelated failure"));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        transport->simulateMessageFailure(0, {initializeId}, QStringLiteral("initialize failed"));

        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), initializeId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void readyFatalTransportErrorClosesLifecycle()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);
        const int requestId = client.request(QStringLiteral("pending"));

        transport->stop();
        transport->simulateError(QStringLiteral("fatal failure"));

        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), requestId);
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(closedSpy.size(), 0);
        QCOMPARE(transport->stopCount(), 2);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void stopBeforeTransportStartFinishesLifecycle()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        connect(&client, &QSocMcpClient::stateChanged, &client, [&](QSocMcpClient::State state) {
            if (state == QSocMcpClient::State::Connecting) {
                client.stop();
            }
        });

        client.start();

        QCOMPARE(transport->startCount(), 0);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void closedHandlerMayRestartClient()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        bool          restarted = false;
        connect(&client, &QSocMcpClient::closed, &client, [&]() {
            if (restarted) {
                return;
            }
            restarted = true;
            client.start();
        });

        client.start();
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        client.stop();
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
        client.start();
        QCOMPARE(transport->startCount(), 1);

        transport->simulateClosed();

        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(transport->startCount(), 2);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        transport->simulateDuplicateClosed();
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        transport->setCloseOnStop(true);
        client.stop();
        QCOMPARE(closedSpy.size(), 2);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void readyReentryDoesNotEmitReady()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        connect(&client, &QSocMcpClient::stateChanged, &client, [&](QSocMcpClient::State state) {
            if (state == QSocMcpClient::State::Ready) {
                client.stop();
            }
        });
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        QCOMPARE(readySpy.size(), 0);
        QCOMPARE(transport->sentCount(), qsizetype(2));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void readyStateFollowsInitializedNotification()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        int           reentrantRequestId = -1;
        bool          notificationSeen   = false;
        connect(&client, &QSocMcpClient::stateChanged, &client, [&](QSocMcpClient::State state) {
            if (state != QSocMcpClient::State::Ready) {
                return;
            }
            notificationSeen   = transport->sentCount() == 2
                                 && transport->sent().last().value("method", std::string())
                                        == "notifications/initialized";
            reentrantRequestId = client.request(QStringLiteral("after-ready"));
        });
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        QVERIFY(notificationSeen);
        QVERIFY(reentrantRequestId > 0);
        QCOMPARE(transport->sentCount(), qsizetype(3));
        QCOMPARE(transport->sent().last().value("method", std::string()), std::string("after-ready"));
        QCOMPARE(readySpy.size(), 1);
    }

    void initializedSendErrorDoesNotPublishReady()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        transport->setSendHook([transport](const nlohmann::json &message) {
            if (message.value("method", std::string()) == "notifications/initialized") {
                transport->simulateError(QStringLiteral("notification failed"));
            }
        });
        client.start();

        const int      initializeId = transport->firstSentId();
        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = initializeId;
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        QCOMPARE(readySpy.size(), 0);
        QCOMPARE(failureSpy.size(), 1);
        QCOMPARE(failureSpy.first().at(0).toInt(), initializeId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QCOMPARE(closedSpy.size(), 1);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void initializedDeliveryTimeoutTerminatesLifecycle()
    {
        McpServerConfig cfg  = basicConfig();
        cfg.requestTimeoutMs = 80;

        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(cfg, transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        transport->setAutoCompleteTrackedMessages(false);
        client.start();

        const int      initializeId = transport->firstSentId();
        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = initializeId;
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QVERIFY(waitForSignal(closedSpy, 1, 1000));
        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), initializeId);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void initializedMessageFailureTerminatesLifecycle()
    {
        auto *transport = new QsocMcpFakeTransport;
        transport->setAutoCompleteTrackedMessages(false);
        transport->setCloseOnStop(false);
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        client.start();

        const int initializeId = transport->firstSentId();
        transport->simulateMessage(
            {{"jsonrpc", "2.0"},
             {"id", initializeId},
             {"result", {{"capabilities", nlohmann::json::object()}}}});
        const quint64 token = transport->lastTrackedToken();
        QVERIFY(token != 0);
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        transport->simulateMessageFailure(token + 1, {}, QStringLiteral("unrelated failure"));
        QCOMPARE(failureSpy.size(), qsizetype(0));
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);

        transport->simulateMessageFailure(token, {}, QStringLiteral("notification failed"));

        QCOMPARE(readySpy.size(), qsizetype(0));
        QCOMPARE(failureSpy.size(), qsizetype(1));
        QCOMPARE(failureSpy.first().at(0).toInt(), initializeId);
        QCOMPARE(failureSpy.first().at(1).toInt(), -32000);
        QCOMPARE(failureSpy.first().at(2).toString(), QStringLiteral("notification failed"));
        QCOMPARE(client.state(), QSocMcpClient::State::Failed);
        QCOMPARE(transport->stopCount(), 1);

        transport->simulateClosed();
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void trackedSendErrorDoesNotReportSuccess()
    {
        QsocMcpFakeTransport transport;
        QSignalSpy           errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy           sentSpy(&transport, &QSocMcpTransport::messageSent);
        transport.start();
        transport.setSendHook([&transport](const nlohmann::json &) {
            transport.simulateError(QStringLiteral("send failed"));
        });

        transport
            .sendTrackedMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, 7);

        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(sentSpy.size(), qsizetype(0));
    }

    void restartClearsServerCapabilities()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                         = "2.0";
        initResponse["id"]                              = transport->firstSentId();
        initResponse["result"]["capabilities"]["tools"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);
        QVERIFY(client.serverCapabilities().contains("tools"));

        client.stop();
        QVERIFY(client.serverCapabilities().empty());
        client.start();
        QCOMPARE(client.state(), QSocMcpClient::State::Initializing);
        QVERIFY(client.serverCapabilities().empty());
        client.stop();
    }

    void synchronousResponsesCompleteOnce()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    readySpy(&client, &QSocMcpClient::ready);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        transport->setSendHook([transport](const nlohmann::json &message) {
            if (!message.contains("id")) {
                return;
            }
            nlohmann::json response;
            response["jsonrpc"] = "2.0";
            response["id"]      = message["id"];
            if (message.value("method", std::string()) == "initialize") {
                response["result"]["capabilities"] = nlohmann::json::object();
            } else {
                response["result"] = {"ok"};
            }
            transport->simulateMessage(response);
        });

        client.start();
        QCOMPARE(readySpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int requestId = client.request(QStringLiteral("echo"));
        QVERIFY(requestId > 0);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(responseSpy.first().at(0).toInt(), requestId);

        client.stop();
        QCOMPARE(failureSpy.size(), 0);
    }

    void duplicateLateAndUnknownResponsesAreIgnored()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        QSignalSpy    responseSpy(&client, &QSocMcpClient::responseReceived);
        QSignalSpy    failureSpy(&client, &QSocMcpClient::requestFailed);
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        const int      completedId = client.request(QStringLiteral("complete"));
        nlohmann::json success;
        success["jsonrpc"] = "2.0";
        success["id"]      = completedId;
        success["result"]  = {"ok"};
        transport->simulateMessage(success);
        transport->simulateMessage(success);

        nlohmann::json unknown = success;
        unknown["id"]          = completedId + 1000;
        transport->simulateMessage(unknown);
        QCOMPARE(responseSpy.size(), 1);

        const int      failedId = client.request(QStringLiteral("fail"));
        nlohmann::json error;
        error["jsonrpc"]          = "2.0";
        error["id"]               = failedId;
        error["error"]["code"]    = -32603;
        error["error"]["message"] = "failed";
        transport->simulateMessage(error);
        transport->simulateMessage(error);
        QCOMPARE(failureSpy.size(), 1);

        const int lateId = client.request(QStringLiteral("late"));
        transport->simulateError(QStringLiteral("transport failure"));
        QCOMPARE(failureSpy.size(), 2);
        QCOMPARE(failureSpy.last().at(0).toInt(), lateId);

        nlohmann::json late = success;
        late["id"]          = lateId;
        transport->simulateMessage(late);
        QCOMPARE(responseSpy.size(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);
    }

    void reentrantStopFailsPendingOnce()
    {
        auto         *transport = new QsocMcpFakeTransport;
        QSocMcpClient client(basicConfig(), transport);
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);
        QCOMPARE(client.state(), QSocMcpClient::State::Ready);

        const int       firstId  = client.request(QStringLiteral("first"));
        const int       secondId = client.request(QStringLiteral("second"));
        QHash<int, int> failures;
        bool            reentered = false;
        connect(&client, &QSocMcpClient::requestFailed, &client, [&](int id, int, const QString &) {
            failures[id]++;
            if (!reentered) {
                reentered = true;
                client.stop();
            }
        });

        client.stop();

        QCOMPARE(failures.value(firstId), 1);
        QCOMPARE(failures.value(secondId), 1);
        QCOMPARE(failures.size(), 2);
        QCOMPARE(transport->stopCount(), 1);
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }

    void pendingFailureMayDeleteClient()
    {
        auto                   *transport = new QsocMcpFakeTransport;
        auto                   *client    = new QSocMcpClient(basicConfig(), transport);
        QPointer<QSocMcpClient> guard(client);
        client->start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);
        QCOMPARE(client->state(), QSocMcpClient::State::Ready);

        QVERIFY(client->request(QStringLiteral("first")) > 0);
        QVERIFY(client->request(QStringLiteral("second")) > 0);
        connect(client, &QSocMcpClient::requestFailed, client, [client](int, int, const QString &) {
            delete client;
        });

        client->stop();
        QVERIFY(guard.isNull());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpclient.moc"
