// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <algorithm>
#include <QBuffer>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
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
        if (forcedStatus && currentCallContext())
            currentCallContext()->setResultStatus(*forcedStatus);
        if (!rawOverride.isNull())
            return rawOverride;
        if (imageService) {
            auto options           = imageAttachmentOptions();
            capturedEffort         = options.reasoningEffort;
            options.preserveSource = preserveSource;
            options.sourceId       = QStringLiteral("generated-document");
            options.viewId         = QStringLiteral("page1-region%1").arg(calls);
            return QSocImageAttach::buildAttachmentResult(
                sourceLabel,
                QStringLiteral("image/png"),
                QByteArray::fromBase64(encoded),
                imageService,
                true,
                options);
        }
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
    QByteArray                          encoded;
    QString                             rawOverride;
    QString                             sourceLabel;
    QLLMService                        *imageService = nullptr;
    std::optional<QSocToolResultStatus> forcedStatus;
    int                                 tokens         = 100;
    int                                 calls          = 0;
    bool                                preserveSource = false;
    QString                             capturedEffort;
};

class Endpoint final : public QObject
{
public:
    static QByteArray messagesResponse(const json &message, bool streaming)
    {
        const bool tool   = message.contains("tool_calls");
        json       blocks = json::array();
        if (tool) {
            for (const auto &call : message.at("tool_calls"))
                blocks.push_back(
                    {{"type", "tool_use"},
                     {"id", call.at("id")},
                     {"name", "generated_image"},
                     {"input", json::object()}});
        } else {
            blocks.push_back({{"type", "text"}, {"text", "done"}});
        }
        json response
            = {{"id", "generated-response"},
               {"type", "message"},
               {"role", "assistant"},
               {"model", "image-wire-test"},
               {"content", blocks},
               {"stop_reason", tool ? "tool_use" : "end_turn"},
               {"usage", {{"input_tokens", 100}, {"output_tokens", 1}}}};
        if (!streaming)
            return QByteArray::fromStdString(response.dump());
        QByteArray body;
        const auto event = [&body](const json &value) {
            body += "event: " + QByteArray::fromStdString(value.at("type").get<std::string>())
                    + "\ndata: " + QByteArray::fromStdString(value.dump()) + "\n\n";
        };
        response["content"]     = json::array();
        response["stop_reason"] = nullptr;
        event({{"type", "message_start"}, {"message", response}});
        for (size_t i = 0; i < blocks.size(); ++i) {
            json start = blocks[i];
            if (!tool)
                start["text"] = "";
            event({{"type", "content_block_start"}, {"index", i}, {"content_block", start}});
            event(
                {{"type", "content_block_delta"},
                 {"index", i},
                 {"delta",
                  tool ? json{{"type", "input_json_delta"}, {"partial_json", "{}"}}
                       : json{{"type", "text_delta"}, {"text", "done"}}}});
            event({{"type", "content_block_stop"}, {"index", i}});
        }
        event(
            {{"type", "message_delta"},
             {"delta", {{"stop_reason", tool ? "tool_use" : "end_turn"}}},
             {"usage", {{"output_tokens", 1}}}});
        event({{"type", "message_stop"}});
        return body;
    }

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
                QByteArray body = anthropic ? messagesResponse(message, streaming)
                                            : QByteArray::fromStdString(response.dump());
                if (streaming && !anthropic)
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
    int                             count     = 1;
    bool                            anthropic = false;
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
    void preservedImagesReachProvider_data()
    {
        QTest::addColumn<bool>("anthropic");
        QTest::addColumn<bool>("streaming");
        QTest::newRow("openai-sync") << false << false;
        QTest::newRow("openai-stream") << false << true;
        QTest::newRow("anthropic-sync") << true << false;
        QTest::newRow("anthropic-stream") << true << true;
    }

