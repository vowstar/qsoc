// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocmemoryextractor.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocsession.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Answers by request kind: memory extraction child, compaction summary, or
 * the main agent, which reads twice and then finishes. */
class ScriptedServer final : public QObject
{
public:
    ScriptedServer()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1/chat/completions").arg(server_.serverPort());
    }

    /* Reads issued for the next main-agent turn. */
    int         readsPerTurn = 2;
    QList<json> extractions;

private:
    void consume(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers_[socket];
        buffer.append(socket->readAll());
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        qsizetype length = 0;
        for (QByteArray line : buffer.left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
            }
        }
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        const json request = json::parse(buffer.mid(headerEnd + 4, length).toStdString());
        buffers_.remove(socket);
        reply(socket, request.value("stream", false), respond(request));
    }

    json respond(const json &request)
    {
        const std::string body = request.dump();
        if (body.find("memory-extraction sub-agent") != std::string::npos) {
            extractions.append(request);
            return {{"role", "assistant"}, {"content", "noted"}};
        }
        if (body.find("precise conversation summarizer") != std::string::npos) {
            return {{"role", "assistant"}, {"content", "SUMMARY_ONLY"}};
        }
        const json &history = request.at("messages");
        if (history.back().value("role", std::string()) == "user") {
            turnReads_ = 0;
        }
        if (turnReads_ >= readsPerTurn) {
            return {{"role", "assistant"}, {"content", "FINAL_ANSWER"}};
        }
        ++turnReads_;
        const std::string id = "call_" + std::to_string(++calls_);
        return {
            {"role", "assistant"},
            {"content", nullptr},
            {"tool_calls",
             json::array(
                 {{{"index", 0},
                   {"id", id},
                   {"type", "function"},
                   {"function", {{"name", "bulk_read"}, {"arguments", "{}"}}}}})}};
    }

    static void reply(QTcpSocket *socket, bool stream, const json &message)
    {
        const std::string finish = message.contains("tool_calls") ? "tool_calls" : "stop";
        QByteArray        body;
        if (stream) {
            json delta = message;
            delta.erase("role");
            const json chunk = {
                {"choices", json::array({{{"delta", delta}, {"finish_reason", finish}}})}};
            body = "data: " + QByteArray::fromStdString(chunk.dump()) + "\n\ndata: [DONE]\n\n";
        } else {
            const json response = {
                {"choices", json::array({{{"message", message}, {"finish_reason", finish}}})}};
            body = QByteArray::fromStdString(response.dump());
        }
        const QByteArray type = stream ? "text/event-stream" : "application/json";
        socket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QTcpServer                      server_;
    int                             calls_     = 0;
    int                             turnReads_ = 0;
};

