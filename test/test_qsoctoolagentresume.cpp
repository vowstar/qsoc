// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocdispatchpolicy.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolagent.h"
#include "agent/tool/qsoctoolagentresume.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QQueue>
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

    json requestBody(int index) const { return json::parse(bodies_.at(index).toStdString()); }

    void enqueueToolCall(const QString &name) { toolCalls_.enqueue(name); }

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

        json    delta  = {{"content", "delegated work done"}};
        QString finish = QStringLiteral("stop");
        if (!toolCalls_.isEmpty()) {
            delta = {
                {"tool_calls",
                 json::array(
                     {{{"index", 0},
                       {"id", "probe-call"},
                       {"type", "function"},
                       {"function",
                        {{"name", toolCalls_.dequeue().toStdString()}, {"arguments", "{}"}}}}})}};
            finish = QStringLiteral("tool_calls");
        }
        const json contentChunk = {{"choices", json::array({{{"delta", delta}}})}};
        const json finishChunk  = {
            {"choices",
             json::array({{{"delta", json::object()}, {"finish_reason", finish.toStdString()}}})}};
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
    QQueue<QString>                 toolCalls_;
    QTcpServer                      server_;
};

QByteArray twoModels(const QString &url)
{
    return QByteArrayLiteral(
               "llm:\n"
               "  model: first-model\n"
               "  models:\n"
               "    first-model:\n"
               "      model: first-wire\n"
               "      url: ")
           + url.toUtf8()
           + QByteArrayLiteral(
               "\n"
               "      timeout: 3000\n"
               "    second-model:\n"
               "      model: second-wire\n"
               "      url: ")
           + url.toUtf8() + QByteArrayLiteral("\n      timeout: 3000\n");
}

QSocAgentDefinition probeDefinition()
{
    QSocAgentDefinition def;
    def.name        = QStringLiteral("probe");
    def.description = QStringLiteral("resume probe");
    def.promptBody  = QStringLiteral("Answer briefly.");
    return def;
}

QSocAgentConfig quietConfig()
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

/* Whether one request carries a user-role message with exactly @p text. */
bool hasUserMessage(const MockLlm &llm, int request, const std::string &text)
{
    for (const auto &message : llm.requestBody(request).value("messages", json::array())) {
        if (message.value("role", std::string()) == "user" && message.contains("content")
            && message["content"].is_string() && message["content"].get<std::string>() == text) {
            return true;
        }
    }
    return false;
}

/* How many messages of one request contain @p text. */
int mentionCount(const MockLlm &llm, int request, const std::string &text)
{
    int count = 0;
    for (const auto &message : llm.requestBody(request).value("messages", json::array())) {
        count += message.dump().find(text) != std::string::npos ? 1 : 0;
    }
    return count;
}

bool requestMentions(const MockLlm &llm, int request, const std::string &text)
{
    return llm.requestBody(request).value("messages", json::array()).dump().find(text)
           != std::string::npos;
}

/* A parent, its spawn tool and a task source bound to one run directory. */
struct Harness
{
    explicit Harness(const QString &runDir)
        : service(nullptr, &serviceConfig)
        , parent(nullptr, &service, &registry, quietConfig())
        , tool(nullptr, &service, &registry, quietConfig(), &definitions, &tasks)
    {
        definitions.registerDefinition(probeDefinition());
        tasks.setTranscriptDir(runDir);
        tasks.enableMessaging(&parent);
        tool.setParentAgent(&parent);
        registry.registerTool(&tool);
    }

    json spawn(const std::string &prompt)
    {
        const json args
            = {{"subagent_type", "probe"},
               {"description", "resume probe"},
               {"prompt", prompt},
               {"run_in_background", false}};
        return json::parse(tool.execute(args).toStdString(), nullptr, false);
    }

    QSocConfig                  serviceConfig;
    QLLMService                 service;
    QSocAgentDefinitionRegistry definitions;
    QSocSubAgentTaskSource      tasks;
    QSocToolRegistry            registry;
    QSocAgent                   parent;
    QSocToolAgent               tool;
};

class Test : public QObject
{
    Q_OBJECT

private:
    QSocAgent *makeAgent() { return new QSocAgent(this, nullptr, nullptr, QSocAgentConfig()); }

private slots:
    void initTestCase() {}

    void testNameAndSchema()
    {
        QSocSubAgentTaskSource src;
        QSocToolAgentResume    tool(this, &src);
        QCOMPARE(tool.getName(), QStringLiteral("agent_resume"));
        QVERIFY(!tool.getDescription().isEmpty());
        const json schema = tool.getParametersSchema();
        QVERIFY(schema["properties"].contains("task_id"));
        QVERIFY(schema["properties"].contains("new_instructions"));
        QVERIFY(schema["properties"].contains("max_tail_bytes"));
    }

