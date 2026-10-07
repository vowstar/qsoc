// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Write the yaml under a fresh XDG_CONFIG_HOME so QSocConfig reads it
 * instead of the developer's real ~/.config/qsoc/qsoc.yml. */
class ScopedConfig
{
public:
    explicit ScopedConfig(const QByteArray &yaml)
    {
        if (!tempDir.isValid()) {
            qFatal("ScopedConfig: failed to create temp dir");
        }
        const QString qsocDir = tempDir.filePath(QStringLiteral("qsoc"));
        if (!QDir().mkpath(qsocDir)) {
            qFatal("ScopedConfig: mkpath failed for %s", qPrintable(qsocDir));
        }
        QFile file(QDir(qsocDir).filePath(QStringLiteral("qsoc.yml")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("ScopedConfig: open failed: %s", qPrintable(file.errorString()));
        }
        if (file.write(yaml) != yaml.size()) {
            qFatal("ScopedConfig: short write");
        }
        file.close();
        previousXdg = qEnvironmentVariable("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", tempDir.path().toUtf8());
    }
    ~ScopedConfig()
    {
        if (previousXdg.isEmpty()) {
            qunsetenv("XDG_CONFIG_HOME");
        } else {
            qputenv("XDG_CONFIG_HOME", previousXdg.toUtf8());
        }
    }
    ScopedConfig(const ScopedConfig &)            = delete;
    ScopedConfig &operator=(const ScopedConfig &) = delete;

private:
    QTemporaryDir tempDir;
    QString       previousXdg;
};

/* An XDG_CONFIG_HOME with no qsoc.yml: the first QSocConfig writes the
 * template there. */
class ScopedEmptyXdg
{
public:
    ScopedEmptyXdg()
    {
        if (!tempDir.isValid()) {
            qFatal("ScopedEmptyXdg: failed to create temp dir");
        }
        previousXdg = qEnvironmentVariable("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", tempDir.path().toUtf8());
    }
    ~ScopedEmptyXdg()
    {
        if (previousXdg.isEmpty()) {
            qunsetenv("XDG_CONFIG_HOME");
        } else {
            qputenv("XDG_CONFIG_HOME", previousXdg.toUtf8());
        }
    }
    ScopedEmptyXdg(const ScopedEmptyXdg &)            = delete;
    ScopedEmptyXdg &operator=(const ScopedEmptyXdg &) = delete;

    QString configPath() const { return tempDir.filePath(QStringLiteral("qsoc/qsoc.yml")); }

private:
    QTemporaryDir tempDir;
    QString       previousXdg;
};

/* Answers every chat completion with a fixed reply and keeps each
 * request body, so the test can read the `model` field that went on
 * the wire. */
class CaptureServer final : public QObject
{
public:
    CaptureServer()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                buffers_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QString url() const
    {
        return QStringLiteral("http://%1:%2/chat/completions")
            .arg(server_.serverAddress().toString())
            .arg(server_.serverPort());
    }

    int         requestCount() const { return requests_.size(); }
    const json &request(int index) const { return requests_.at(index); }

private:
    void consume(QTcpSocket *socket)
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
        const qsizetype bodyStart = headerEnd + 4;
        if (buffer.size() < bodyStart + contentLength) {
            return;
        }
        requests_.append(
            json::parse(buffer.mid(bodyStart, contentLength).toStdString(), nullptr, false));
        buffers_.remove(socket);

        QByteArray body;
        QByteArray contentType;
        if (requests_.last().value("stream", false)) {
            const json finish = {
                {"choices",
                 json::array({{{"delta", {{"content", "ok"}}}, {"finish_reason", "stop"}}})}};
            body        = QByteArrayLiteral("data: ") + QByteArray::fromStdString(finish.dump())
                          + QByteArrayLiteral("\n\ndata: [DONE]\n\n");
            contentType = QByteArrayLiteral("text/event-stream");
        } else {
            const json reply = {
                {"choices",
                 json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})}};
            body        = QByteArray::fromStdString(reply.dump());
            contentType = QByteArrayLiteral("application/json");
        }
        QByteArray headers = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ") + contentType
                             + QByteArrayLiteral("\r\nContent-Length: ");
        headers += QByteArray::number(body.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<json>                     requests_;
    QTcpServer                      server_;
};