class BulkReadTool final : public QSocTool
{
public:
    using QSocTool::QSocTool;
    QString getName() const override { return QStringLiteral("bulk_read"); }
    QString getDescription() const override { return QStringLiteral("Reads a large file"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    bool    isReadOnly() const override { return true; }
    QString execute(const json &) override
    {
        return QStringLiteral("READ_RESULT_%1 ").arg(++reads_) + QString(12000, QLatin1Char('x'));
    }

private:
    int reads_ = 0;
};

class Test : public QObject
{
    Q_OBJECT

private:
    struct Harness
    {
        QTemporaryDir                      directory;
        ScriptedServer                     server;
        QLLMService                        service;
        QSocToolRegistry                   registry;
        QSocProjectManager                 project;
        std::unique_ptr<QSocMemoryManager> memory;
        std::unique_ptr<QSocSession>       session;
        std::unique_ptr<QSocAgent>         agent;
        QSocMemoryExtractor::Cursor        cursor;
        QString                            sessionPath;
        QString                            sessionId;
    };

    static QSocAgentConfig config(int contextTokens, int everyTurns)
    {
        QSocAgentConfig cfg;
        cfg.autoLoadMemory              = false;
        cfg.memoryRecallEnabled         = false;
        cfg.systemPromptOverride        = QStringLiteral("Follow the task.");
        cfg.maxContextTokens            = contextTokens;
        cfg.keepRecentMessages          = 2;
        cfg.memoryExtractMinNewMessages = 1;
        cfg.memoryExtractEveryTurns     = everyTurns;
        return cfg;
    }

    /* Mirrors the REPL committer: keep the unextracted slice, save the
     * cursor, then the snapshot. */
    static void bindAgent(Harness &harness, const QSocAgentConfig &cfg)
    {
        harness.agent
            = std::make_unique<QSocAgent>(nullptr, &harness.service, &harness.registry, cfg);
        QVERIFY(harness.agent->bindToolResultStore(
            harness.directory.filePath(QStringLiteral("artifacts")), harness.sessionId));
        harness.session  = std::make_unique<QSocSession>(harness.sessionId, harness.sessionPath);
        QSocAgent *agent = harness.agent.get();
        agent->setCompactionCommitter([&harness, agent](const auto &candidate) {
            QSocMemoryExtractor::carryOver(
                harness.cursor,
                agent->getMessages(),
                static_cast<int>(candidate.candidateMessages.size()));
            return QSocMemoryExtractor::saveCursor(harness.session.get(), harness.cursor)
                   && harness.session->appendSnapshot(candidate.candidateMessages);
        });
    }

    static void setUp(Harness &harness, const QSocAgentConfig &cfg)
    {
        QVERIFY(harness.directory.isValid());
        QVERIFY(harness.server.listen());
        LLMModelConfig endpoint;
        endpoint.name            = QStringLiteral("mock");
        endpoint.model           = QStringLiteral("mock");
        endpoint.url             = harness.server.url();
        endpoint.timeout         = 5000;
        endpoint.contextTokens   = 64000;
        endpoint.maxOutputTokens = 1000;
        harness.service.setModel(endpoint);
        harness.registry.registerTool(new BulkReadTool(&harness.registry));
        harness.project.setProjectPath(harness.directory.path());
        harness.memory      = std::make_unique<QSocMemoryManager>(nullptr, &harness.project);
        harness.sessionId   = QSocSession::generateId();
        harness.sessionPath = harness.directory.filePath(QStringLiteral("session.jsonl"));
        bindAgent(harness, cfg);
    }

    static bool extract(Harness &harness, int turn)
    {
        QSocMemoryExtractor extractor(harness.agent.get(), harness.memory.get(), &harness.service);
        return extractor.extract(harness.cursor, turn);
    }

    static QString extractedText(const Harness &harness)
    {
        if (harness.server.extractions.size() != 1) {
            return {};
        }
        const json &messages = harness.server.extractions.front().at("messages");
        return QString::fromStdString(messages.back().value("content", std::string()));
    }

    QTemporaryDir userConfig_;

private slots:
    /* Keep the user's own memory topics out of the extraction manifest. */
    void initTestCase()
    {
        QVERIFY(userConfig_.isValid());
        qputenv("XDG_CONFIG_HOME", userConfig_.path().toLocal8Bit());
    }

    void testCompactedTurnReachesExtraction_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("mid-turn") << QStringLiteral("mid-turn");
        QTest::newRow("idle") << QStringLiteral("idle");
        QTest::newRow("manual") << QStringLiteral("manual");
        QTest::newRow("typing-skip") << QStringLiteral("typing-skip");
    }

    void testCompactedTurnReachesExtraction()
    {
        QFETCH(QString, mode);
        const bool midTurn = mode == QStringLiteral("mid-turn");
        Harness    harness;
        /* Off-cadence turn 1 stands in for any skipped extraction before an
         * idle or manual compaction. */
        const int everyTurns = mode == QStringLiteral("typing-skip") || midTurn ? 1 : 2;
        setUp(harness, config(midTurn ? 9000 : 64000, everyTurns));
        QSignalSpy compacted(harness.agent.get(), &QSocAgent::compacting);

        QCOMPARE(
            harness.agent->run(QStringLiteral("MARKER_REQUEST_ONE read the file twice")),
            QStringLiteral("FINAL_ANSWER"));
        if (midTurn) {
            QVERIFY(compacted.count() > 0);
        } else {
            if (mode != QStringLiteral("typing-skip")) {
                QVERIFY(!extract(harness, 1));
            }
            QCOMPARE(compacted.count(), 0);
            if (mode == QStringLiteral("manual")) {
                QVERIFY(harness.agent->compact() > 0);
            } else {
                harness.agent->setConfig(config(9000, everyTurns));
                QVERIFY(harness.agent->compactIfNeeded() > 0);
            }
            harness.server.readsPerTurn = 0;
            QCOMPARE(
                harness.agent->run(QStringLiteral("MARKER_REQUEST_TWO")),
                QStringLiteral("FINAL_ANSWER"));
        }
        QVERIFY(!harness.agent->getMessages().dump().contains("MARKER_REQUEST_ONE"));

        QVERIFY(extract(harness, 2));
        const QString text = extractedText(harness);
        QVERIFY(text.contains(QStringLiteral("MARKER_REQUEST_ONE")));
        QVERIFY(text.contains(QStringLiteral("READ_RESULT_1")));
        QVERIFY(text.contains(QStringLiteral("READ_RESULT_2")));
        QVERIFY(text.contains(QStringLiteral("FINAL_ANSWER")));
        QVERIFY(midTurn || text.contains(QStringLiteral("MARKER_REQUEST_TWO")));
        QVERIFY(harness.cursor.pending.empty());
        QCOMPARE(harness.cursor.index, static_cast<int>(harness.agent->getMessages().size()));
    }

    void testPendingSurvivesRestart()
    {
        Harness harness;
        setUp(harness, config(64000, 1));
        QCOMPARE(
            harness.agent->run(QStringLiteral("MARKER_REQUEST_ONE read the file twice")),
            QStringLiteral("FINAL_ANSWER"));
        QVERIFY(harness.session->appendSnapshot(harness.agent->getMessages()));
        QVERIFY(harness.agent->compact() > 0);

        /* Exit before extraction: a new process restores both from disk. */
        const json saved = QSocSession::loadMessages(harness.sessionPath);
        QVERIFY(saved == harness.agent->getMessages());
        harness.cursor
            = QSocMemoryExtractor::loadCursor(harness.sessionPath, static_cast<int>(saved.size()));
        QVERIFY(!harness.cursor.pending.empty());
        bindAgent(harness, config(64000, 1));
        harness.agent->setMessages(saved);

        QVERIFY(extract(harness, 1));
        const QString text = extractedText(harness);
        QVERIFY(text.contains(QStringLiteral("MARKER_REQUEST_ONE")));
        QVERIFY(text.contains(QStringLiteral("READ_RESULT_2")));
        QVERIFY(harness.cursor.pending.empty());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmemoryaftercompaction.moc"