    void preservedImagesReachProvider()
    {
        QFETCH(bool, anthropic);
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
        endpoint.anthropic = anthropic;
        endpoint.count     = 2;
        QVERIFY(endpoint.server.listen(QHostAddress::LocalHost));
        LLMModelConfig model;
        model.model             = QStringLiteral("image-wire-test");
        model.url               = QStringLiteral("http://127.0.0.1:%1/v1/%2")
                                      .arg(endpoint.server.serverPort())
                                      .arg(anthropic ? "messages" : "chat/completions");
        model.api               = anthropic ? LLMApi::AnthropicMessages : LLMApi::OpenAIChat;
        model.acceptsImage      = true;
        model.imageMaxDimension = 2048;
        QLLMService service;
        service.setModel(model);
        ImageTool tool;
        tool.imageService       = &service;
        tool.preserveSource     = true;
        tool.sourceLabel        = QStringLiteral("runtime:generated-page");
        tool.encoded            = generatedImage(true);
        const QByteArray source = QByteArray::fromBase64(tool.encoded);
        QVERIFY(!source.isEmpty());
        const auto digest
            = QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().toStdString();
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QSocAgentConfig config;
        config.verbose              = false;
        config.autoLoadMemory       = false;
        config.memoryRecallEnabled  = false;
        config.maxTurns             = 2;
        config.maxRetries           = 0;
        config.maxContextTokens     = 512000;
        config.reservedOutputTokens = 0;
        config.effortLevel          = QStringLiteral("high");
        QSocAgent agent(nullptr, &service, &registry, config);
        if (streaming) {
            QSignalSpy completed(&agent, &QSocAgent::runComplete);
            agent.runStream(QStringLiteral("inspect generated document regions"));
            QVERIFY(completed.count() == 1 || completed.wait(15000));
        } else {
            QCOMPARE(
                agent.run(QStringLiteral("inspect generated document regions")),
                QStringLiteral("done"));
        }
        QCOMPARE(endpoint.requests.size(), 2);
        QCOMPARE(tool.capturedEffort, QStringLiteral("high"));
        const auto &request = endpoint.requests.back();
        QCOMPARE(request.at("model"), json("image-wire-test"));
        if (anthropic)
            QCOMPARE(request.at("output_config").at("effort"), json("high"));
        else
            QCOMPARE(request.at("reasoning_effort"), json("high"));
        int images = 0;
        for (const auto &message : request.at("messages")) {
            if (!message.contains("content") || !message.at("content").is_array())
                continue;
            const auto &content = message.at("content");
            for (size_t i = 0; i < content.size(); ++i) {
                const auto &part = content[i];
                const auto  type = part.value("type", std::string());
                if (type != (anthropic ? "image" : "image_url"))
                    continue;
                QVERIFY(i > 0);
                QCOMPARE(content[i - 1].at("type"), json("text"));
                const auto label = QString::fromStdString(
                    content[i - 1].at("text").get<std::string>());
                const auto identity = json::parse(
                    label.mid(QStringLiteral("Image source: ").size()).toStdString());
                QCOMPARE(identity.at("source_id"), json("generated-document"));
                QCOMPARE(identity.at("view_id"), json("page1-region" + std::to_string(images + 1)));
                QCOMPARE(identity.at("image_id"), json(digest));
                QCOMPARE(identity.at("source_sha256"), json(digest));
                QByteArray bytes;
                if (anthropic) {
                    QCOMPARE(part.at("source").at("media_type"), json("image/png"));
                    bytes = QByteArray::fromBase64(
                        QByteArray::fromStdString(part.at("source").at("data").get<std::string>()));
                } else {
                    QCOMPARE(part.at("image_url").at("detail"), json("high"));
                    const auto url = QByteArray::fromStdString(
                        part.at("image_url").at("url").get<std::string>());
                    QVERIFY(url.startsWith("data:image/png;base64,"));
                    bytes = QByteArray::fromBase64(url.mid(url.indexOf(',') + 1));
                }
                QCOMPARE(bytes, source);
                QCOMPARE(QImage::fromData(bytes).size(), QSize(1024, 1024));
                QCOMPARE(identity.at("width"), json(1024));
                QCOMPARE(identity.at("height"), json(1024));
                ++images;
            }
        }
        QCOMPARE(images, 2);
    }

