// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <algorithm>
#include <QBuffer>
#include <QHostAddress>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

namespace {
class ImageTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("generated_image"); }
    QString getDescription() const override { return QStringLiteral("Returns a generated image"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        ++calls;
        const json attachment
            = {{"mime", "image/png"},
               {"data", encoded.toStdString()},
               {"source_url", "runtime:generated-image"},
               {"est_tokens", tokens}};
        return QStringLiteral("effect %1 recorded\n").arg(calls)
               + QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen())
               + QString::fromStdString(attachment.dump())
               + QString::fromLatin1(QSocImageAttach::attachmentMarkerClose());
    }
    QByteArray encoded;
    int        tokens = 100;
    int        calls  = 0;
};

class Endpoint final : public QObject
{
public:
    Endpoint()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                auto &buffer = buffers[socket];
                buffer += socket->readAll();
                const auto end = buffer.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                qint64 length = 0;
                for (const auto &line : buffer.left(end).split('\n')) {
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toLongLong();
                }
                if (buffer.size() < end + 4 + length)
                    return;
                requests.push_back(json::parse(buffer.mid(end + 4, length).toStdString()));
                buffers.remove(socket);
                json message = {{"role", "assistant"}, {"content", "done"}};
                if (requests.size() % 2 == 1) {
                    json calls = json::array();
                    for (int i = 0; i < count; ++i)
                        calls.push_back(
                            {{"index", i},
                             {"id", "call_" + std::to_string(i)},
                             {"type", "function"},
                             {"function", {{"name", "generated_image"}, {"arguments", "{}"}}}});
                    message = {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", calls}};
                }
                const bool streaming = requests.back().value("stream", false);
                const json response
                    = streaming
                          ? json{{"choices", json::array({{{"delta", message}, {"finish_reason", message.contains("tool_calls") ? "tool_calls" : "stop"}}})}}
                          : json{{"choices", json::array({{{"message", message}}})}};
                QByteArray body = QByteArray::fromStdString(response.dump());
                if (streaming)
                    body = QByteArrayLiteral("data: ") + body
                           + QByteArrayLiteral("\n\ndata: [DONE]\n\n");
                const QByteArray contentType = streaming ? QByteArrayLiteral("text/event-stream")
                                                         : QByteArrayLiteral("application/json");
                socket->write(
                    QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ") + contentType
                    + QByteArrayLiteral("\r\nConnection: close\r\nContent-Length: ")
                    + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                buffers.remove(socket);
                socket->deleteLater();
            });
        });
    }
    QTcpServer                      server;
    QHash<QTcpSocket *, QByteArray> buffers;
    QList<json>                     requests;
    int                             count = 1;
};

QByteArray generatedImage(bool large)
{
    QImage  image(large ? 1024 : 2, large ? 1024 : 2, QImage::Format_RGB888);
    quint32 state = 17;
    for (int row = 0; row < image.height(); ++row) {
        auto *bytes = image.scanLine(row);
        for (int i = 0; i < image.width() * 3; ++i) {
            state    = state * 1664525U + 1013904223U;
            bytes[i] = static_cast<uchar>(state >> 24);
        }
    }
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return {};
    return bytes.toBase64();
}

