// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"
#include "qsoc_test_pty.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

using namespace QSocTestPty;

const QString kThinking = QStringLiteral("weighing the options");

/* One mock process per test, killed by the destructor. */
struct Mock
{
    QTemporaryDir dir{QDir::tempPath() + QStringLiteral("/test_qsoc_reason_XXXXXX")};
    std::unique_ptr<BoundedProcess> process = std::make_unique<BoundedProcess>();
    int                             port    = 0;

    QString log() const { return QDir(dir.path()).filePath(QStringLiteral("requests.jsonl")); }

    bool start(const QMap<QString, QString> &variables)
    {
        port = pickFreePort();
        if (!dir.isValid() || port <= 0) {
            return false;
        }
        auto environment = isolatedEnvironment(dir.path());
        environment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
        environment.insert(QStringLiteral("MOCK_REQUEST_LOG"), log());
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
            environment.insert(it.key(), it.value());
        }
        process->setProcessEnvironment(environment);
        process->setStandardOutputFile(QDir(dir.path()).filePath(QStringLiteral("mock.out")));
        process->setStandardErrorFile(QDir(dir.path()).filePath(QStringLiteral("mock.err")));
        process->start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), {QString::number(port)});
        return process->waitForStarted(5000) && waitForMockReady(*process, port, 45000);
    }

    LLMModelConfig model() const
    {
        LLMModelConfig config;
        config.id      = QStringLiteral("mock");
        config.name    = config.id;
        config.model   = config.id;
        config.url     = QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(port);
        config.timeout = 20000;
        return config;
    }

    QList<json> requests() const
    {
        QFile file(log());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QList<json> out;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.trimmed().isEmpty()) {
                out.append(json::parse(line.toStdString()));
            }
        }
        return out;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void streamKeepsProviderField_data()
    {
        QTest::addColumn<QString>("fields");
        QTest::addColumn<QString>("expected");
        QTest::newRow("reasoning") << QStringLiteral("reasoning") << QStringLiteral("reasoning");
        QTest::newRow("reasoning_content")
            << QStringLiteral("reasoning_content") << QStringLiteral("reasoning_content");
        QTest::newRow("reasoning_text")
            << QStringLiteral("reasoning_text") << QStringLiteral("reasoning_text");
        QTest::newRow("mirrored") << QStringLiteral("reasoning,reasoning_content")
                                  << QStringLiteral("reasoning_content");
    }

    void streamKeepsProviderField()
    {
        QFETCH(QString, fields);
        QFETCH(QString, expected);
        Mock mock;
        QVERIFY(mock.start(
            {{QStringLiteral("MOCK_REASONING"), kThinking},
             {QStringLiteral("MOCK_REASONING_FIELD"), fields}}));

        QLLMService service;
        service.setModel(mock.model());
        QString thinking;
        connect(&service, &QLLMService::streamReasoningChunk, this, [&thinking](const QString &c) {
            thinking += c;
        });
        QList<json> done;
        connect(&service, &QLLMService::streamComplete, this, [&done](const json &response) {
            done.append(response);
        });
        service.sendChatCompletionStream(json::array({{{"role", "user"}, {"content", "hi"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);

        QCOMPARE(thinking, kThinking);
        const json message = done.at(0)["choices"][0]["message"];
        for (const char *field : {"reasoning", "reasoning_content", "reasoning_text"}) {
            QCOMPARE(message.contains(field), expected == QLatin1String(field));
        }
        QCOMPARE(message[expected.toStdString()].get<std::string>(), kThinking.toStdString());
    }

    void toolRoundReplaysThinking()
    {
        Mock mock;
        QVERIFY(mock.start(
            {{QStringLiteral("MOCK_REASONING"), kThinking},
             {QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("no_such_tool")},
             {QStringLiteral("MOCK_TOOL_MAX"), QStringLiteral("1")}}));

        QLLMService service;
        service.setModel(mock.model());
        QSocToolRegistry registry;
        QSocAgentConfig  config;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxRetries          = 0;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy complete(&agent, &QSocAgent::runComplete);
        QSignalSpy failed(&agent, &QSocAgent::runError);
        agent.runStream(QStringLiteral("use a tool"));
        QTRY_VERIFY_WITH_TIMEOUT(complete.count() + failed.count() > 0, 20000);
        QCOMPARE(failed.count(), 0);

        const QList<json> requests = mock.requests();
        QVERIFY(requests.size() >= 2);
        const json &messages = requests.at(1)["messages"];
        bool        replayed = false;
        for (const json &message : messages) {
            if (message.value("role", std::string()) == "assistant"
                && message.contains("tool_calls")) {
                QCOMPARE(message.value("reasoning", std::string()), kThinking.toStdString());
                QVERIFY(!message.contains("reasoning_content"));
                replayed = true;
            }
        }
        QVERIFY(replayed);
    }

    void reasoningFalseSendsNoEffort_data()
    {
        QTest::addColumn<bool>("reasoning");
        QTest::newRow("true") << true;
        QTest::newRow("false") << false;
    }

    void reasoningFalseSendsNoEffort()
    {
        QFETCH(bool, reasoning);
        Mock mock;
        QVERIFY(mock.start({}));
        QLLMService    service;
        LLMModelConfig model = mock.model();
        model.reasoning      = reasoning;
        service.setModel(model);
        int done = 0;
        connect(&service, &QLLMService::streamComplete, this, [&done](const json &) { ++done; });
        service.sendChatCompletionStream(
            json::array({{{"role", "user"}, {"content", "hi"}}}),
            json::array(),
            0.2,
            QStringLiteral("high"));
        QTRY_COMPARE_WITH_TIMEOUT(done, 1, 10000);
        const json sync = service.sendChatCompletion(
            json::array({{{"role", "user"}, {"content", "hi"}}}),
            json::array(),
            0.2,
            {},
            QStringLiteral("high"));
        QVERIFY(!sync.contains("error"));

        const QList<json> requests = mock.requests();
        QCOMPARE(requests.size(), 2);
        for (const json &request : requests) {
            QCOMPARE(request.contains("reasoning_effort"), reasoning);
            QCOMPARE(request.contains("temperature"), !reasoning);
        }
    }

    void configReadsReasoningKey()
    {
        QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/test_qsoc_reason_XXXXXX"));
        QVERIFY(dir.isValid());
        QFile file(QDir(dir.path()).filePath(QStringLiteral("qsoc.yml")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(
            "llm:\n"
            "  model: plain\n"
            "  models:\n"
            "    plain: {url: \"http://127.0.0.1:9/v1/chat/completions\", reasoning: false}\n"
            "    thinker: {url: \"http://127.0.0.1:9/v1/chat/completions\"}\n");
        file.close();
        const auto restore = [](const char *name, bool wasSet, const QByteArray &value) {
            if (wasSet) {
                qputenv(name, value);
            } else {
                qunsetenv(name);
            }
        };
        const bool       hadHome = qEnvironmentVariableIsSet("QSOC_HOME");
        const bool       hadXdg  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        const QByteArray oldHome = qgetenv("QSOC_HOME");
        const QByteArray oldXdg  = qgetenv("XDG_CONFIG_HOME");
        qputenv("QSOC_HOME", dir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
        QSocConfig  config;
        QLLMService service(nullptr, &config);
        const bool  plain   = service.getModelConfig(QStringLiteral("plain")).reasoning;
        const bool  thinker = service.getModelConfig(QStringLiteral("thinker")).reasoning;
        restore("QSOC_HOME", hadHome, oldHome);
        restore("XDG_CONFIG_HOME", hadXdg, oldXdg);
        QVERIFY(!plain);
        QVERIFY(thinker);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmservicereasoning.moc"
