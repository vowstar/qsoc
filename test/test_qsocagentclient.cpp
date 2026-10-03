// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocagentdaemonclient.h"
#include "agent/protocol/qsocagentprotocol.h"
#include "common/qsoclocalendpoint.h"
#include "qsoc_test.h"

#include <QJsonDocument>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void fragmentedGreetingAndRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = QSocLocalEndpoint::resolve(directory.filePath("client.sock"));
        QString    error;
        QVERIFY2(QSocLocalEndpoint::prepareDirectory(path, &error), qPrintable(error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(path));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto      *socket = server.nextPendingConnection();
            const auto hello  = QSocAgentProtocol::frame(
                QJsonObject{{"daemon", "qsoc-agentd"}, {"protocol", 1}, {"version", "test"}});
            socket->write(hello.first(5));
            QTimer::singleShot(1, socket, [socket, hello] { socket->write(hello.mid(5)); });
            connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer = QByteArray{}]() mutable {
                buffer += socket->readAll();
                const int length = QSocAgentProtocol::payloadLength(buffer);
                if (length <= 0 || buffer.size() < QSocAgentProtocol::headerBytes + length)
                    return;
                const auto            request = QJsonDocument::fromJson(
                                                    buffer.mid(QSocAgentProtocol::headerBytes, length))
                                                    .object();
                QSocAgentRuntimeEvent event;
                event.kind         = QSocAgentRuntimeEvent::Kind::ContentChunk;
                event.text         = QStringLiteral("reply chunk");
                const auto payload = QJsonDocument::fromJson(
                                         QByteArray::fromStdString(event.toJson().dump()))
                                         .object();
                socket->write(QSocAgentProtocol::frame(QJsonObject{{"event", payload}}));
                socket->write(
                    QSocAgentProtocol::frame(
                        QJsonObject{
                            {"id", request.value("id")}, {"result", request.value("params")}}));
                buffer.clear();
            });
        });
        QSocAgentDaemonClient client(path);
        QSignalSpy            events(&client, &QSocAgentDaemonClient::eventReceived);
        QVERIFY2(client.connectToDaemon(), qPrintable(client.error()));
        QCOMPARE(client.daemonVersion(), QStringLiteral("test"));
        const auto reply = client.request("echo", {{"value", 42}});
        QCOMPARE(reply.value("result").toObject().value("value").toInt(), 42);
        QTRY_COMPARE(events.size(), 1);
        const auto event = qvariant_cast<QSocAgentRuntimeEvent>(events.first().first());
        QCOMPARE(event.kind, QSocAgentRuntimeEvent::Kind::ContentChunk);
        QCOMPARE(event.text, QStringLiteral("reply chunk"));
    }

    void rejectsIncompatibleGreeting()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = QSocLocalEndpoint::resolve(directory.filePath("client.sock"));
        QString    error;
        QVERIFY(QSocLocalEndpoint::prepareDirectory(path, &error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(path));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            socket->write(
                QSocAgentProtocol::frame(QJsonObject{{"daemon", "qsoc-agentd"}, {"protocol", 99}}));
        });
        QSocAgentDaemonClient client(path);
        QVERIFY(!client.connectToDaemon());
        QVERIFY(!client.isConnected());
        QVERIFY(client.error().contains("protocol"));
    }

    void disconnectedRequestFinishes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = QSocLocalEndpoint::resolve(directory.filePath("client.sock"));
        QString    error;
        QVERIFY(QSocLocalEndpoint::prepareDirectory(path, &error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(path));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            socket->write(
                QSocAgentProtocol::frame(QJsonObject{{"daemon", "qsoc-agentd"}, {"protocol", 1}}));
            connect(socket, &QLocalSocket::readyRead, socket, [socket] { socket->abort(); });
        });
        QSocAgentDaemonClient client(path);
        QVERIFY(client.connectToDaemon());
        QCOMPARE(
            client.request("status").value("error").toString(),
            QStringLiteral("daemon disconnected"));
    }

    void eventHandlerCanRequestDuringReplyDelivery()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = QSocLocalEndpoint::resolve(directory.filePath("client.sock"));
        QString    error;
        QVERIFY(QSocLocalEndpoint::prepareDirectory(path, &error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(path));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            socket->write(QSocIpc::frame(QJsonObject{{"daemon", "qsoc-agentd"}, {"protocol", 1}}));
            connect(socket, &QLocalSocket::readyRead, socket, [socket, pending = QByteArray{}]() mutable {
                pending += socket->readAll();
                QJsonObject request;
                while (QSocIpc::decode(pending, request) == QSocIpc::DecodeResult::Complete) {
                    if (request.value("method") == QStringLiteral("outer")) {
                        socket->write(
                            QSocIpc::frame(
                                QJsonObject{
                                    {"event",
                                     QJsonObject{{"kind", "content_chunk"}, {"text", "chunk"}}}}));
                    }
                    socket->write(
                        QSocIpc::frame(
                            QJsonObject{
                                {"id", request.value("id")}, {"result", request.value("method")}}));
                }
            });
        });
        QSocAgentDaemonClient client(path);
        QVERIFY(client.connectToDaemon());
        QJsonObject nested;
        connect(
            &client,
            &QSocAgentDaemonClient::eventReceived,
            &client,
            [&](const QSocAgentRuntimeEvent &) { nested = client.request("nested"); });
        const auto outer = client.request("outer");
        QCOMPARE(outer.value("result").toString(), QStringLiteral("outer"));
        QCOMPARE(nested.value("result").toString(), QStringLiteral("nested"));
    }

    void rejectsOversizedOutgoingFrame()
    {
        QSocAgentDaemonClient client(QStringLiteral("unused"));
        client.send({{"text", QString(QSocIpc::maxPayloadBytes, QLatin1Char('x'))}});
        QVERIFY(client.error().contains("limit"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentclient.moc"
