// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocrequestusage.h"
#include "agent/qsocsession.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"
#include "qsoc_test_pty.h"

#include <cmath>
#include <memory>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
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
        const QString directory = tempDir.filePath(QStringLiteral("qsoc"));
        QDir().mkpath(directory);
        QFile file(QDir(directory).filePath(QStringLiteral("qsoc.yml")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(yaml);
        }
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

/* The mock LLM with one MOCK_TOKENIZE behavior. */
class Mock
{
public:
    explicit Mock(const QHash<QString, QString> &settings)
    {
        port                            = QSocTestPty::pickFreePort();
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
        environment.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("DONE"));
        environment.insert(QStringLiteral("MOCK_REQUEST_LOG"), log.filePath(QStringLiteral("log")));
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            environment.insert(it.key(), it.value());
        }
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process.start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), {QString::number(port)});
    }

    bool ready() { return port > 0 && QSocTestPty::waitForMockReady(process, port, 45000); }

    QString url(const QString &host, const QString &path) const
    {
        return QStringLiteral("http://%1:%2%3").arg(host).arg(port).arg(path);
    }

    int hits(const char *counter) const
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, quint16(port));
        if (!socket.waitForConnected(3000)) {
            return -1;
        }
        socket.write("GET /hits HTTP/1.0\r\n\r\n");
        QByteArray reply;
        while (socket.waitForReadyRead(3000)) {
            reply += socket.readAll();
        }
        const auto body = reply.mid(reply.indexOf("\r\n\r\n") + 4);
        return int(json::parse(body.toStdString(), nullptr, false).value(counter, -1));
    }

    /* Logged count requests: /tokenize bodies, or Messages bodies without max_tokens. */
    QList<json> countBodies() const
    {
        QList<json> bodies;
        QFile       file(log.filePath(QStringLiteral("log")));
        if (file.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : file.readAll().split('\n')) {
                const json body = json::parse(line.toStdString(), nullptr, false);
                if (body.is_object() && body.contains("messages")
                    && (body.contains("add_generation_prompt") || !body.contains("max_tokens"))) {
                    bodies.append(body);
                }
            }
        }
        return bodies;
    }

    int                         port = 0;
    QTemporaryDir               log;
    QSocTestPty::BoundedProcess process;
};

class Probe final : public QSocTool
{
public:
    using QSocTool::QSocTool;
    QString getName() const override { return QStringLiteral("probe"); }
    QString getDescription() const override { return QStringLiteral("Probe a register."); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return QStringLiteral("ok"); }
};

/* About one o200k token per four characters. */
std::string prose(size_t characters, char lead)
{
    std::string text(1, lead);
    while (text.size() + 4 <= characters) {
        text += " the";
    }
    return text;
}

/* An agent on the mock whose history sits inside the prune gate's margin. */
class Session
{
public:
    explicit Session(const QByteArray &yaml, int characters = 2600)
        : scope(yaml)
    {
        llm = std::make_unique<QLLMService>(nullptr, &config);
        registry.registerTool(new Probe(&registry));
        QSocAgentConfig settings;
        settings.keepRecentMessages   = 2;
        settings.autoLoadMemory       = false;
        settings.memoryRecallEnabled  = false;
        settings.memoryExtractEnabled = false;
        settings.memoryDreamEnabled   = false;
        settings.systemPromptOverride = QStringLiteral("Follow the task.");
        settings.maxContextTokens     = 20000;
        agent = std::make_unique<QSocAgent>(nullptr, llm.get(), &registry, settings);
        agent->bindToolResultStore(
            artifacts.filePath(QStringLiteral("artifacts")), QSocSession::generateId());
        for (int index = 0; index < 10; ++index) {
            history.push_back(
                {{"role", index % 2 ? "assistant" : "user"},
                 {"content", prose(size_t(characters), static_cast<char>('a' + index))}});
        }
        agent->setMessages(history);
    }

    /* A Chat Completions entry whose tokenizer is the mock's /tokenize on host. */
    static QByteArray chat(const Mock &mock, const QString &host)
    {
        return configuration(
            mock.url(QStringLiteral("127.0.0.1"), QStringLiteral("/v1/chat/completions")),
            QStringLiteral(
                "      chat_template_kwargs:\n"
                "        enable_thinking: false\n"
                "      tokenizer: %1\n")
                .arg(mock.url(host, QStringLiteral("/tokenize"))));
    }

