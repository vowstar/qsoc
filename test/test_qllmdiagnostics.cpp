// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmdiagnostics.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <limits>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QtTest>

namespace {
using Kind    = QLLMDiagnostics::Kind;
using Outcome = QLLMDiagnostics::Outcome;

json payload(const std::string &content = "hello")
{
    return {
        {"model", "test"},
        {"messages",
         json::array(
             {{{"role", "system"}, {"content", "rules"}},
              {{"role", "user"}, {"content", content}}})}};
}

json usage()
{
    return {
        {"prompt_tokens", 100},
        {"completion_tokens", 3},
        {"prompt_tokens_details", {{"cached_tokens", 80}, {"cache_write_tokens", 20}}}};
}

class Endpoint : public QTcpServer
{
public:
    QList<json> requests;
    bool        hold = false;
    QByteArray  response;

    Endpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket  = nextPendingConnection();
                auto  bytes   = std::make_shared<QByteArray>();
                auto  handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, handled] {
                    *bytes += socket->readAll();
                    if (*handled)
                        return;
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    int length = 0;
                    for (const auto &line : bytes->left(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (bytes->size() < end + 4 + length)
                        return;
                    *handled = true;
                    requests.append(json::parse(bytes->mid(end + 4, length).toStdString()));
                    const bool stream = requests.back().value("stream", false);
                    if (stream) {
                        socket->write(
                            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: "
                            "close\r\n\r\n");
                        const json delta = {
                            {"choices",
                             json::array({{{"index", 0}, {"delta", {{"content", "done"}}}}})}};
                        socket->write("data: " + QByteArray::fromStdString(delta.dump()) + "\n\n");
                        if (!hold) {
                            const json counters = {{"choices", json::array()}, {"usage", usage()}};
                            const auto chunk = "data: " + QByteArray::fromStdString(counters.dump())
                                               + "\n\n";
                            socket->write(chunk);
                            socket->write(chunk);
                            socket->write("data: [DONE]\n\n");
                            socket->disconnectFromHost();
                        }
                    } else {
                        const json body
                            = {{"choices",
                                json::array(
                                    {{{"message", {{"role", "assistant"}, {"content", "done"}}}}})},
                               {"usage", usage()}};
                        const QByteArray data = response.isEmpty()
                                                    ? QByteArray::fromStdString(body.dump())
                                                    : response;
                        socket->write(
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                            + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n"
                            + data);
                        socket->disconnectFromHost();
                    }
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    LLMModelConfig model() const
    {
        LLMModelConfig value;
        value.id      = QStringLiteral("test");
        value.model   = value.id;
        value.url     = QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(serverPort());
        value.timeout = 2000;
        value.key     = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return value;
    }
};

class Test : public QObject
{
    Q_OBJECT
private slots:
    void missingZeroAndInvalid()
    {
        QLLMDiagnostics   observer;
        const QList<json> counters
            = {json(),
               {{"prompt_tokens", 100}, {"prompt_tokens_details", {{"cached_tokens", 0}}}},
               {{"prompt_tokens", 100}, {"prompt_tokens_details", {{"cached_tokens", -1}}}},
               {{"prompt_tokens", 100}, {"prompt_tokens_details", {{"cached_tokens", 101}}}},
               {{"prompt_tokens", 100}, {"prompt_tokens_details", {{"cached_tokens", "80"}}}},
               {{"prompt_tokens", 100},
                {"prompt_tokens_details",
                 {{"cached_tokens", (std::numeric_limits<quint64>::max)()}}}}};
        for (const auto &value : counters) {
            const auto request = observer.begin(Kind::Chat, {}, payload());
            request->posted();
            request->usage(value);
            request->finish(Outcome::Completed);
        }
        const json result = observer.snapshot()["chat"];
        QCOMPARE(result["service_calls"].get<int>(), 6);
        QCOMPARE(result["cache_read"]["reported"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["missing"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["invalid"].get<int>(), 4);
        QCOMPARE(result["cache_read"]["tokens"].get<int>(), 0);
        QCOMPARE(result["cache_ratio"]["fraction"].get<double>(), 0.0);
        QVERIFY(observer.snapshot()["text"]["cache_read"]["tokens"].is_null());
    }

    void rawAnthropicUsage()
    {
        QLLMDiagnostics observer;
        auto            request = observer.begin(Kind::Chat, {}, payload());
        request->usage(
            {{"input_tokens", 10},
             {"cache_read_input_tokens", 80},
             {"cache_creation_input_tokens", 20},
             {"output_tokens", 2}},
            true);
        request->finish(Outcome::Completed);
        auto second = observer.begin(Kind::Chat, {}, payload());
        second->usage({{"input_tokens", 10}, {"output_tokens", 2}}, true);
        second->finish(Outcome::Completed);
        const json result = observer.snapshot()["chat"];
        QCOMPARE(result["input"]["tokens"].get<int>(), 110);
        QCOMPARE(result["input"]["missing"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["tokens"].get<int>(), 80);
        QCOMPARE(result["cache_write"]["tokens"].get<int>(), 20);
    }

    void changesStayPrivate()
    {
        QLLMDiagnostics observer;
        const QString   secret = QUuid::createUuid().toString();
        json            body   = payload(secret.toStdString());
        const auto      record = [&](const json &value) {
            auto request = observer.begin(Kind::Stream, secret, value);
            request->posted();
            request->finish(Outcome::Completed);
        };
        record(body);
        record(body);
        body["messages"].push_back({{"role", "user"}, {"content", "next"}});
        record(body);
        body["messages"][1]["content"] = "changed";
        record(body);
        body["messages"][0]["content"] = "changed system";
        record(body);
        body["tools"] = json::array({{{"type", "function"}}});
        record(body);
        body["model"] = "other";
        record(body);
        const json changes = observer.snapshot()["stream"]["prefix_changes"];
        for (const char *key :
             {"first_observation",
              "unchanged",
              "history_appended",
              "history_changed",
              "system_changed",
              "tools_changed",
              "model_changed"})
            QCOMPARE(changes[key].get<int>(), 1);
        const QString serialized = QString::fromStdString(observer.snapshot().dump());
        QVERIFY(!serialized.contains(secret));
        QVERIFY(!serialized.contains(QStringLiteral("changed system")));
        QVERIFY(!serialized.contains(QStringLiteral("sha256")));
        QVERIFY(!serialized.contains(QStringLiteral("digest")));
    }

    void terminalAndResetIsolation()
    {
        QLLMDiagnostics observer;
        auto            request = observer.begin(Kind::Stream, {}, payload());
        request->posted();
        request->posted();
        request->usage(usage());
        request->finish(Outcome::Cancelled);
        request->usage({{"prompt_tokens", 999}});
        request->finish(Outcome::Completed);
        const json result = observer.snapshot()["stream"];
        QCOMPARE(result["network_attempts"].get<int>(), 1);
        QCOMPARE(result["finished_calls"].get<int>(), 1);
        QCOMPARE(result["outcomes"]["cancelled"].get<int>(), 1);
        QCOMPARE(result["partial_usage_reports"].get<int>(), 1);
        QCOMPARE(result["input"]["tokens"].get<int>(), 100);
        auto late = observer.begin(Kind::Chat, {}, payload());
        observer.setEnabled(false);
        QVERIFY(!observer.begin(Kind::Chat, {}, payload()));
        observer.setEnabled(true);
        late->posted();
        late->finish(Outcome::Completed);
        QCOMPARE(observer.snapshot()["chat"]["service_calls"].get<int>(), 0);
    }

    void excessiveHistoryIsUnobserved()
    {
        QLLMDiagnostics observer;
        json            body = payload();
        for (int i = 0; i < 4096; ++i)
            body["messages"].push_back({{"role", "user"}, {"content", "x"}});
        const auto request = observer.begin(Kind::Chat, {}, body);
        request->finish(Outcome::Completed);
        QCOMPARE(observer.snapshot()["chat"]["prefix_changes"]["unobserved"].get<int>(), 1);
    }

    void oversizedPartsAreUnobserved_data()
    {
        QTest::addColumn<QString>("part");
        for (const char *name : {"message", "tool", "schema", "system"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void oversizedPartsAreUnobserved()
    {
        QFETCH(QString, part);
        QLLMDiagnostics observer;
        json            body = payload();
        std::string     large(16 * 1024 * 1024, 'x');
        if (part == QStringLiteral("message"))
            body["messages"][1]["content"] = std::move(large);
        else if (part == QStringLiteral("tool"))
            body["tools"] = json::array({{{"description", std::move(large)}}});
        else if (part == QStringLiteral("schema"))
            body["response_format"] = {{"schema", {{"description", std::move(large)}}}};
        else
            body["system"] = std::move(large);
        const auto request = observer.begin(Kind::Chat, {}, body);
        request->finish(Outcome::Completed);
        QCOMPARE(observer.snapshot()["chat"]["prefix_changes"]["unobserved"].get<int>(), 1);
        QCOMPARE(body["messages"].size(), std::size_t(2));
        const auto small = observer.begin(Kind::Chat, {}, payload());
        small->finish(Outcome::Completed);
        QCOMPARE(observer.snapshot()["chat"]["prefix_changes"]["first_observation"].get<int>(), 1);
    }

    void deepAndWideStructuresAreUnobserved()
    {
        for (const bool deep : {true, false}) {
            QLLMDiagnostics observer;
            json            body  = payload();
            json            value = json::array();
            if (deep) {
                for (int i = 0; i < 80; ++i)
                    value = json::array({std::move(value)});
            } else {
                for (int i = 0; i < 65537; ++i)
                    value.push_back(i);
            }
            body["tools"]      = std::move(value);
            const auto request = observer.begin(Kind::Chat, {}, body);
            request->finish(Outcome::Completed);
            QCOMPARE(observer.snapshot()["chat"]["prefix_changes"]["unobserved"].get<int>(), 1);
        }
    }

    void serviceWireAndUsage()
    {
        Endpoint endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        QLLMService service;
        const auto  model = endpoint.model();
        service.setModel(model);
        const json messages = payload()["messages"];
        service.setDiagnosticsEnabled(false);
        QVERIFY(!service.sendChatCompletion(messages).contains("error"));
        service.setDiagnosticsEnabled(true);
        QVERIFY(!service.sendChatCompletion(messages).contains("error"));
        QCOMPARE(endpoint.requests.size(), 2);
        QVERIFY(endpoint.requests[0] == endpoint.requests[1]);
        QCOMPARE(service.requestDiagnostics()["chat"]["network_attempts"].get<int>(), 1);
        QCOMPARE(service.requestDiagnostics()["chat"]["cache_read"]["tokens"].get<int>(), 80);
        QVERIFY(service.sendRequest(QStringLiteral("hello")).success);
        bool asyncDone = false;
        service.sendRequestAsync(QStringLiteral("hello"), [&](const LLMResponse &response) {
            asyncDone = response.success;
        });
        QTRY_VERIFY(asyncDone);
        QCOMPARE(service.requestDiagnostics()["text"]["service_calls"].get<int>(), 2);
        QSignalSpy complete(&service, &QLLMService::streamComplete);
        service.sendChatCompletionStream(messages);
        QTRY_COMPARE(complete.size(), 1);
        const auto result = service.requestDiagnostics();
        QCOMPARE(result["stream"]["cache_read"]["tokens"].get<int>(), 80);
        QCOMPARE(result["stream"]["cache_read"]["reported"].get<int>(), 1);
        QVERIFY(!QString::fromStdString(result.dump()).contains(model.key));
        std::unique_ptr<QLLMService> clone(service.clone());
        QCOMPARE(clone->requestDiagnostics()["chat"]["service_calls"].get<int>(), 0);
    }

    void streamTimeoutIsNotSuccess()
    {
        Endpoint endpoint;
        endpoint.hold = true;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        QLLMService service;
        auto        model = endpoint.model();
        model.timeout     = 50;
        service.setModel(model);
        QSignalSpy error(&service, &QLLMService::streamError);
        service.sendChatCompletionStream(payload()["messages"]);
        QTRY_COMPARE_WITH_TIMEOUT(error.size(), 1, 3000);
        const auto result = service.requestDiagnostics()["stream"];
        QCOMPARE(result["outcomes"]["timed_out"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["missing"].get<int>(), 1);
        QVERIFY(result["cache_read"]["tokens"].is_null());
        QCOMPARE(endpoint.requests.size(), 1);
    }

    void abortedStreamCannotSettleNextCall()
    {
        Endpoint endpoint;
        endpoint.hold = true;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        QLLMService service;
        service.setModel(endpoint.model());
        QSignalSpy chunk(&service, &QLLMService::streamChunk);
        QSignalSpy complete(&service, &QLLMService::streamComplete);
        service.sendChatCompletionStream(payload()["messages"]);
        QTRY_COMPARE(chunk.size(), 1);
        service.abortStream();
        endpoint.hold = false;
        service.sendChatCompletionStream(payload()["messages"]);
        QTRY_COMPARE(complete.size(), 1);
        const auto result = service.requestDiagnostics()["stream"];
        QCOMPARE(result["service_calls"].get<int>(), 2);
        QCOMPARE(result["finished_calls"].get<int>(), 2);
        QCOMPARE(result["outcomes"]["cancelled"].get<int>(), 1);
        QCOMPARE(result["outcomes"]["completed"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["reported"].get<int>(), 1);
        QCOMPARE(result["cache_read"]["missing"].get<int>(), 1);
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmdiagnostics.moc"
