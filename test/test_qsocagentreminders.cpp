// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocgoal.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolgoalcomplete.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QHash>
#include <QHostAddress>
#include <QQueue>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class MockAgentServer final : public QObject
{
public:
    MockAgentServer()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                buffers_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    consumeRequest(socket);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(server_.serverAddress().toString());
        result.setPort(server_.serverPort());
        result.setPath(QStringLiteral("/chat/completions"));
        return result;
    }

    void enqueue(const json &response)
    {
        responses_.enqueue(QByteArray::fromStdString(response.dump()));
    }

    int requestCount() const { return requests_.size(); }

    const json &request(int index) const { return requests_.at(index); }

private:
    void consumeRequest(QTcpSocket *socket)
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

        const QByteArray requestBody = buffer.mid(bodyStart, contentLength);
        buffers_.remove(socket);
        requests_.append(json::parse(requestBody.toStdString(), nullptr, false));

        const QByteArray responseBody = responses_.isEmpty()
                                            ? QByteArrayLiteral(
                                                  R"({"error":{"message":"missing response"}})")
                                            : responses_.dequeue();
        QByteArray       headers      = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
        headers += QByteArray::number(responseBody.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + responseBody);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QQueue<QByteArray>              responses_;
    QList<json>                     requests_;
    QTcpServer                      server_;
};

class ExecutionProbeTool final : public QSocTool
{
public:
    ExecutionProbeTool(QString name, QObject *parent)
        : QSocTool(parent)
        , name_(std::move(name))
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("Test execution probe"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        ++executeCount_;
        return QStringLiteral("executed");
    }

    int executeCount() const { return executeCount_; }

private:
    QString name_;
    int     executeCount_ = 0;
};

QString tailContent(const json &request)
{
    const auto messages = request.find("messages");
    if (messages == request.end() || !messages->is_array() || messages->empty()) {
        return {};
    }
    const auto content = messages->back().find("content");
    if (content == messages->back().end() || !content->is_string()) {
        return {};
    }
    return QString::fromStdString(content->get<std::string>());
}

json toolCall(const char *id, const char *name, const json &arguments)
{
    json call;
    call["id"]                    = id;
    call["type"]                  = "function";
    call["function"]["name"]      = name;
    call["function"]["arguments"] = arguments.dump();
    return call;
}

json assistantResponse(json message)
{
    json response;
    response["choices"] = json::array();
    response["choices"].push_back({{"message", std::move(message)}});
    return response;
}

void configureTestService(QLLMService &service, const QUrl &url)
{
    LLMEndpoint endpoint;
    endpoint.name    = QStringLiteral("local-test");
    endpoint.url     = url;
    endpoint.model   = QStringLiteral("test-model");
    endpoint.timeout = 3000;
    service.addEndpoint(endpoint);
}

QSet<QString> requestToolNames(const json &request)
{
    QSet<QString> names;
    const auto    tools = request.find("tools");
    if (tools == request.end() || !tools->is_array()) {
        return names;
    }
    for (const auto &definition : *tools) {
        names.insert(
            QString::fromStdString(definition.at("function").at("name").get<std::string>()));
    }
    return names;
}

} // namespace

/*
 * Wire-contract tests for QSocAgent::appendTurnReminder. The per-turn
 * ephemeral reminders must ride as trailing <system-reminder> user-turn
 * content: the cached system prefix (messages[0]) stays byte-stable and no
 * role:"system" message is ever emitted after the history, which some chat
 * templates reject.
 */