    /* A Messages API entry with extra model keys. */
    static QByteArray messages(const Mock &mock, const QString &extra)
    {
        return configuration(
            mock.url(QStringLiteral("127.0.0.1"), QStringLiteral("/v1/messages")),
            QStringLiteral("      api: anthropic-messages\n") + extra);
    }

    static QByteArray configuration(const QString &url, const QString &extra)
    {
        return (QStringLiteral(
                    "proxy:\n"
                    "  type: none\n"
                    "llm:\n"
                    "  model: lab\n"
                    "  models:\n"
                    "    lab:\n"
                    "      model: lab-served\n"
                    "      url: %1\n"
                    "      key: placeholder-key\n"
                    "      timeout: 1500\n"
                    "      context: 20000\n"
                    "      max_output_tokens: 2000\n")
                    .arg(url)
                + extra)
            .toUtf8();
    }

    /* Threshold of the first gate: the prune fraction of the input budget. */
    qint64 gate() const
    {
        return qint64(agent->effectiveContextTokens() * agent->getConfig().pruneThreshold);
    }

    ScopedConfig                 scope;
    QSocConfig                   config;
    std::unique_ptr<QLLMService> llm;
    QSocToolRegistry             registry;
    std::unique_ptr<QSocAgent>   agent;
    QTemporaryDir                artifacts;
    json                         history = json::array();
};

QSocRequestSnapshot request(QSocTokenizer::Mode counter)
{
    QSocRequestSnapshot result;
    result.route    = QStringLiteral("route");
    result.counter  = counter;
    result.messages = json::array(
        {{{"role", "system"}, {"content", "Follow the task."}},
         {{"role", "user"}, {"content", "Count the registers in the clock domain."}}});
    return result;
}

class Test final : public QObject
{
    Q_OBJECT
private slots:
    void tokenizerSettingIsValidated()
    {
        ScopedConfig scope(
            "llm:\n"
            "  model: plain\n"
            "  models:\n"
            "    plain:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "    local:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: o200k\n"
            "    rough:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: BYTES\n"
            "    typo:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: o200\n"
            "    served:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: http://127.0.0.1:9/Tokenize\n"
            "    ftp:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: ftp://127.0.0.1/tokenize\n");
        QSocTestCapture capture;
        QSocConfig      config;
        QLLMService     llm(nullptr, &config);
        QCOMPARE(llm.getModelConfig(QStringLiteral("plain")).tokenizer, QStringLiteral("auto"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("local")).tokenizer, QStringLiteral("o200k"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("rough")).tokenizer, QStringLiteral("bytes"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("typo")).tokenizer, QStringLiteral("auto"));
        QCOMPARE(
            llm.getModelConfig(QStringLiteral("served")).tokenizer,
            QStringLiteral("http://127.0.0.1:9/Tokenize"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("ftp")).tokenizer, QStringLiteral("auto"));
        QVERIFY(capture.text().contains(QStringLiteral("invalid tokenizer")));
        QCOMPARE(QSocRequestUsage::counterFor(QStringLiteral("bytes")), QSocTokenizer::Mode::Bytes);
        QCOMPARE(QSocRequestUsage::counterFor(QStringLiteral("auto")), QSocTokenizer::Mode::O200k);
    }

    void anchoredEstimateWidensOnlyTheLocalPart()
    {
        QSocRequestUsage usage;
        const auto       first = request(QSocTokenizer::Mode::O200k);
        QVERIFY(usage.complete(usage.begin(first), {{"prompt_tokens", 2000}}));
        const auto exact = usage.estimate(first);
        QCOMPARE(exact.reported, qint64(2000));
        QCOMPARE(exact.counted, qint64(0));
        QCOMPARE(exact.upper(), qint64(2000));
        QVERIFY(!exact.approximate());

        auto next = first;
        next.messages.push_back({{"role", "assistant"}, {"content", "Twelve registers."}});
        const auto   grown = usage.estimate(next);
        const qint64 local = QSocRequestUsage::estimateMessages(
            json::array({next.messages.back()}), next.imageTokens);
        QCOMPARE(grown.reported, qint64(2000));
        QCOMPARE(grown.counted, local);
        QCOMPARE(grown.point(), 2000 + local);
        QCOMPARE(grown.upper(), 2000 + qint64(std::ceil(local * 1.3)));
        QVERIFY(grown.approximate());
    }

