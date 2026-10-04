// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/services/qsocpredictioncontroller.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

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

/* Answers every chat completion with one line and keeps the `model`
 * field of each request body. */
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

    int     requestCount() const { return models_.size(); }
    QString wireModel(int index) const { return models_.at(index); }
    QString body(int index) const { return bodies_.at(index); }
    bool    hold = false;
    void    release()
    {
        const auto pending = pending_;
        pending_.clear();
        for (const auto &entry : pending) {
            if (entry.first) {
                respond(entry.first, entry.second);
            }
        }
    }

private:
    static void respond(QTcpSocket *socket, bool messages)
    {
        const json reply
            = messages
                  ? json{{"type", "message"}, {"role", "assistant"}, {"content", json::array({{{"type", "text"}, {"text", "run the tests"}}})}, {"stop_reason", "end_turn"}}
                  : json{
                        {"choices",
                         json::array(
                             {{{"message",
                                {{"role", "assistant"}, {"content", "run the tests"}}}}})}};
        const QByteArray body = QByteArray::fromStdString(reply.dump());
        socket->write(
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
            + QByteArray::number(body.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
            + body);
        socket->flush();
        socket->disconnectFromHost();
    }

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
        const json payload
            = json::parse(buffer.mid(bodyStart, contentLength).toStdString(), nullptr, false);
        models_.append(QString::fromStdString(payload.value("model", std::string())));
        bodies_.append(QString::fromStdString(payload.dump()));
        buffers_.remove(socket);

        if (hold) {
            pending_.append({socket, payload.contains("max_tokens")});
        } else {
            respond(socket, payload.contains("max_tokens"));
        }
    }

    QList<QPair<QPointer<QTcpSocket>, bool>> pending_;
    QHash<QTcpSocket *, QByteArray>          buffers_;
    QStringList                              models_;
    QStringList                              bodies_;
    QTcpServer                               server_;
};