QByteArray twoEntriesOneServedModel(const QString &url)
{
    return QByteArrayLiteral(
               "llm:\n"
               "  model: flash-a\n"
               "  models:\n"
               "    flash-a:\n"
               "      url: ")
           + url.toUtf8()
           + QByteArrayLiteral(
               "\n"
               "      timeout: 3000\n"
               "    flash-b:\n"
               "      name: Flash (mirror)\n"
               "      model: served-model\n"
               "      url: ")
           + url.toUtf8() + QByteArrayLiteral("\n      timeout: 3000\n");
}

QString wireModel(const json &request)
{
    return QString::fromStdString(request.value("model", std::string()));
}

} // namespace

/*
 * The entry key under llm.models is the handle qsoc selects by; the
 * optional `model` field is the name that goes into the request body.
 * Without it the key is sent, as before.
 */
class TestQLLMServiceWireModel : public QObject
{
    Q_OBJECT

private slots:
    void requestPathFields_data()
    {
        QTest::addColumn<int>("variant");
        QTest::newRow("minimal") << 0;
        QTest::newRow("configured") << 1;
        QTest::newRow("reasoning") << 2;
        QTest::newRow("effort-disabled") << 3;
        QTest::newRow("empty-effort") << 4;
    }

    void requestPathFields()
    {
        QFETCH(int, variant);
        CaptureServer server;
        QVERIFY(server.listen());
        LLMModelConfig endpoint;
        endpoint.url                = server.url();
        endpoint.timeout            = 3000;
        endpoint.model              = variant ? QStringLiteral("wire-model") : QString();
        endpoint.maxOutputTokens    = variant ? 123 : 0;
        endpoint.reasoning          = variant == 2 || variant == 4;
        endpoint.chatTemplateKwargs = variant ? json{{"clear_thinking", false}} : json::object();
        const QString effort   = variant == 2 || variant == 3 ? QStringLiteral("high") : QString();
        const json    messages = json::array({{{"role", "user"}, {"content", "hi"}}});
        const json    tools
            = variant
                  ? json::array(
                        {{{"type", "function"},
                          {"function", {{"name", "lookup"}, {"parameters", {{"type", "object"}}}}}}})
                  : json::array();
        QLLMService llm;
        llm.setModel(endpoint);
        for (int path = 0; path < 4; ++path) {
            if (path == 0) {
                QVERIFY(
                    llm.sendRequest(QStringLiteral("hi"), QString(), 0.37, variant != 0).success);
            } else if (path == 1) {
                bool completed = false;
                bool success   = false;
                llm.sendRequestAsync(
                    QStringLiteral("hi"),
                    [&](const LLMResponse &response) {
                        success   = response.success;
                        completed = true;
                    },
                    QString(),
                    0.37,
                    variant != 0,
                    effort);
                QTRY_VERIFY(completed);
                QVERIFY(success);
            } else if (path == 2) {
                QVERIFY(
                    !llm.sendChatCompletion(messages, tools, 0.37, {}, effort).contains("error"));
            } else {
                QSignalSpy done(&llm, &QLLMService::streamComplete);
                QSignalSpy failed(&llm, &QLLMService::streamError);
                llm.sendChatCompletionStream(messages, tools, 0.37, effort);
                QTRY_COMPARE(done.count() + failed.count(), 1);
                QCOMPARE(failed.count(), 0);
            }
            QCOMPARE(server.requestCount(), path + 1);
            json expected = {{"messages", messages}, {"stream", path == 3}};
            if (variant == 2 && path != 0) {
                expected["reasoning_effort"] = "high";
                expected["reasoning"]        = {{"effort", "high"}};
            } else {
                expected["temperature"] = 0.37;
            }
            if (variant) {
                expected["model"]                = "wire-model";
                expected["chat_template_kwargs"] = {{"clear_thinking", false}};
                if (path < 2) {
                    expected["response_format"] = {{"type", "json_object"}};
                } else {
                    expected["tools"]      = tools;
                    expected["max_tokens"] = 123;
                }
            }
            if (path == 3) {
                expected["stream_options"] = {{"include_usage", true}};
            }
            QCOMPARE(server.request(path), expected);
        }
    }

