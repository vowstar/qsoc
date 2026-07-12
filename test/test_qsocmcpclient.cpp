// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"
#include "qsocmcp_fake_transport.h"

#include <nlohmann/json.hpp>

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

    void timeoutFiresRequestFailed()
    {
        McpServerConfig cfg  = basicConfig();
        cfg.requestTimeoutMs = 80;

        auto         *transport = new QsocMcpFakeTransport;
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
        QSignalSpy    closedSpy(&client, &QSocMcpClient::closed);
        transport->setSendHook([transport](const nlohmann::json &message) {
            if (message.value("method", std::string()) == "notifications/initialized") {
                transport->simulateError(QStringLiteral("notification failed"));
            }
        });
        client.start();

        nlohmann::json initResponse;
        initResponse["jsonrpc"]                = "2.0";
        initResponse["id"]                     = transport->firstSentId();
        initResponse["result"]["capabilities"] = nlohmann::json::object();
        transport->simulateMessage(initResponse);

        QCOMPARE(readySpy.size(), 0);
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