json twoTurns()
{
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", "hello"}});
    messages.push_back({{"role", "assistant"}, {"content", "hi"}});
    messages.push_back({{"role", "user"}, {"content", "do it"}});
    messages.push_back(
        {{"role", "user"},
         {"content", "<system-reminder>\ninternal note\n</system-reminder>"},
         {"_qsoc_reminder", {{"plan", true}}}});
    messages.push_back({{"role", "assistant"}, {"content", "done"}});
    return messages;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* The predictor clones the main service at startup. A later /model
     * switch on the main service must reach the prediction request. */
    void requestPrediction_followsTheMainServiceModel()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        const QByteArray yaml = QByteArrayLiteral(
                                    "llm:\n"
                                    "  model: flash-a\n"
                                    "  models:\n"
                                    "    flash-a:\n"
                                    "      url: ")
                                + server.url().toUtf8()
                                + QByteArrayLiteral(
                                    "\n"
                                    "      timeout: 3000\n"
                                    "    flash-b:\n"
                                    "      model: served-model\n"
                                    "      url: ")
                                + server.url().toUtf8()
                                + QByteArrayLiteral("\n      timeout: 3000\n");
        ScopedConfig     scope(yaml);

        QSocConfig               config;
        QLLMService              main(nullptr, &config);
        QSocPredictionController predictor(nullptr, &main);
        QSignalSpy               ghosts(&predictor, &QSocPredictionController::ghostReady);

        QVERIFY(main.setCurrentModel(QStringLiteral("flash-b")));
        predictor.requestPrediction(twoTurns());
        QTRY_COMPARE(ghosts.count(), 1);

        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.wireModel(0), QStringLiteral("served-model"));
        QVERIFY(server.body(0).contains(QStringLiteral("do it")));
        QVERIFY(!server.body(0).contains(QStringLiteral("internal note")));
        QCOMPARE(main.getCurrentModelId(), QStringLiteral("flash-b"));
    }

    void effortAndEndpointSnapshot_data()
    {
        QTest::addColumn<bool>("messagesApi");
        QTest::addColumn<bool>("reasoning");
        QTest::newRow("chat-thinking") << false << true;
        QTest::newRow("messages-thinking") << true << true;
        QTest::newRow("chat-no-thinking") << false << false;
        QTest::newRow("messages-no-thinking") << true << false;
    }

    void effortAndEndpointSnapshot()
    {
        QFETCH(bool, messagesApi);
        QFETCH(bool, reasoning);
        CaptureServer first;
        CaptureServer second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        QLLMService    main;
        LLMModelConfig model;
        model.id        = QStringLiteral("selected");
        model.model     = QStringLiteral("served-first");
        model.url       = first.url();
        model.timeout   = 3000;
        model.api       = messagesApi ? LLMApi::AnthropicMessages : LLMApi::OpenAIChat;
        model.reasoning = reasoning;
        model.effort    = QStringLiteral("low");
        main.setModel(model);
        QSocPredictionController predictor(nullptr, &main);
        QSignalSpy               ghosts(&predictor, &QSocPredictionController::ghostReady);
        json                     messages = twoTurns();
        predictor.requestPrediction(messages, QStringLiteral("high"));
        QTRY_COMPARE(ghosts.count(), 1);
        const json high = json::parse(first.body(0).toStdString());
        if (messagesApi) {
            QCOMPARE(high.contains("thinking"), reasoning);
            if (reasoning) {
                QCOMPARE(high["output_config"]["effort"].get<std::string>(), std::string("high"));
            }
        } else {
            QCOMPARE(high.contains("reasoning_effort"), reasoning);
            if (reasoning) {
                QCOMPARE(high["reasoning_effort"].get<std::string>(), std::string("high"));
                QCOMPARE(high["reasoning"]["effort"].get<std::string>(), std::string("high"));
            }
        }
        QCOMPARE(high.contains("temperature"), !reasoning);

        model.model = QStringLiteral("served-second");
        model.url   = second.url();
        main.setModel(model);
        QVERIFY(!predictor.hasGhost());
        messages.push_back({{"role", "assistant"}, {"content", "next turn"}});
        predictor.requestPrediction(messages, QString());
        QTRY_COMPARE(ghosts.count(), 2);
        QCOMPARE(first.requestCount(), 1);
        QCOMPARE(second.requestCount(), 1);
        const json off = json::parse(second.body(0).toStdString());
        QCOMPARE(off["model"].get<std::string>(), std::string("served-second"));
        QVERIFY(!off.contains("reasoning_effort"));
        QVERIFY(!off.contains("reasoning"));
        QVERIFY(!off.contains("thinking"));
        QVERIFY(!off.contains("output_config"));
        QVERIFY(off.contains("temperature"));
    }

    void settingChangeDropsLateResponse()
    {
        CaptureServer server;
        QVERIFY(server.listen());
        server.hold = true;
        QLLMService    main;
        LLMModelConfig model;
        model.model   = QStringLiteral("before");
        model.url     = server.url();
        model.timeout = 3000;
        main.setModel(model);
        QSocPredictionController predictor(nullptr, &main);
        QSignalSpy               ghosts(&predictor, &QSocPredictionController::ghostReady);
        predictor.requestPrediction(twoTurns(), QStringLiteral("high"));
        QTRY_COMPARE(server.requestCount(), 1);
        model.model = QStringLiteral("after");
        main.setModel(model);
        server.release();
        server.hold   = false;
        json messages = twoTurns();
        messages.push_back({{"role", "assistant"}, {"content", "another turn"}});
        predictor.requestPrediction(messages);
        QTRY_COMPARE(server.requestCount(), 2);
        QTRY_COMPARE(ghosts.count(), 1);
        QCOMPARE(server.wireModel(1), QStringLiteral("after"));
        QTest::qWait(50);
        QCOMPARE(ghosts.count(), 1);
    }

    /* The filter is the core correctness surface: it decides whether a raw
     * model line is shown to the user. shouldFilter() returns true to REJECT. */
    void shouldFilter_acceptsPlausibleInputs()
    {
        QVERIFY(!QSocPredictionController::shouldFilter("run the tests"));
        QVERIFY(!QSocPredictionController::shouldFilter("commit the changes"));
        QVERIFY(!QSocPredictionController::shouldFilter("add a unit test for this"));
        QVERIFY(!QSocPredictionController::shouldFilter("push it"));
    }

    void shouldFilter_acceptsAllowlistedSingleWords()
    {
        QVERIFY(!QSocPredictionController::shouldFilter("commit"));
        QVERIFY(!QSocPredictionController::shouldFilter("yes"));
        QVERIFY(!QSocPredictionController::shouldFilter("continue"));
        /* Slash commands are valid even as a single token. */
        QVERIFY(!QSocPredictionController::shouldFilter("/help"));
    }

    void shouldFilter_rejectsEmptyAndOversized()
    {
        QVERIFY(QSocPredictionController::shouldFilter(""));
        QVERIFY(QSocPredictionController::shouldFilter(QString(120, QLatin1Char('x'))));
        QVERIFY(
            QSocPredictionController::shouldFilter(
                "run the tests and then commit and then push and also deploy to staging now"));
    }

    void shouldFilter_rejectsBareSingleWord()
    {
        /* A single non-allowlisted word is too vague. */
        QVERIFY(QSocPredictionController::shouldFilter("foo"));
        QVERIFY(QSocPredictionController::shouldFilter("refactor"));
    }

    void shouldFilter_rejectsQuestionsAndMultiSentence()
    {
        QVERIFY(QSocPredictionController::shouldFilter("what about the edge cases?"));
        QVERIFY(QSocPredictionController::shouldFilter("Fix it. Then run tests."));
    }

    void shouldFilter_rejectsEvaluative()
    {
        QVERIFY(QSocPredictionController::shouldFilter("looks good to me"));
        QVERIFY(QSocPredictionController::shouldFilter("that works great"));
        QVERIFY(QSocPredictionController::shouldFilter("thanks for the help"));
    }

    void shouldFilter_rejectsAssistantVoice()
    {
        QVERIFY(QSocPredictionController::shouldFilter("Let me run the tests"));
        QVERIFY(QSocPredictionController::shouldFilter("I'll commit the changes"));
        QVERIFY(QSocPredictionController::shouldFilter("Here's what I found"));
    }

    void shouldFilter_rejectsFormatting()
    {
        QVERIFY(QSocPredictionController::shouldFilter("run **the** tests"));
    }

    /* With no LLM service, every request is a guarded no-op: no ghost, no
     * signal, no crash. Verifies the fail-closed default. */
    void requestPrediction_withoutServiceIsNoOp()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QSignalSpy               spy(&predictor, &QSocPredictionController::ghostReady);

        json messages = json::array();
        messages.push_back({{"role", "user"}, {"content", "hello"}});
        messages.push_back({{"role", "assistant"}, {"content", "hi"}});
        messages.push_back({{"role", "user"}, {"content", "do it"}});
        messages.push_back({{"role", "assistant"}, {"content", "done"}});

        predictor.requestPrediction(messages);

        QCOMPARE(spy.count(), 0);
        QVERIFY(!predictor.hasGhost());
    }

    void setEnabled_togglesState()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QVERIFY(predictor.isEnabled());
        predictor.setEnabled(false);
        QVERIFY(!predictor.isEnabled());
        predictor.setEnabled(true);
        QVERIFY(predictor.isEnabled());
    }

    /* cancel() on an idle controller emits nothing (no spurious clears). */
    void cancel_withoutGhostIsSilent()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QSignalSpy               spy(&predictor, &QSocPredictionController::ghostCleared);
        predictor.cancel();
        QCOMPARE(spy.count(), 0);
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocpredictioncontroller.moc"