    void countEndpointIsAskedOnlyInsideTheMargin()
    {
        Mock mock({{QStringLiteral("MOCK_TOKENIZE_COUNT"), QStringLiteral("5000")}});
        QVERIFY(mock.ready());
        Session    session(Session::chat(mock, QStringLiteral("127.0.0.1")));
        const auto estimate = session.agent->contextEstimate();
        QVERIFY(estimate.point() <= session.gate() && estimate.upper() > session.gate());
        QCOMPARE(session.agent->compactIfNeeded(), 0);
        QCOMPARE(mock.hits("tokenize"), 1);
        QCOMPARE(mock.hits("tokenize_auth"), 1);
        QCOMPARE(session.agent->contextEstimate().point(), qint64(5000));
        QVERIFY(!session.agent->contextEstimate().approximate());
        QCOMPARE(session.agent->compactIfNeeded(), 0);
        QCOMPARE(mock.hits("tokenize"), 1);

        const auto bodies = mock.countBodies();
        QCOMPARE(bodies.size(), 1);
        const json &body = bodies.front();
        QCOMPARE(body.value("model", std::string()), std::string("lab-served"));
        QCOMPARE(body.value("add_generation_prompt", false), true);
        QCOMPARE(body.at("chat_template_kwargs").value("enable_thinking", true), false);
        QCOMPARE(body.at("messages").size(), session.history.size() + 1);
        QCOMPARE(body.at("messages").front().value("role", std::string()), std::string("system"));
        QCOMPARE(
            body.at("tools").front().at("function").value("name", std::string()),
            std::string("probe"));
    }

    void smallSessionNeverAsks()
    {
        Mock mock({});
        QVERIFY(mock.ready());
        Session session(Session::chat(mock, QStringLiteral("127.0.0.1")), 400);
        QVERIFY(session.agent->contextEstimate().upper() <= session.gate());
        QCOMPARE(session.agent->compactIfNeeded(), 0);
        QCOMPARE(mock.hits("tokenize"), 0);
    }

    void otherOriginGetsNoKey()
    {
        Mock mock({{QStringLiteral("MOCK_TOKENIZE_COUNT"), QStringLiteral("5000")}});
        QVERIFY(mock.ready());
        Session session(Session::chat(mock, QStringLiteral("localhost")));
        QCOMPARE(session.agent->compactIfNeeded(), 0);
        QCOMPARE(mock.hits("tokenize"), 1);
        QCOMPARE(mock.hits("tokenize_auth"), 0);
        QVERIFY(
            QLLMService::sameOrigin(
                QUrl(QStringLiteral("https://api.example.com/v1/chat")),
                QUrl(QStringLiteral("https://API.example.com:443/tokenize"))));
        QVERIFY(!QLLMService::sameOrigin(
            QUrl(QStringLiteral("https://api.example.com/v1/chat")),
            QUrl(QStringLiteral("http://api.example.com/tokenize"))));
        QVERIFY(!QLLMService::sameOrigin(
            QUrl(QStringLiteral("http://lab:8000/v1/chat")),
            QUrl(QStringLiteral("http://lab:8001/tokenize"))));
    }

