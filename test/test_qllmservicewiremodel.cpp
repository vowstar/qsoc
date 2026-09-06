// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
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

        const json reply = {
            {"choices", json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})}};
        const QByteArray body    = QByteArray::fromStdString(reply.dump());
        QByteArray       headers = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
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