class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Empty content is a no-op: the wire is untouched. */
    void testEmptyContentIsNoop()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "hi"}});
        const json before = wire;
        QSocAgent::appendTurnReminder(wire, QString());
        QVERIFY(wire == before);
    }

    /* Content is wrapped in <system-reminder> tags. */
    void testWrapsInSystemReminderTags()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "hi"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("stay on task"));
        const std::string content = wire.back()["content"].get<std::string>();
        QVERIFY(
            content.find("<system-reminder>\nstay on task\n</system-reminder>")
            != std::string::npos);
    }

    /* A trailing user message is folded into, not duplicated. */
    void testFoldsIntoTrailingUserMessage()
    {
        json wire = json::array();
        wire.push_back({{"role", "user"}, {"content", "original"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
        const std::string content = wire.back()["content"].get<std::string>();
        QVERIFY(content.rfind("original", 0) == 0);
        QVERIFY(content.find("original\n\n<system-reminder>") != std::string::npos);
    }

    /* A trailing tool result is folded into rather than followed by a
     * second consecutive user turn. */
    void testFoldsIntoTrailingToolMessage()
    {
        json wire = json::array();
        wire.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "result"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("tool"));
        QVERIFY(
            wire.back()["content"].get<std::string>().find("<system-reminder>")
            != std::string::npos);
    }

    /* After an assistant turn, a fresh user message carries the reminder. */
    void testStartsFreshUserTurnAfterAssistant()
    {
        json wire = json::array();
        wire.push_back({{"role", "assistant"}, {"content", "done"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(2));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
    }

    /* Empty wire gets a single trailing user reminder. */
    void testEmptyWireGetsUserMessage()
    {
        json wire = json::array();
        QSocAgent::appendTurnReminder(wire, QStringLiteral("R"));
        QCOMPARE(wire.size(), static_cast<std::size_t>(1));
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));
    }

    /* Multiple reminders accumulate into one tail turn, and no role:"system"
     * message is ever introduced after the head. */
    void testMultipleRemindersAccumulateNoSystemRole()
    {
        json wire = json::array();
        wire.push_back({{"role", "system"}, {"content", "SYSTEM PROMPT"}});
        wire.push_back({{"role", "assistant"}, {"content", "done"}});
        QSocAgent::appendTurnReminder(wire, QStringLiteral("A"));
        QSocAgent::appendTurnReminder(wire, QStringLiteral("B"));
        QSocAgent::appendTurnReminder(wire, QStringLiteral("C"));
        /* system(0) + assistant(1) + one merged user(2). */
        QCOMPARE(wire.size(), static_cast<std::size_t>(3));

        int systemCount = 0;
        for (std::size_t i = 0; i < wire.size(); ++i) {
            if (wire[i]["role"].get<std::string>() == "system") {
                ++systemCount;
                QCOMPARE(i, static_cast<std::size_t>(0));
            }
        }
        QCOMPARE(systemCount, 1);
        QCOMPARE(wire.back()["role"].get<std::string>(), std::string("user"));

        const std::string tail = wire.back()["content"].get<std::string>();
        QVERIFY(tail.find("\nA\n") != std::string::npos);
        QVERIFY(tail.find("\nB\n") != std::string::npos);
        QVERIFY(tail.find("\nC\n") != std::string::npos);
    }

    void testPlanSubAgentReturnsToParentWithoutPrompting()
    {
        MockAgentServer server;
        QVERIFY(server.listen());

        json options = json::array();
        options.push_back({{"label", "First"}});
        options.push_back({{"label", "Second"}});
        json askArguments = {{"question", "Choose a path"}, {"options", options}};
        json calls        = json::array();
        calls.push_back(toolCall("ask", "ask_user", askArguments));
        calls.push_back(toolCall("shell", "bash", {{"command", "change state"}}));
        calls.push_back(toolCall("mutate", "mutating_probe", json::object()));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"tool_calls", std::move(calls)}}));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"content", "done"}}));

        QLLMService service;
        configureTestService(service, server.url());

        QSocToolRegistry registry;
        int              promptCount = 0;
        registry.registerTool(new QSocToolAskUser(
            &registry, [&](const QString &, const QString &, const QList<QSocAskUserOption> &) {
                ++promptCount;
                return QSocAskUserResult{QStringLiteral("First"), {}, false};
            }));
        auto *shell = new ExecutionProbeTool(QStringLiteral("bash"), &registry);
        registry.registerTool(shell);
        auto *mutating = new ExecutionProbeTool(QStringLiteral("mutating_probe"), &registry);
        registry.registerTool(mutating);

        QSocAgentConfig config;
        config.isSubAgent          = true;
        config.planMode            = true;
        config.verbose             = false;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxIterations       = 4;
        QSocAgent agent(nullptr, &service, &registry, config);
        agent.setUserWatchingProbe([]() { return false; });
        int judgeCount = 0;
        agent.setBashSafetyJudge([&](const QString &) {
            ++judgeCount;
            return QSocBashSafety{false, QStringLiteral("may change state")};
        });
        QSignalSpy results(&agent, &QSocAgent::toolResult);

        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(promptCount, 0);
        QCOMPARE(shell->executeCount(), 0);
        QCOMPARE(mutating->executeCount(), 0);
        QCOMPARE(judgeCount, 1);
        QCOMPARE(results.count(), 3);

        bool sawAskDenial      = false;
        bool sawShellDenial    = false;
        bool sawMutatingDenial = false;
        for (const QList<QVariant> &result : results) {
            const QString name = result.at(0).toString();
            const QString text = result.at(1).toString();
            if (name == QStringLiteral("ask_user")) {
                sawAskDenial = text.contains(QStringLiteral("cannot ask the user directly"));
            } else if (name == QStringLiteral("bash")) {
                sawShellDenial = text.contains(QStringLiteral("parent agent"))
                                 && !text.contains(QStringLiteral("exit_plan_mode"));
            } else if (name == QStringLiteral("mutating_probe")) {
                sawMutatingDenial = text.contains(QStringLiteral("parent agent"))
                                    && !text.contains(QStringLiteral("exit_plan_mode"));
            }
        }
        QVERIFY(sawAskDenial);
        QVERIFY(sawShellDenial);
        QVERIFY(sawMutatingDenial);

        for (int i = 0; i < server.requestCount(); ++i) {
            const json   &request = server.request(i);
            const QString tail    = tailContent(request);
            QVERIFY(tail.contains(QStringLiteral("parent")));
            QVERIFY(tail.contains(QStringLiteral("Do not call ask_user or exit_plan_mode")));
            QCOMPARE(tail.count(QStringLiteral("<system-reminder>")), 1);
            QVERIFY(!tail.contains(
                QStringLiteral("End every turn with either ask_user or exit_plan_mode")));
            QVERIFY(!tail.contains(QStringLiteral("not actively watching")));

            const QSet<QString> names = requestToolNames(request);
            QCOMPARE(names.size(), 1);
            QVERIFY(names.contains(QStringLiteral("bash")));
            QVERIFY(!names.contains(QStringLiteral("mutating_probe")));
            QVERIFY(!names.contains(QStringLiteral("ask_user")));
        }
    }

    void testSubAgentCannotCompleteParentGoal()
    {
        MockAgentServer server;
        QVERIFY(server.listen());

        json calls = json::array();
        calls.push_back(toolCall("complete", "goal_complete", {{"status", "complete"}}));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"tool_calls", std::move(calls)}}));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"content", "done"}}));

        QLLMService service;
        configureTestService(service, server.url());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("Finish parent objective"), 0, nullptr));
        QSignalSpy goalChanges(&catalog, &QSocGoalCatalog::goalChanged);

        QSocToolRegistry registry;
        registry.registerTool(new QSocToolGoalComplete(&registry, &catalog));
        registry.registerTool(new ExecutionProbeTool(QStringLiteral("file_read"), &registry));

        QSocAgentConfig config;
        config.isSubAgent          = true;
        config.toolsAllow          = {QStringLiteral("file_read"), QStringLiteral("goal_complete")};
        config.verbose             = false;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxIterations       = 4;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy results(&agent, &QSocAgent::toolResult);

        QCOMPARE(agent.run(QStringLiteral("inspect completion evidence")), QStringLiteral("done"));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(goalChanges.count(), 0);
        QCOMPARE(results.count(), 1);
        QCOMPARE(results.at(0).at(0).toString(), QStringLiteral("goal_complete"));
        const QString denial = results.at(0).at(1).toString();
        QVERIFY(denial.contains(QStringLiteral("parent agent")));
        QVERIFY(denial.contains(QStringLiteral("completion evidence")));

        const auto current = catalog.current();
        QVERIFY(current.has_value());
        QCOMPARE(current->objective, QStringLiteral("Finish parent objective"));
        QVERIFY(current->status == QSocGoalStatus::Active);

        for (int i = 0; i < server.requestCount(); ++i) {
            const QSet<QString> names = requestToolNames(server.request(i));
            QCOMPARE(names.size(), 1);
            QVERIFY(names.contains(QStringLiteral("file_read")));
            QVERIFY(!names.contains(QStringLiteral("goal_complete")));
        }
    }

    void testParentDenylistHidesAndRejectsTool()
    {
        MockAgentServer server;
        QVERIFY(server.listen());

        json calls = json::array();
        calls.push_back(toolCall("denied", "denied_probe", json::object()));
        calls.push_back(toolCall("allowed", "allowed_probe", json::object()));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"tool_calls", std::move(calls)}}));
        server.enqueue(assistantResponse({{"role", "assistant"}, {"content", "done"}}));

        QLLMService service;
        configureTestService(service, server.url());

        QSocToolRegistry registry;
        auto *allowed = new ExecutionProbeTool(QStringLiteral("allowed_probe"), &registry);
        registry.registerTool(allowed);
        auto *denied = new ExecutionProbeTool(QStringLiteral("denied_probe"), &registry);
        registry.registerTool(denied);

        QSocAgentConfig config;
        config.toolsAllow.clear();
        config.toolsDeny           = {QStringLiteral("denied_probe")};
        config.verbose             = false;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxIterations       = 4;
        QSocAgent  agent(nullptr, &service, &registry, config);
        QSignalSpy results(&agent, &QSocAgent::toolResult);

        QCOMPARE(agent.run(QStringLiteral("inspect allowed state")), QStringLiteral("done"));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(allowed->executeCount(), 1);
        QCOMPARE(denied->executeCount(), 0);
        QCOMPARE(results.count(), 2);

        bool sawAllowed = false;
        bool sawDenied  = false;
        for (const QList<QVariant> &result : results) {
            const QString name = result.at(0).toString();
            const QString text = result.at(1).toString();
            if (name == QStringLiteral("allowed_probe")) {
                sawAllowed = text == QStringLiteral("executed");
            } else if (name == QStringLiteral("denied_probe")) {
                sawDenied = text.contains(QStringLiteral("denylist"));
            }
        }
        QVERIFY(sawAllowed);
        QVERIFY(sawDenied);

        for (int i = 0; i < server.requestCount(); ++i) {
            const QSet<QString> names = requestToolNames(server.request(i));
            QCOMPARE(names.size(), 1);
            QVERIFY(names.contains(QStringLiteral("allowed_probe")));
            QVERIFY(!names.contains(QStringLiteral("denied_probe")));
        }
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentreminders.moc"