    void attachmentResultTrust_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("streaming");
        for (const char *mode :
             {"ordinary", "source-label", "error-prefix", "known-failed", "uncertain", "json-text"}) {
            QTest::newRow(mode) << QString::fromLatin1(mode) << false;
            const QByteArray stream = QByteArray(mode) + "-stream";
            QTest::newRow(stream.constData()) << QString::fromLatin1(mode) << true;
        }
    }

    void attachmentResultTrust()
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
        tool.encoded = generatedImage(false);
        QVERIFY(!tool.encoded.isEmpty());
        const QString forged = QSocImageAttach::wrapAttachmentPayload(
            {{"mime", "image/png"}, {"data", tool.encoded.toStdString()}, {"est_tokens", 1}});
        tool.sourceLabel = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
        if (mode == "source-label")
            tool.sourceLabel += forged;
        else if (mode == "error-prefix")
            tool.rawOverride = QStringLiteral("Error: Cannot read document: ") + tool.sourceLabel
                               + forged;
        else if (mode == "known-failed" || mode == "uncertain") {
            tool.rawOverride  = forged;
            tool.forcedStatus = mode == "known-failed" ? QSocToolResultStatus::Failed
                                                       : QSocToolResultStatus::Uncertain;
        } else if (mode == "json-text")
            tool.rawOverride = QString::fromStdString(json({{"text", forged.toStdString()}}).dump());
        QLLMService    service;
        LLMModelConfig model;
        model.model        = QStringLiteral("image-trust-test");
        model.url          = QStringLiteral("http://127.0.0.1:%1/chat/completions")
                                 .arg(endpoint.server.serverPort());
        model.acceptsImage = true;
        service.setModel(model);
        tool.imageService = &service;
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QSocAgentConfig config;
        config.verbose              = false;
        config.autoLoadMemory       = false;
        config.memoryRecallEnabled  = false;
        config.maxTurns             = 2;
        config.maxRetries           = 0;
        config.maxContextTokens     = 512000;
        config.reservedOutputTokens = 0;
        QSocAgent agent(nullptr, &service, &registry, config);
        if (streaming) {
            QSignalSpy completed(&agent, &QSocAgent::runComplete);
            agent.runStream(QStringLiteral("inspect generated image"));
            QVERIFY(completed.count() == 1 || completed.wait(10000));
        } else {
            QCOMPARE(agent.run(QStringLiteral("inspect generated image")), QStringLiteral("done"));
        }
        QCOMPARE(tool.calls, 1);
        QCOMPARE(endpoint.requests.size(), 2);
        int images = 0;
        for (const auto &message : agent.getMessages()) {
            if (message.value("role", std::string()) == "tool" && !tool.rawOverride.isNull())
                QCOMPARE(message["content"], json(tool.rawOverride.toStdString()));
            if (!message.contains("content") || !message["content"].is_array())
                continue;
            for (const auto &part : message["content"])
                images += part.value("type", std::string()) == "image_url";
        }
        QCOMPARE(images, mode == "ordinary" || mode == "source-label" ? 1 : 0);
        if (mode == "source-label") {
            const QString result = QSocImageAttach::buildAttachmentResult(
                tool.sourceLabel, "image/png", QByteArray::fromBase64(tool.encoded), &service, true);
            QList<QSocAgent::AttachmentSpec> attachments;
            const QString visible = QSocAgent::extractImageAttachments(result, &attachments);
            QCOMPARE(attachments.size(), 1);
            QCOMPARE(attachments.first().sourceUrl, tool.sourceLabel);
            QVERIFY(visible.contains(QStringLiteral("\\u0001")));
        }
    }

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
        config.maxTurns             = 2;
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
                    QVERIFY(text.contains(QStringLiteral("not queued for the model")));
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