    void failingEndpointFallsBackOnce_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"404", "500", "429", "html", "hold"}) {
            QTest::newRow(mode) << QString::fromLatin1(mode);
        }
    }

    void failingEndpointFallsBackOnce()
    {
        QFETCH(QString, mode);
        Mock mock({{QStringLiteral("MOCK_TOKENIZE"), mode}});
        QVERIFY(mock.ready());
        Session    session(Session::chat(mock, QStringLiteral("127.0.0.1")));
        QSignalSpy fellBack(session.agent.get(), &QSocAgent::tokenCountFellBack);
        session.agent->compactIfNeeded();
        QCOMPARE(mock.hits("tokenize"), 1);
        QCOMPARE(fellBack.count(), 1);
        QVERIFY(fellBack.front().front().toString().contains(QStringLiteral("lab")));
        QVERIFY(session.agent->contextEstimate().approximate());

        session.agent->setMessages(session.history);
        session.agent->compactIfNeeded();
        QCOMPARE(mock.hits("tokenize"), 1);
        QCOMPARE(fellBack.count(), 1);

        session.agent->resetTokenCounting();
        session.agent->setMessages(session.history);
        session.agent->compactIfNeeded();
        QCOMPARE(mock.hits("tokenize"), 2);
    }

    void countMustMatchTheReportedUsage_data()
    {
        QTest::addColumn<int>("reported");
        QTest::addColumn<int>("fallbacks");
        QTest::newRow("agrees") << 5100 << 0;
        QTest::newRow("ten-percent-off") << 5500 << 1;
    }

    void countMustMatchTheReportedUsage()
    {
        QFETCH(int, reported);
        QFETCH(int, fallbacks);
        Mock mock(
            {{QStringLiteral("MOCK_TOKENIZE_COUNT"), QStringLiteral("5000")},
             {QStringLiteral("MOCK_PROMPT_TOKENS"), QString::number(reported)}});
        QVERIFY(mock.ready());
        Session    session(Session::chat(mock, QStringLiteral("127.0.0.1")));
        QSignalSpy fellBack(session.agent.get(), &QSocAgent::tokenCountFellBack);
        session.agent->run(QStringLiteral("Count the registers."));
        QCOMPARE(mock.hits("tokenize"), 1);
        QCOMPARE(fellBack.count(), fallbacks);

        session.agent->setMessages(session.history);
        session.agent->compactIfNeeded();
        QCOMPARE(mock.hits("tokenize"), 2 - fallbacks);
    }

    void messagesApiCountsWithCountTokens()
    {
        Mock mock({{QStringLiteral("MOCK_TOKENIZE_COUNT"), QStringLiteral("5000")}});
        QVERIFY(mock.ready());
        Session session(Session::messages(mock, QStringLiteral("      effort: high\n")));
        auto    settings     = session.agent->getConfig();
        settings.effortLevel = QStringLiteral("high");
        session.agent->setConfig(settings);
        QCOMPARE(session.agent->compactIfNeeded(), 0);
        QCOMPARE(mock.hits("count_tokens"), 1);
        QCOMPARE(mock.hits("count_tokens_auth"), 1);
        QCOMPARE(mock.hits("tokenize"), 0);
        QCOMPARE(session.agent->contextEstimate().point(), qint64(5000));

        const auto bodies = mock.countBodies();
        QCOMPARE(bodies.size(), 1);
        const json &body = bodies.front();
        for (const auto &[key, value] : body.items()) {
            QVERIFY2(
                QStringList({"model", "system", "messages", "tools", "tool_choice", "thinking"})
                    .contains(QString::fromStdString(key)),
                key.c_str());
        }
        QCOMPARE(body.value("model", std::string()), std::string("lab-served"));
        for (const char *field : {"system", "messages", "tools", "thinking"}) {
            QVERIFY2(body.contains(field), field);
        }
    }

    void o200kOnTheMessagesApiNeverCounts()
    {
        Mock mock({});
        QVERIFY(mock.ready());
        Session session(Session::messages(mock, QStringLiteral("      tokenizer: o200k\n")));
        QVERIFY(session.agent->contextEstimate().upper() > session.gate());
        session.agent->compactIfNeeded();
        QCOMPARE(mock.hits("count_tokens"), 0);
    }

    void messagesCountMustMatchTheReportedUsage_data()
    {
        QTest::addColumn<int>("reported");
        QTest::addColumn<int>("fallbacks");
        QTest::newRow("agrees") << 5100 << 0;
        QTest::newRow("ten-percent-off") << 5500 << 1;
    }

    void messagesCountMustMatchTheReportedUsage()
    {
        QFETCH(int, reported);
        QFETCH(int, fallbacks);
        Mock mock(
            {{QStringLiteral("MOCK_TOKENIZE_COUNT"), QStringLiteral("5000")},
             {QStringLiteral("MOCK_PROMPT_TOKENS"), QString::number(reported)}});
        QVERIFY(mock.ready());
        Session    session(Session::messages(mock, QString()));
        QSignalSpy fellBack(session.agent.get(), &QSocAgent::tokenCountFellBack);
        session.agent->run(QStringLiteral("Count the registers."));
        QCOMPARE(mock.hits("count_tokens"), 1);
        QCOMPARE(fellBack.count(), fallbacks);
    }

    void unanchoredEstimateUsesTheCounterMargin()
    {
        const QSocRequestUsage usage;
        for (const auto counter : {QSocTokenizer::Mode::O200k, QSocTokenizer::Mode::Bytes}) {
            const auto snapshot = request(counter);
            const auto estimate = usage.estimate(snapshot);
            const auto margin   = counter == QSocTokenizer::Mode::Bytes ? 1.5 : 1.3;
            QCOMPARE(estimate.reported, qint64(0));
            QCOMPARE(estimate.counted, QSocRequestUsage::estimateRequest(snapshot));
            QCOMPARE(estimate.margin, margin);
            QCOMPARE(estimate.upper(), qint64(std::ceil(estimate.counted * margin)));
        }
        QVERIFY(
            QSocRequestUsage::estimateRequest(request(QSocTokenizer::Mode::Bytes))
            != QSocRequestUsage::estimateRequest(request(QSocTokenizer::Mode::O200k)));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctokencount.moc"