    void testUnknownTaskIdReturnsError()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        QSocToolAgentResume tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", "nope"}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("no metadata") != std::string::npos);
    }

    void testResumePayloadCarriesPriorContext()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());

        const QString runId
            = src.registerRun(QStringLiteral("rtl-research"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(runId, QStringLiteral("found module clk_gen at clk.v:42\n"));
        src.markCompleted(runId, QStringLiteral("PRIOR FINAL"));

        QSocToolAgentResume tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", runId.toStdString()}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("ok"));
        QCOMPARE(parsed["original_subagent_type"].get<std::string>(), std::string("explore"));
        QCOMPARE(parsed["original_label"].get<std::string>(), std::string("rtl-research"));
        QCOMPARE(parsed["original_status"].get<std::string>(), std::string("completed"));
        const std::string resume = parsed["resume_prompt"].get<std::string>();
        QVERIFY(resume.find("RESUMING") != std::string::npos);
        QVERIFY(resume.find("rtl-research") != std::string::npos);
        QVERIFY(resume.find("clk_gen") != std::string::npos);
        QVERIFY(resume.find("PRIOR FINAL") != std::string::npos);
    }

    void userMessageWakesAnIdleLiveChild()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig  scope(twoModels(llm.url()));
        QTemporaryDir runs;
        Harness       h(runs.path());
        const json    spawned = h.spawn("first objective");
        QCOMPARE(spawned.value("status", std::string()), std::string("ok"));
        const QString taskId = QString::fromStdString(spawned.value("task_id", std::string()));
        QCOMPARE(llm.requestCount(), 1);
        QVERIFY(h.tasks.liveAgentFor(taskId) != nullptr);

        const json receipt = h.tasks.sendFromUser(taskId, QStringLiteral("user follow-up sentinel"));
        QCOMPARE(receipt.value("status", std::string()), std::string("ok"));
        QCOMPARE(receipt.value("delivery", std::string()), std::string("woken"));
        QVERIFY(receipt.value("task_id", std::string()) != taskId.toStdString());
        QTRY_COMPARE(llm.requestCount(), 2);
        QVERIFY(hasUserMessage(llm, 1, "user follow-up sentinel"));
        QVERIFY(requestMentions(llm, 1, "first objective"));
        QCOMPARE(mentionCount(llm, 1, "user follow-up sentinel"), 1);
        QTRY_VERIFY(!h.tasks.liveAgentFor(taskId)->isRunning());
        QCOMPARE(h.tasks.mailbox()->pendingCount(h.parent.agentIdentity()), 0);
    }

    void finishedChildResumesFromStoredHistory()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig  scope(twoModels(llm.url()));
        QTemporaryDir runs;
        QString       taskId;
        {
            Harness    first(runs.path());
            const json spawned = first.spawn("first objective");
            QCOMPARE(spawned.value("status", std::string()), std::string("ok"));
            taskId = QString::fromStdString(spawned.value("task_id", std::string()));
        }
        QCOMPARE(llm.requestCount(), 1);

        /* A later process: the child is gone, its stored history is not. */
        Harness later(runs.path());
        QCOMPARE(later.tasks.liveAgentFor(taskId), nullptr);
        const json resumed = later.tool.resumeRun(
            taskId,
            QStringLiteral("continue sentinel"),
            later.tasks.mailbox()->idFor(&later.parent));
        QCOMPARE(resumed.value("resume", std::string()), std::string("history"));
        QCOMPARE(resumed.value("status", std::string()), std::string("async_launched"));
        QVERIFY(resumed.value("task_id", std::string()) != taskId.toStdString());
        QTRY_COMPARE(llm.requestCount(), 2);
        QVERIFY(hasUserMessage(llm, 1, "first objective"));
        QVERIFY(hasUserMessage(llm, 1, "continue sentinel"));
        QVERIFY(requestMentions(llm, 1, "delegated work done"));
    }

    void resumedChildUsesTheCurrentModelAndEffort()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig  scope(twoModels(llm.url()));
        QTemporaryDir runs;
        QString       taskId;
        {
            Harness first(runs.path());
            first.parent.setEffortLevel(QStringLiteral("low"));
            taskId = QString::fromStdString(
                first.spawn("first objective").value("task_id", std::string()));
        }
        QCOMPARE(llm.wireModel(0), QStringLiteral("first-wire"));
        QCOMPARE(llm.requestBody(0).value("reasoning_effort", std::string()), std::string("low"));

        Harness later(runs.path());
        QVERIFY(later.service.setCurrentModel(QStringLiteral("second-model")));
        later.parent.setEffortLevel(QStringLiteral("high"));
        const json resumed
            = later.tool.resumeRun(taskId, QStringLiteral("go on"), QSocAgentMailbox::userSender());
        QCOMPARE(resumed.value("resume", std::string()), std::string("history"));
        QTRY_COMPARE(llm.requestCount(), 2);
        QCOMPARE(llm.wireModel(1), QStringLiteral("second-wire"));
        QCOMPARE(llm.requestBody(1).value("reasoning_effort", std::string()), std::string("high"));
    }

    /* Counterexample: a rebuilt child ran on the model its earlier call named
     * although the user had since taken that model away from sub-agents. */
    void aResumedChildAsksForItsModelAgain()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        const QByteArray yaml   = twoModels(llm.url());
        const YAML::Node models = YAML::Load(yaml.toStdString())["llm"]["models"];
        const auto       policy = [&models](const char *dispatch) {
            return QSocDispatchPolicy::fromNodes(YAML::Load(dispatch), models, nullptr, nullptr);
        };
        ScopedConfig  scope(yaml);
        QTemporaryDir runs;
        QString       taskId;
        {
            Harness first(runs.path());
            first.tool.setDispatchPolicy(policy("models: [second-model]\n"));
            const json args
                = {{"subagent_type", "probe"},
                   {"description", "resume probe"},
                   {"prompt", "first objective"},
                   {"model", "second-model"}};
            const json done = json::parse(first.tool.execute(args).toStdString());
            QCOMPARE(done.value("status", std::string()), std::string("ok"));
            taskId = QString::fromStdString(done.value("task_id", std::string()));
        }
        QCOMPARE(llm.wireModel(0), QStringLiteral("second-wire"));

        Harness revoked(runs.path());
        revoked.tool.setDispatchPolicy(policy("{}"));
        const json refused
            = revoked.tool.resumeRun(taskId, QStringLiteral("go on"), QSocAgentMailbox::userSender());
        QCOMPARE(refused.value("status", std::string()), std::string("error"));
        QVERIFY2(
            refused.value("error", std::string()).find("second-model") != std::string::npos,
            refused.dump().c_str());
        QCOMPARE(llm.requestCount(), 1);

        Harness granted(runs.path());
        granted.tool.setDispatchPolicy(policy("models: [second-model]\n"));
        const json resumed
            = granted.tool.resumeRun(taskId, QStringLiteral("go on"), QSocAgentMailbox::userSender());
        QCOMPARE(resumed.value("resume", std::string()), std::string("history"));
        QTRY_COMPARE(llm.requestCount(), 2);
        QCOMPARE(llm.wireModel(1), QStringLiteral("second-wire"));
    }

    void runWithoutStoredHistoryFallsBackToThePrompt()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("old"), QStringLiteral("explore"), makeAgent());
        src.markCompleted(runId, QStringLiteral("PRIOR FINAL"));
        QSocAgentDefinitionRegistry definitions;
        QSocToolAgent spawner(this, nullptr, nullptr, QSocAgentConfig(), &definitions, &src);
        const json    direct = spawner.resumeRun(runId, {}, QSocAgentMailbox::userSender());
        QCOMPARE(direct.value("resume", std::string()), std::string("unavailable"));
        QSocToolAgentResume tool(this, &src, &spawner);
        const json          parsed = json::parse(
            tool.execute(json{{"task_id", runId.toStdString()}}).toStdString());
        QCOMPARE(parsed.value("resume", std::string()), std::string("prompt_only"));
        QVERIFY(
            parsed.value("resume_prompt", std::string()).find("PRIOR FINAL") != std::string::npos);
    }

    void testNewInstructionsAppendedToResumePrompt()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("dummy"), QStringLiteral("verification"), makeAgent());
        src.markCompleted(runId, QStringLiteral("PASS"));

        QSocToolAgentResume tool(this, &src);
        const QString       out = tool.execute(
            json{
                {"task_id", runId.toStdString()},
                {"new_instructions", "Continue with the second test suite."}});
        const json    parsed       = json::parse(out.toStdString());
        const QString resumePrompt = QString::fromStdString(
            parsed["resume_prompt"].get<std::string>());
        QVERIFY(resumePrompt.contains(QStringLiteral("New instructions:")));
        QVERIFY(resumePrompt.contains(QStringLiteral("second test suite")));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagentresume.moc"
