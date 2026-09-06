// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * A sub-agent definition's `model:` names an llm.models key. The child
 * must run on that entry, the parent must stay on its own, and a key
 * that is not configured must fail the spawn instead of silently
 * running the child on the parent's model.
 */
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

/* Minimal streaming chat-completions endpoint: answers every request
 * with one final assistant message and keeps each request body. */
class MockLlm final : public QObject
{
public:
    MockLlm()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                buffers_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
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

    int     requestCount() const { return bodies_.size(); }
    QString wireModel(int index) const
    {
        const json payload = json::parse(bodies_.at(index).toStdString(), nullptr, false);
        return QString::fromStdString(payload.value("model", std::string()));
    }

private:
    void consume(QTcpSocket *socket)
    {
        auto it = buffers_.find(socket);
        if (it == buffers_.end()) {
            return;
        }
        it.value().append(socket->readAll());
        const qsizetype headerEnd = it.value().indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        qsizetype contentLength = 0;
        for (QByteArray line : it.value().left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                contentLength = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
            }
        }
        const qsizetype bodyStart = headerEnd + 4;
        if (it.value().size() < bodyStart + contentLength) {
            return;
        }
        bodies_.append(it.value().mid(bodyStart, contentLength));
        buffers_.erase(it);

        const json contentChunk = {
            {"choices", json::array({{{"delta", {{"content", "delegated work done"}}}}})}};
        const json finishChunk = {
            {"choices", json::array({{{"delta", json::object()}, {"finish_reason", "stop"}}})}};
        const QByteArray body    = QByteArrayLiteral("data: ")
                                   + QByteArray::fromStdString(contentChunk.dump())
                                   + QByteArrayLiteral("\n\ndata: ")
                                   + QByteArray::fromStdString(finishChunk.dump())
                                   + QByteArrayLiteral("\n\ndata: [DONE]\n\n");
        QByteArray       headers = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ");
        headers += QByteArray::number(body.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<QByteArray>               bodies_;
    QTcpServer                      server_;
};

/* Two entries on one server: the parent's default, and a child entry
 * whose wire name differs from its key so the request body tells the
 * two apart. */
QByteArray parentAndChildModels(const QString &url)
{
    return QByteArrayLiteral(
               "llm:\n"
               "  model: parent-model\n"
               "  models:\n"
               "    parent-model:\n"
               "      url: ")
           + url.toUtf8()
           + QByteArrayLiteral(
               "\n"
               "      timeout: 3000\n"
               "    child-model:\n"
               "      model: child-wire\n"
               "      url: ")
           + url.toUtf8() + QByteArrayLiteral("\n      timeout: 3000\n");
}

QSocAgentDefinition definitionOn(const QString &model)
{
    QSocAgentDefinition def;
    def.name        = QStringLiteral("probe");
    def.description = QStringLiteral("spawn probe");
    def.promptBody  = QStringLiteral("Answer briefly.");
    def.model       = model;
    return def;
}

QSocAgentConfig quietParent()
{
    QSocAgentConfig config;
    config.verbose             = false;
    config.autoLoadMemory      = false;
    config.memoryRecallEnabled = false;
    config.maxIterations       = 2;
    config.maxRetries          = 1;
    config.autoBackgroundMs    = 0;
    return config;
}

json spawnArgs()
{
    return json{
        {"subagent_type", "probe"},
        {"description", "probe the child model"},
        {"prompt", "say hello"},
        {"run_in_background", false}};
}

struct Harness
{
    explicit Harness(const QSocAgentDefinition &def)
        : service(nullptr, &serviceConfig)
        , parent(nullptr, &service, &registry, quietParent())
        , tool(nullptr, &service, &registry, quietParent(), &definitions, &tasks)
    {
        definitions.registerDefinition(def);
        tool.setParentAgent(&parent);
        registry.registerTool(&tool);
    }

    json spawn()
    {
        const QString raw = registry.executeTool(QStringLiteral("agent"), spawnArgs());
        return json::parse(raw.toStdString(), nullptr, false);
    }

    QSocConfig                  serviceConfig;
    QLLMService                 service;
    QSocAgentDefinitionRegistry definitions;
    QSocSubAgentTaskSource      tasks;
    QSocToolRegistry            registry;
    QSocAgent                   parent;
    QSocToolAgent               tool;
};

} // namespace

class TestQSocToolAgentModel : public QObject
{
    Q_OBJECT

private slots:
    void definitionModelRunsTheChildOnThatEntry()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig scope(parentAndChildModels(llm.url()));

        Harness h(definitionOn(QStringLiteral("child-model")));
        QCOMPARE(h.service.getCurrentModelId(), QStringLiteral("parent-model"));

        const json response = h.spawn();
        QCOMPARE(response.value("status", std::string()), std::string("ok"));
        QCOMPARE(llm.requestCount(), 1);
        QCOMPARE(llm.wireModel(0), QStringLiteral("child-wire"));
        QCOMPARE(h.service.getCurrentModelId(), QStringLiteral("parent-model"));
    }

    void emptyDefinitionModelInheritsTheParentSelection()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig scope(parentAndChildModels(llm.url()));

        Harness h(definitionOn(QString()));
        QVERIFY(h.service.setCurrentModel(QStringLiteral("child-model")));

        const json response = h.spawn();
        QCOMPARE(response.value("status", std::string()), std::string("ok"));
        QCOMPARE(llm.requestCount(), 1);
        QCOMPARE(llm.wireModel(0), QStringLiteral("child-wire"));
    }

    void unknownDefinitionModelFailsTheSpawn()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig scope(parentAndChildModels(llm.url()));

        Harness h(definitionOn(QStringLiteral("not-configured")));

        const json response = h.spawn();
        QCOMPARE(response.value("status", std::string()), std::string("error"));
        QVERIFY(response.value("error", std::string()).find("not-configured") != std::string::npos);
        QCOMPARE(llm.requestCount(), 0);
    }
};

QSOC_TEST_MAIN(TestQSocToolAgentModel)
#include "test_qsoctoolagentmodel.moc"