class Test final : public QObject
{
    Q_OBJECT
private slots:
    void aggregateAdmission_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("streaming");
        for (const char *name : {"ordinary", "context", "bytes", "unknown", "negative"}) {
            QTest::newRow(name) << QString::fromLatin1(name) << false;
            const QByteArray row = QByteArray(name) + QByteArrayLiteral("-stream");
            QTest::newRow(row.constData()) << QString::fromLatin1(name) << true;
        }
    }
    void aggregateAdmission()
    {
        QFETCH(QString, mode);
        QFETCH(bool, streaming);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const auto previous = qgetenv("QSOC_HOME");
        qputenv("QSOC_HOME", home.path().toUtf8());
        const auto restore = qScopeGuard([previous] {
            if (previous.isNull())
                qunsetenv("QSOC_HOME");
            else
                qputenv("QSOC_HOME", previous);
        });
        Endpoint   endpoint;
        QVERIFY(endpoint.server.listen(QHostAddress::LocalHost));
        ImageTool tool;
        tool.encoded = generatedImage(mode == QStringLiteral("bytes"));
        QVERIFY(!tool.encoded.isEmpty());
        endpoint.count = mode == QStringLiteral("context") ? 3
                         : mode == QStringLiteral("bytes") ? 4
                                                           : 1;
        tool.tokens    = mode == QStringLiteral("context")    ? 40000
                         : mode == QStringLiteral("unknown")  ? 0
                         : mode == QStringLiteral("negative") ? -1
                                                              : 100;
        QLLMService    service;
        LLMModelConfig model;
        model.name           = QStringLiteral("image-budget-test");
        model.model          = model.name;
        model.url            = QStringLiteral("http://127.0.0.1:%1/chat/completions")
                                   .arg(endpoint.server.serverPort());
        model.imageMaxTokens = 5000;
        service.setModel(model);
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QSocAgentConfig config;
        config.verbose              = false;
        config.autoLoadMemory       = false;
        config.memoryRecallEnabled  = false;
        config.maxIterations        = 2;
        config.maxRetries           = 0;
        config.maxContextTokens     = mode == QStringLiteral("context") ? 100000 : 512000;
        config.reservedOutputTokens = 0;
        QSocAgent  agent(nullptr, &service, &registry, config);
        const json prefix = json::array(
            {{{"role", "user"}, {"content", "earlier question"}},
             {{"role", "assistant"}, {"content", "earlier answer"}}});
        agent.setMessages(prefix);
        const auto run = [&agent, streaming](const QString &input) {
            if (!streaming)
                return agent.run(input) == QStringLiteral("done");
            QSignalSpy completed(&agent, &QSocAgent::runComplete);
            agent.runStream(input);
            return completed.count() == 1 || completed.wait(10000);
        };
        QVERIFY(run(QStringLiteral("generate images")));
        QCOMPARE(tool.calls, endpoint.count);
        QCOMPARE(endpoint.requests.size(), 2);
        const auto history = agent.getMessages();
        QCOMPARE(history[0], prefix[0]);
        QCOMPARE(history[1], prefix[1]);
        qint64 bytes   = 0;
        int    images  = 0;
        int    results = 0;
        bool   omitted = false;
        for (const auto &message : history) {
            if (message.value("role", std::string()) == "tool") {
                QCOMPARE(images, 0);
                QCOMPARE(message.at("tool_call_id"), json("call_" + std::to_string(results)));
                const auto text = QString::fromStdString(message.at("content").get<std::string>());
                QVERIFY(text.contains(QStringLiteral("effect %1 recorded").arg(results + 1)));
                if (text.contains(QStringLiteral("Image attachments omitted"))) {
                    omitted = true;
                    QVERIFY(text.contains(QStringLiteral("No image artifact was saved")));
                    QVERIFY(text.contains(QStringLiteral("runtime:generated-image")));
                    QVERIFY(!message.contains("_qsoc_artifact_refs"));
                }
                ++results;
            }
            if (!message.contains("content") || !message.at("content").is_array())
                continue;
            for (const auto &part : message.at("content")) {
                if (part.value("type", std::string()) == "image_url") {
                    ++images;
                    bytes += part.at("image_url").at("url").get_ref<const std::string &>().size();
                }
            }
        }
        QCOMPARE(results, endpoint.count);
        QVERIFY(bytes <= 16 * 1024 * 1024);
        if (mode == QStringLiteral("ordinary")) {
            QCOMPARE(images, 1);
            QVERIFY(!omitted);
        } else {
            QVERIFY(omitted);
            QVERIFY(images < endpoint.count);
            if (mode == QStringLiteral("unknown") || mode == QStringLiteral("negative"))
                QCOMPARE(images, 0);
            else
                QVERIFY(images > 0);
        }
        const auto &wire = endpoint.requests.back().at("messages");
        QVERIFY(std::any_of(wire.begin(), wire.end(), [](const auto &message) {
            return message.value("role", std::string()) == "tool";
        }));
        if (mode == QStringLiteral("bytes")) {
            QVERIFY(run(QStringLiteral("generate another batch")));
            QCOMPARE(tool.calls, endpoint.count * 2);
            const auto second      = agent.getMessages();
            int        totalImages = 0;
            for (const auto &message : second) {
                if (message.contains("content") && message.at("content").is_array())
                    for (const auto &part : message.at("content"))
                        totalImages += part.value("type", std::string()) == "image_url";
            }
            QCOMPARE(totalImages, images * 2);
        }
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentimagebudget.moc"
