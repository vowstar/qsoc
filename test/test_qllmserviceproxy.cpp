// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class Peer : public QObject
{
public:
    explicit Peer(bool socks = false)
        : socks_(socks)
    {
        server_.setProxy(QNetworkProxy::NoProxy);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                auto *socket = server_.nextPendingConnection();
                states_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    states_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool    listen() { return server_.listen(QHostAddress::LocalHost); }
    quint16 port() const { return server_.serverPort(); }
    void    close() { server_.close(); }
    QString proxy() const
    {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(socks_ ? QStringLiteral("socks5") : QStringLiteral("http"))
            .arg(port());
    }
    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(port());
    }
    QStringList requests;
    QStringList tunnels;
    bool        reject = false;

private:
    struct State
    {
        QByteArray buffer;
        int        phase = 0;
    };

    bool handshake(QTcpSocket *socket, State &state)
    {
        auto &buffer = state.buffer;
        if (state.phase == 0) {
            if (buffer.size() < 2 || buffer.size() < 2 + quint8(buffer[1]))
                return false;
            buffer.remove(0, 2 + quint8(buffer[1]));
            socket->write(QByteArray::fromHex("0500"));
            state.phase = 1;
        }
        if (state.phase == 1) {
            if (buffer.size() < 5)
                return false;
            const int type = quint8(buffer[3]);
            const int size = type == 3 ? 7 + quint8(buffer[4]) : type == 1 ? 10 : 22;
            if (buffer.size() < size)
                return false;
            if (type == 3)
                tunnels.append(QString::fromUtf8(buffer.mid(5, quint8(buffer[4]))));
            buffer.remove(0, size);
            socket->write(QByteArray::fromHex("050000017f0000010000"));
            state.phase = 2;
        }
        return true;
    }

    void read(QTcpSocket *socket)
    {
        auto &state = states_[socket];
        state.buffer += socket->readAll();
        if (socks_ && !handshake(socket, state))
            return;
        auto      &buffer = state.buffer;
        const auto end    = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QByteArray firstLine = buffer.left(buffer.indexOf("\r\n"));
        if (firstLine.startsWith("CONNECT ")) {
            tunnels.append(QString::fromUtf8(firstLine));
            socket->write(
                "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        qsizetype length = 0;
        for (const auto &line : buffer.left(end).split('\n')) {
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < end + 4 + length)
            return;
        const auto request = json::parse(buffer.mid(end + 4, length).toStdString(), nullptr, false);
        requests.append(QString::fromUtf8(firstLine));
        const bool anthropic = buffer.left(end).toLower().contains("anthropic-version:");
        buffer.clear();
        QByteArray body;
        QByteArray contentType = "application/json";
        if (firstLine.contains("/count_tokens") || firstLine.contains("/tokenize")) {
            body = "{\"input_tokens\":7,\"count\":7}";
        } else if (anthropic && request.value("stream", false)) {
            contentType = "text/event-stream";
            const QList<json> events
                = {{{"type", "message_start"},
                    {"message",
                     {{"id", "probe"},
                      {"type", "message"},
                      {"role", "assistant"},
                      {"content", json::array()},
                      {"usage", {{"input_tokens", 1}, {"output_tokens", 0}}}}}},
                   {{"type", "content_block_start"},
                    {"index", 0},
                    {"content_block", {{"type", "text"}, {"text", ""}}}},
                   {{"type", "content_block_delta"},
                    {"index", 0},
                    {"delta", {{"type", "text_delta"}, {"text", "ok"}}}},
                   {{"type", "content_block_stop"}, {"index", 0}},
                   {{"type", "message_delta"},
                    {"delta", {{"stop_reason", "end_turn"}}},
                    {"usage", {{"output_tokens", 1}}}},
                   {{"type", "message_stop"}}};
            for (const auto &event : events)
                body += "event: " + QByteArray::fromStdString(event.at("type").get<std::string>())
                        + "\ndata: " + QByteArray::fromStdString(event.dump()) + "\n\n";
        } else if (request.value("stream", false)) {
            contentType      = "text/event-stream";
            const json chunk = {
                {"choices",
                 {{{"index", 0}, {"delta", {{"content", "ok"}}}, {"finish_reason", "stop"}}}}};
            body = "data: " + QByteArray::fromStdString(chunk.dump()) + "\n\ndata: [DONE]\n\n";
        } else if (anthropic) {
            const json response
                = {{"id", "probe"},
                   {"type", "message"},
                   {"role", "assistant"},
                   {"content", {{{"type", "text"}, {"text", "ok"}}}},
                   {"stop_reason", "end_turn"},
                   {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
            body = QByteArray::fromStdString(response.dump());
        } else {
            const json response = {
                {"choices",
                 {{{"message", {{"role", "assistant"}, {"content", "ok"}}},
                   {"finish_reason", "stop"}}}}};
            body = QByteArray::fromStdString(response.dump());
        }
        socket->write(
            (reject ? QByteArray("HTTP/1.1 502 Bad Gateway\r\n") : QByteArray("HTTP/1.1 200 OK\r\n"))
            + "Content-Type: " + contentType + "\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    bool                       socks_;
    QHash<QTcpSocket *, State> states_;
    QTcpServer                 server_;
};

const json messages = {{{"role", "user"}, {"content", "proxy probe"}}};

class Test : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        oldHome_ = qgetenv("QSOC_HOME");
        oldXdg_  = qgetenv("XDG_CONFIG_HOME");
        qputenv("QSOC_HOME", directory_.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", directory_.path().toUtf8());
    }

    void cleanupTestCase()
    {
        if (oldHome_.isNull())
            qunsetenv("QSOC_HOME");
        else
            qputenv("QSOC_HOME", oldHome_);
        if (oldXdg_.isNull())
            qunsetenv("XDG_CONFIG_HOME");
        else
            qputenv("XDG_CONFIG_HOME", oldXdg_);
        QVERIFY(directory_.remove());
    }

    void routing_data()
    {
        QTest::addColumn<QString>("spec");
        QTest::addColumn<bool>("socks");
        for (const auto &spec :
             {"inherit", "empty", "null", "system", "default", "model", "none", "off", "direct"}) {
            QTest::newRow(qPrintable(QStringLiteral("http-%1").arg(spec)))
                << QString::fromLatin1(spec) << false;
            QTest::newRow(qPrintable(QStringLiteral("socks-%1").arg(spec)))
                << QString::fromLatin1(spec) << true;
        }
    }

    void routing()
    {
        QFETCH(QString, spec);
        QFETCH(bool, socks);
        Peer origin, global(socks), model(socks);
        QVERIFY(origin.listen());
        QVERIFY(global.listen());
        QVERIFY(model.listen());
        QSocConfig config;
        config.setValue("proxy.type", socks ? "socks5" : "http");
        config.setValue("proxy.host", "127.0.0.1");
        config.setValue("proxy.port", QString::number(global.port()));
        const bool direct = spec == "none" || spec == "off" || spec == "direct";
        YAML::Node entry;
        entry["url"]     = (direct ? origin.url()
                                   : QStringLiteral("http://llm.invalid/v1/chat/completions"))
                               .toStdString();
        entry["timeout"] = 3000;
        if (spec == "null")
            entry["proxy"] = YAML::Node(YAML::NodeType::Null);
        else if (spec != "inherit")
            entry["proxy"] = (spec == "model"   ? model.proxy()
                              : spec == "empty" ? QString()
                                                : spec)
                                 .toStdString();
        const auto parsed = QLLMService::parseModelEntry("probe", entry);
        QVERIFY(parsed.has_value());
        auto        endpoint = *parsed;
        QLLMService service(nullptr, &config);
        service.setModel(endpoint);
        Peer &selected = direct ? origin : spec == "model" ? model : global;
        for (int path = 0; path < 8; ++path) {
            exercise(service, endpoint, path);
            QCOMPARE(selected.requests.size(), path + 1);
            QCOMPARE(
                origin.requests.size() + global.requests.size() + model.requests.size(), path + 1);
        }
        if (!direct && socks)
            QVERIFY(selected.tunnels.contains("llm.invalid"));
        if (!direct && !socks)
            QVERIFY(selected.requests.first().startsWith("POST http://llm.invalid/"));
    }

    void switchingAndCloning()
    {
        Peer first, second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        QFile file(directory_.filePath("qsoc.yml"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto yaml = QStringLiteral(
                              "proxy: {type: none}\nllm:\n  model: first\n  models:\n"
                              "    first: {url: 'http://llm.invalid/v1/chat/completions', proxy: "
                              "'%1', timeout: 3000}\n"
                              "    second: {url: 'http://llm.invalid/v1/chat/completions', proxy: "
                              "'%2', timeout: 3000}\n")
                              .arg(first.proxy(), second.proxy())
                              .toUtf8();
        QCOMPARE(file.write(yaml), yaml.size());
        file.close();
        QSocConfig  config;
        QLLMService service(nullptr, &config);
        auto        child     = std::unique_ptr<QLLMService>(service.clone());
        int         done      = 0;
        int         succeeded = 0;
        auto        callback  = [&](const LLMResponse &response) {
            ++done;
            succeeded += response.success;
        };
        service.sendRequestAsync("first", callback);
        QVERIFY(service.setCurrentModel("second"));
        service.sendRequestAsync("second", callback);
        child->sendRequestAsync("child", callback);
        QTRY_COMPARE(done, 3);
        QCOMPARE(succeeded, 3);
        QCOMPARE(first.requests.size(), 2);
        QCOMPARE(second.requests.size(), 1);
        QVERIFY(service.setCurrentModel("first"));
        QVERIFY(service.sendRequest("again").success);
        QCOMPARE(first.requests.size(), 3);
        config.setValue("proxy.type", "http");
        config.setValue("proxy.host", "127.0.0.1");
        config.setValue("proxy.port", QString::number(second.port()));
        auto inherited = service.getCurrentModelConfig();
        inherited.proxy.clear();
        service.setModel(inherited);
        QVERIFY(service.sendRequest("inherit").success);
        QCOMPARE(second.requests.size(), 2);
        const auto selectedModel = service.getModelConfig("first");
        QVERIFY(!service.sendChatCompletionTo(selectedModel, messages, json::array(), 0.2, {}, {})
                     .contains("error"));
        QCOMPARE(first.requests.size(), 4);
        auto counter      = selectedModel;
        counter.tokenizer = "http://counter.invalid/tokenize";
        QString error;
        QVERIFY(service.countTokens(counter, messages, json::array(), {}, {}, &error).has_value());
        QCOMPARE(first.requests.size(), 5);
        QCOMPARE(second.requests.size(), 2);
        QLLMService reloaded(nullptr, &config);
        reloaded.setModel(inherited);
        reloaded.sendRequestAsync("before reload", callback);
        config.setValue("proxy.port", QString::number(first.port()));
        reloaded.setConfig(&config);
        reloaded.setModel(inherited);
        reloaded.sendRequestAsync("after reload", callback);
        QTRY_COMPARE(done, 5);
        QCOMPARE(succeeded, 5);
        QCOMPARE(first.requests.size(), 6);
        QCOMPARE(second.requests.size(), 3);
        QVERIFY(file.remove());
    }

    void proxyFailureDoesNotConnectDirectly()
    {
        Peer origin, proxy, global;
        QVERIFY(origin.listen());
        QVERIFY(proxy.listen());
        QVERIFY(global.listen());
        proxy.reject = true;
        QSocConfig config;
        config.setValue("proxy.type", "http");
        config.setValue("proxy.host", "127.0.0.1");
        config.setValue("proxy.port", QString::number(global.port()));
        QLLMService    service(nullptr, &config);
        LLMModelConfig endpoint;
        endpoint.url     = origin.url();
        endpoint.proxy   = proxy.proxy();
        endpoint.timeout = 1000;
        service.setModel(endpoint);
        QVERIFY(!service.sendRequest("fail").success);
        QVERIFY(!proxy.requests.isEmpty());
        QCOMPARE(origin.requests.size(), 0);
        QCOMPARE(global.requests.size(), 0);
        proxy.close();
        QVERIFY(!service.sendRequest("refused").success);
        QCOMPARE(origin.requests.size(), 0);
        QCOMPARE(global.requests.size(), 0);
    }

    void invalidProxyBlocksEveryRequest_data()
    {
        QTest::addColumn<bool>("globalInvalid");
        QTest::newRow("model") << false;
        QTest::newRow("global") << true;
    }

    void invalidProxyBlocksEveryRequest()
    {
        QFETCH(bool, globalInvalid);
        Peer origin, global;
        QVERIFY(origin.listen());
        QVERIFY(global.listen());
        QSocConfig config;
        config.setValue("proxy.type", globalInvalid ? "invalid" : "http");
        config.setValue("proxy.host", "127.0.0.1");
        config.setValue("proxy.port", QString::number(global.port()));
        LLMModelConfig endpoint;
        endpoint.url     = origin.url();
        endpoint.proxy   = globalInvalid ? QString() : QStringLiteral("http://");
        endpoint.timeout = 1000;
        QLLMService service(nullptr, &config);
        service.setModel(endpoint);
        const auto text = service.sendRequest("probe");
        QVERIFY(!text.success);
        QVERIFY(text.errorMessage.contains("Invalid proxy configuration:"));
        bool done = false;
        service.sendRequestAsync("probe", [&](const LLMResponse &response) {
            QVERIFY(!response.success);
            QVERIFY(response.errorMessage.contains("Invalid proxy configuration:"));
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(service.sendChatCompletion(messages).contains("error"));
        for (const auto api : {LLMApi::OpenAIChat, LLMApi::AnthropicMessages}) {
            endpoint.api = api;
            service.setModel(endpoint);
            QVERIFY(service.sendChatCompletionTo(endpoint, messages, json::array(), 0.2, {}, {})
                        .contains("error"));
            QSignalSpy complete(&service, &QLLMService::streamComplete);
            QSignalSpy failed(&service, &QLLMService::streamError);
            service.sendChatCompletionStream(messages);
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(complete.size(), 0);
            QVERIFY(failed.first().first().toString().contains("Invalid proxy configuration:"));
            endpoint.tokenizer = endpoint.url + "/count_tokens";
            QString error;
            QVERIFY(!service.countTokens(endpoint, messages, json::array(), {}, {}, &error));
            QVERIFY(error.contains("Invalid proxy configuration:"));
        }
        QCOMPARE(origin.requests.size(), 0);
        QCOMPARE(global.requests.size(), 0);
        config.setValue("proxy.type", "http");
        endpoint.api   = LLMApi::OpenAIChat;
        endpoint.proxy = "none";
        service.setModel(endpoint);
        QVERIFY(service.sendRequest("probe").success);
        QCOMPARE(origin.requests.size(), 1);
        QCOMPARE(global.requests.size(), 0);
    }

    void httpsUsesConnect()
    {
        Peer global, model;
        QVERIFY(global.listen());
        QVERIFY(model.listen());
        QSocConfig config;
        config.setValue("proxy.type", "http");
        config.setValue("proxy.host", "127.0.0.1");
        config.setValue("proxy.port", QString::number(global.port()));
        QLLMService    service(nullptr, &config);
        LLMModelConfig endpoint;
        endpoint.url     = "https://llm.invalid/v1/chat/completions";
        endpoint.timeout = 1000;
        endpoint.proxy   = model.proxy();
        service.setModel(endpoint);
        QVERIFY(!service.sendRequest("tunnel").success);
        QVERIFY(!model.tunnels.isEmpty());
        QVERIFY(model.tunnels.first().startsWith("CONNECT llm.invalid:443 "));
        QVERIFY(global.tunnels.isEmpty());
    }

private:
    void exercise(QLLMService &service, LLMModelConfig endpoint, int path)
    {
        if (path == 0) {
            QVERIFY(service.sendRequest("probe").success);
        } else if (path == 1) {
            bool done    = false;
            bool success = false;
            service.sendRequestAsync("probe", [&](const LLMResponse &response) {
                done    = true;
                success = response.success;
            });
            QTRY_VERIFY(done);
            QVERIFY(success);
        } else if (path == 2) {
            QVERIFY(!service.sendChatCompletion(messages).contains("error"));
        } else if (path == 3 || path == 7) {
            if (path == 7) {
                endpoint.api = LLMApi::AnthropicMessages;
                service.setModel(endpoint);
            }
            QSignalSpy done(&service, &QLLMService::streamComplete);
            QSignalSpy failed(&service, &QLLMService::streamError);
            service.sendChatCompletionStream(messages);
            QTRY_COMPARE(done.size() + failed.size(), 1);
            QCOMPARE(failed.size(), 0);
        } else if (path == 4) {
            endpoint.api = LLMApi::AnthropicMessages;
            QVERIFY(!service.sendChatCompletionTo(endpoint, messages, json::array(), 0.2, {}, {})
                         .contains("error"));
        } else {
            if (path == 5)
                endpoint.tokenizer = "http://counter.invalid/tokenize";
            else
                endpoint.api = LLMApi::AnthropicMessages;
            // Direct count requests must use the loopback peer too.
            if (endpoint.proxy == "none" || endpoint.proxy == "off" || endpoint.proxy == "direct")
                endpoint.tokenizer = endpoint.url + "/count_tokens";
            QString    error;
            const auto count
                = service.countTokens(endpoint, messages, json::array(), {}, {}, &error);
            QVERIFY2(count.has_value(), qPrintable(error));
            QCOMPARE(*count, 7);
        }
    }

    QTemporaryDir directory_{QDir::tempPath() + "/test_qsoc_proxy_XXXXXX"};
    QByteArray    oldHome_;
    QByteArray    oldXdg_;
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmserviceproxy.moc"