    void keyIsSentWhenModelFieldIsAbsent()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        ScopedConfig scope(twoEntriesOneServedModel(server.url()));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        QCOMPARE(llm.getCurrentModelId(), QStringLiteral("flash-a"));

        const LLMModelConfig cfg = llm.getCurrentModelConfig();
        QCOMPARE(cfg.id, QStringLiteral("flash-a"));
        QCOMPARE(cfg.model, QStringLiteral("flash-a"));
        QCOMPARE(cfg.name, QStringLiteral("flash-a"));

        QVERIFY(llm.sendRequest(QStringLiteral("hi")).success);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(wireModel(server.request(0)), QStringLiteral("flash-a"));
    }

    void modelFieldIsSentAndKeyStaysTheHandle()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        ScopedConfig scope(twoEntriesOneServedModel(server.url()));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        QVERIFY(llm.setCurrentModel(QStringLiteral("flash-b")));
        QCOMPARE(llm.getCurrentModelId(), QStringLiteral("flash-b"));

        const LLMModelConfig cfg = llm.getCurrentModelConfig();
        QCOMPARE(cfg.id, QStringLiteral("flash-b"));
        QCOMPARE(cfg.model, QStringLiteral("served-model"));
        QCOMPARE(cfg.name, QStringLiteral("Flash (mirror)"));

        QVERIFY(llm.sendRequest(QStringLiteral("hi")).success);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(wireModel(server.request(0)), QStringLiteral("served-model"));
    }

    /* The flat llm.url / llm.key / llm.model form is gone: a config that
     * still uses it configures no endpoint and lists no models. */
    void flatKeysConfigureNothing()
    {
        ScopedConfig scope(QByteArrayLiteral(
            "llm:\n"
            "  url: http://127.0.0.1:9/v1/chat/completions\n"
            "  key: placeholder\n"
            "  model: flat-model\n"));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        QVERIFY(!llm.hasEndpoint());
        QVERIFY(llm.availableModels().isEmpty());
        QVERIFY(llm.getCurrentModelId().isEmpty());
    }

    /* The template written on first start must teach the form the loader
     * reads: uncommenting its llm block yields one registry entry. */
    void templateTeachesTheRegistryForm()
    {
        ScopedEmptyXdg xdg;
        QSocConfig     config;

        QFile file(xdg.configPath());
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(xdg.configPath()));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

        QStringList block;
        bool        inBlock = false;
        for (const QString &line : lines) {
            if (line == QStringLiteral("# llm:")) {
                inBlock = true;
            }
            if (!inBlock) {
                continue;
            }
            if (line.trimmed().isEmpty()) {
                break;
            }
            block << line.mid(2);
        }
        QVERIFY(!block.isEmpty());

        const YAML::Node root   = YAML::Load(block.join(QLatin1Char('\n')).toStdString());
        const YAML::Node models = root["llm"]["models"];
        QVERIFY(models.IsMap());
        QCOMPARE(static_cast<int>(models.size()), 1);
        const std::string selected = root["llm"]["model"].as<std::string>();
        QVERIFY(models[selected].IsMap());
        QVERIFY(models[selected]["url"].IsScalar());
        QVERIFY(!root["llm"]["url"]);
    }

    /* Effort is a dial on the current entry, never a model switch: the
     * streaming request carries reasoning_effort and the entry's wire name. */
    void effortRidesOnTheCurrentModel()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        ScopedConfig scope(twoEntriesOneServedModel(server.url()));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        QVERIFY(llm.setCurrentModel(QStringLiteral("flash-b")));

        QSignalSpy done(&llm, &QLLMService::streamComplete);
        QSignalSpy failed(&llm, &QLLMService::streamError);
        json       messages = json::array();
        messages.push_back({{"role", "user"}, {"content", "hi"}});
        llm.sendChatCompletionStream(messages, json::array(), 0.2, QStringLiteral("high"));
        QTRY_VERIFY(done.count() + failed.count() == 1);

        QCOMPARE(server.requestCount(), 1);
        const json &request = server.request(0);
        QCOMPARE(wireModel(request), QStringLiteral("served-model"));
        QCOMPARE(request.value("reasoning_effort", std::string()), std::string("high"));
    }

    void templateKwargsRideOnTheirModelOnly()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        ScopedConfig scope(
            QByteArrayLiteral(
                "llm:\n"
                "  model: thinker\n"
                "  models:\n"
                "    thinker:\n"
                "      url: ")
            + server.url().toUtf8()
            + QByteArrayLiteral(
                "\n"
                "      timeout: 3000\n"
                "      chat_template_kwargs:\n"
                "        clear_thinking: false\n"
                "        budget: 1024\n"
                "        mode: fast\n"
                "        label: \"true\"\n"
                "        tags: [a, 2]\n"
                "    plain:\n"
                "      url: ")
            + server.url().toUtf8()
            + QByteArrayLiteral(
                "\n"
                "      timeout: 3000\n"
                "    messages:\n"
                "      api: anthropic-messages\n"
                "      chat_template_kwargs: {clear_thinking: false}\n"
                "      url: ")
            + server.url().toUtf8() + QByteArrayLiteral("\n      timeout: 3000\n"));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        const json  expected
            = {{"clear_thinking", false},
               {"budget", 1024},
               {"mode", "fast"},
               {"label", "true"},
               {"tags", json::array({"a", 2})}};
        QCOMPARE(llm.getCurrentModelConfig().chatTemplateKwargs, expected);

        QVERIFY(llm.sendRequest(QStringLiteral("hi")).success);
        QSignalSpy done(&llm, &QLLMService::streamComplete);
        QSignalSpy failed(&llm, &QLLMService::streamError);
        json       messages = json::array();
        messages.push_back({{"role", "user"}, {"content", "hi"}});
        llm.sendChatCompletionStream(messages, json::array(), 0.2, QString());
        QTRY_VERIFY(done.count() + failed.count() == 1);
        QVERIFY(llm.setCurrentModel(QStringLiteral("plain")));
        QVERIFY(llm.sendRequest(QStringLiteral("hi")).success);

        QVERIFY(llm.setCurrentModel(QStringLiteral("messages")));
        llm.sendRequest(QStringLiteral("hi"));

        QCOMPARE(server.requestCount(), 4);
        QCOMPARE(server.request(0).at("chat_template_kwargs"), expected);
        QCOMPARE(server.request(1).at("chat_template_kwargs"), expected);
        QVERIFY(!server.request(2).contains("chat_template_kwargs"));
        QVERIFY(!server.request(3).contains("chat_template_kwargs"));
    }

    /* Sub-agents and memory children run on clones; the clone must
     * resolve the wire name the same way the parent did. */
    void cloneKeepsTheWireName()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        ScopedConfig scope(twoEntriesOneServedModel(server.url()));

        QSocConfig  config;
        QLLMService llm(nullptr, &config);
        QVERIFY(llm.setCurrentModel(QStringLiteral("flash-b")));

        QLLMService *child = llm.clone(nullptr);
        QCOMPARE(child->getCurrentModelId(), QStringLiteral("flash-b"));
        QVERIFY(child->sendRequest(QStringLiteral("hi")).success);
        delete child;

        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(wireModel(server.request(0)), QStringLiteral("served-model"));
    }
};

QSOC_TEST_MAIN(TestQLLMServiceWireModel)
#include "test_qllmservicewiremodel.moc"
