// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsochookmanager.h"
#include "agent/qsocsession.h"
#include "agent/qsocsessionrecovery.h"
#include "agent/tool/qsoctooloutputread.h"
#include "agent/tool/qsoctoolweb.h"
#include "qsoc_test.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

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

    std::function<json(const json &, int)> respond;

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

        const json response
            = respond ? respond(requests_.last(), requests_.size() - 1)
                      : json(
                            {{"choices",
                              json::array(
                                  {{{"message", {{"role", "assistant"}, {"content", "done"}}}}})}});
        const bool stream = requests_.last().value("stream", false);
        QByteArray responseBody;
        if (stream) {
            const json delta = {
                {"choices",
                 json::array(
                     {{{"delta", response.at("choices").at(0).at("message")},
                       {"finish_reason", "stop"}}})}};
            responseBody = "data: " + QByteArray::fromStdString(delta.dump())
                           + "\n\ndata: [DONE]\n\n";
        } else {
            responseBody = QByteArray::fromStdString(response.dump());
        }
        QByteArray headers = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
        if (stream)
            headers.replace("application/json", "text/event-stream");
        headers += QByteArray::number(responseBody.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + responseBody);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<json>                     requests_;
    QTcpServer                      server_;
};

class ProbeTool final : public QSocTool
{
public:
    explicit ProbeTool(QString name = QStringLiteral("probe"))
        : name_(std::move(name))
    {}
    QString getName() const override { return name_; }
    QString getDescription() const override
    {
        const auto callback = std::exchange(onDefinition, {});
        if (callback)
            callback();
        return description;
    }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    bool    isReadOnly() const override { return name_ != "bash"; }
    bool    supportsDeferred() const override { return deferred; }
    QString execute(const json &arguments) override
    {
        ++calls;
        observed = arguments;
        if (onExecute)
            onExecute();
        const QPointer<QSocToolCallContext> context = currentCallContext();
        if (deferred && context && context->canDefer()) {
            context->defer();
            QObject::connect(context, &QSocToolCallContext::cancellationRequested, this, [this] {
                ++cancellations;
            });
            QTimer::singleShot(20, context, [context] {
                if (context) {
                    context->reportOutput(QStringLiteral("progress"));
                    context->completeDeferred(QStringLiteral("executed"));
                    context->completeDeferred(QStringLiteral("duplicate"));
                }
            });
            return {};
        }
        return QStringLiteral("executed");
    }
    mutable std::function<void()> onDefinition;
    std::function<void()>         onExecute;
    QString                       description   = QStringLiteral("Dispatch probe");
    bool                          deferred      = false;
    int                           calls         = 0;
    int                           cancellations = 0;
    json                          observed;

private:
    QString name_;
};

json completion(json message)
{
    return {{"choices", json::array({{{"message", std::move(message)}}})}};
}
json call(const char *id, const char *name, const json &arguments)
{
    return completion(
        {{"role", "assistant"},
         {"tool_calls",
          json::array(
              {{{"index", 0},
                {"id", id},
                {"type", "function"},
                {"function", {{"name", name}, {"arguments", arguments.dump()}}}}})}});
}
json done()
{
    return completion({{"role", "assistant"}, {"content", "done"}});
}
QSocAgentConfig config()
{
    QSocAgentConfig result;
    result.verbose             = false;
    result.autoLoadMemory      = false;
    result.memoryRecallEnabled = false;
    result.maxIterations       = 6;
    result.toolPresentation    = "catalog";
    return result;
}
void configureService(QLLMService &service, const QUrl &url)
{
    LLMModelConfig endpoint;
    endpoint.name    = QStringLiteral("dispatch-test");
    endpoint.model   = QStringLiteral("test-model");
    endpoint.url     = url.toString();
    endpoint.timeout = 3000;
    service.setModel(endpoint);
}
QString latestVersion(const json &request)
{
    for (auto it = request.at("messages").rbegin(); it != request.at("messages").rend(); ++it) {
        if (it->value("role", std::string()) == "tool") {
            const auto content = json::parse(it->at("content").get<std::string>(), nullptr, false);
            if (content.is_object() && content.contains("schema_version"))
                return QString::fromStdString(content.at("schema_version").get<std::string>());
        }
    }
    return {};
}
void useCatalog(MockAgentServer &server, const char *name = "probe")
{
    server.respond = [name](const json &request, int index) {
        if (index == 0)
            return call("describe", "tool_catalog", {{"operation", "describe"}, {"name", name}});
        if (index == 1)
            return call(
                "invoke",
                "tool_invoke",
                {{"name", name},
                 {"schema_version", latestVersion(request).toStdString()},
                 {"arguments_json", "{\"command\":\"inspect\"}"}});
        return done();
    };
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void catalogUsesOneDeferredCall()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        tool.deferred = true;
        registry.registerTool(&tool);
        int activeCalls = 0;
        tool.onExecute  = [&] { activeCalls = registry.children().size(); };
        QSocAgent agent(nullptr, &service, &registry, config());
        int       barriers = 0;
        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &) {
            if (point == QSocAgent::PersistencePoint::BeforeTool)
                ++barriers;
            return true;
        });
        QSignalSpy finished(&agent, &QSocAgent::runComplete);
        QSignalSpy started(&agent, &QSocAgent::toolCallStarted);
        QSignalSpy results(&agent, &QSocAgent::toolCallFinished);
        QSignalSpy output(&agent, &QSocAgent::toolCallOutput);
        agent.runStream(QStringLiteral("inspect"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(tool.calls, 1);
        QCOMPARE(barriers, 2);
        QCOMPARE(activeCalls, 1);
        QCOMPARE(started.count(), 2);
        QCOMPARE(results.count(), 2);
        QCOMPARE(output.count(), 1);
        QCOMPARE(started.at(1).at(1).toString(), QStringLiteral("probe"));
        QCOMPARE(results.at(1).at(1).toString(), QStringLiteral("probe"));
        QCOMPARE(server.requestCount(), 3);
    }

    void definitionCallbackCanDestroyAgent()
    {
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        QPointer<QSocAgent> agent = new QSocAgent(nullptr, nullptr, &registry, config());
        tool.onDefinition         = [&agent] { delete agent.data(); };
        const auto definitions    = agent->getEffectiveToolDefinitions();
        QVERIFY(agent.isNull());
        QVERIFY(definitions.empty());
    }

    void finalDefinitionCallbackCanDestroyAgent()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        server.respond = [](const json &, int index) {
            return index == 0 ? call("probe", "probe", json::object()) : done();
        };
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        QPointer<QSocAgent> agent = new QSocAgent(nullptr, &service, &registry, config());
        connect(
            agent,
            &QSocAgent::toolCallStarted,
            &tool,
            [&agent, &tool](const QString &, const QString &, const QString &) {
                tool.onDefinition = [&agent] { delete agent.data(); };
            });
        agent->run(QStringLiteral("inspect"));
        QVERIFY(agent.isNull());
        QCOMPARE(tool.calls, 0);
    }

    void presentationSurvivesSessionRoundtrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString          path = directory.filePath("session.jsonl");
        QSocSession            session("presentation-session", path);
        QSocSession::RunRecord record;
        record.runId            = "presentation-run";
        record.event            = QSocSession::RunEvent::Started;
        record.input            = "inspect";
        record.messageCount     = 0;
        record.historyDigest    = QSocSession::historyDigest(json::array());
        record.contextPresent   = true;
        record.modelId          = "test-model";
        record.effortLevel      = "high";
        record.toolPresentation = "catalog";
        record.projectRoot      = directory.path();
        record.workingDir       = directory.path();
        QVERIFY(session.appendRun(record));
        const auto loaded = QSocSession::latestRun(path);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->toolPresentation, QStringLiteral("catalog"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto lines = file.readAll().split('\n');
        file.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        for (const auto &line : lines) {
            if (line.trimmed().isEmpty())
                continue;
            auto legacy = json::parse(line.toStdString());
            if (legacy.contains("context"))
                legacy["context"].erase("tool_presentation");
            file.write(QByteArray::fromStdString(legacy.dump()) + '\n');
        }
        file.close();
        const auto legacy = QSocSession::latestRun(path);
        QVERIFY(legacy.has_value());
        QCOMPARE(legacy->toolPresentation, QStringLiteral("direct"));
    }

    void registryChangeAtBarrier()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        QSocToolRegistry replacement;
        ProbeTool        originalTool;
        ProbeTool        replacementTool;
        registry.registerTool(&originalTool);
        replacement.registerTool(&replacementTool);
        QSocAgent agent(nullptr, &service, &registry, config());
        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &id) {
            if (point == QSocAgent::PersistencePoint::BeforeTool && id == "invoke")
                agent.setToolRegistry(&replacement);
            return true;
        });
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(originalTool.calls, 0);
        QCOMPARE(replacementTool.calls, 0);
    }

    void catalogCompactionUsesPresentedSchemas()
    {
        QSocToolRegistry registry;
        ProbeTool        specialized(QStringLiteral("specialized_probe"));
        specialized.description = QString(600000, QLatin1Char('x'));
        registry.registerTool(&specialized);
        auto cfg                 = config();
        cfg.systemPromptOverride = QStringLiteral("Follow the task.");
        cfg.maxContextTokens     = 32768;
        cfg.keepRecentMessages   = 2;
        QSocAgent agent(nullptr, nullptr, &registry, cfg);
        json      history = json::array();
        for (int index = 0; index < 12; ++index)
            history.push_back(
                {{"role", index % 2 ? "assistant" : "user"},
                 {"content", std::string(4000, static_cast<char>('a' + index))}});
        agent.setMessages(history);
        bool committed = false;
        agent.setCompactionCommitter([&](const QSocAgent::CompactionCandidate &candidate) {
            committed = candidate.beforeTokens < cfg.maxContextTokens
                        && candidate.afterTokens < candidate.beforeTokens;
            return true;
        });
        QVERIFY(agent.compact() > 0);
        QVERIFY(committed);
    }

    void artifactBindingChangeInvalidatesCatalog()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        QSocAgent     agent(nullptr, &service, &registry, config());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        bool rebound = false;
        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &id) {
            if (point == QSocAgent::PersistencePoint::BeforeTool && id == "invoke")
                rebound = agent.bindToolResultStore(directory.path(), "replacement");
            return true;
        });
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QVERIFY(rebound);
        QCOMPARE(tool.calls, 0);
        QVERIFY(QString::fromStdString(server.request(2).dump()).contains("changed"));
    }

    void invokedArtifactReader_data()
    {
        QTest::addColumn<bool>("stream");
        QTest::newRow("sync") << false;
        QTest::newRow("stream") << true;
    }

    void invokedArtifactReader()
    {
        QFETCH(bool, stream);
        MockAgentServer server;
        QVERIFY(server.listen());
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry   registry;
        QSocToolOutputRead reader(nullptr);
        ProbeTool          specialized(QStringLiteral("specialized_probe"));
        specialized.description = QString(600000, QLatin1Char('x'));
        registry.registerTool(&reader);
        registry.registerTool(&specialized);
        QSocAgent     agent(nullptr, &service, &registry, config());
        const QString text  = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen())
                              + QStringLiteral("literal text")
                              + QString::fromLatin1(QSocToolWebFetch::attachmentMarkerClose())
                              + QString(32000, QLatin1Char('x'));
        const auto    saved = agent.toolResultStore()->publish(text, "ok");
        QVERIFY(saved.has_value());
        const auto stored = agent.toolResultStore()->storedBytes();
        server.respond    = [&](const json &request, int index) {
            if (index == 0)
                return call(
                    "describe",
                    "tool_catalog",
                    {{"operation", "describe"}, {"name", "tool_output_read"}});
            if (index == 1)
                return call(
                    "read",
                    "tool_invoke",
                    {{"name", "tool_output_read"},
                     {"schema_version", latestVersion(request).toStdString()},
                     {"arguments_json", json({{"artifact_id", saved->id.toStdString()}}).dump()}});
            return done();
        };
        QSignalSpy finished(&agent, &QSocAgent::runComplete);
        QSignalSpy results(&agent, &QSocAgent::toolCallFinished);
        if (stream) {
            agent.runStream(QStringLiteral("read"));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        } else {
            QCOMPARE(agent.run(QStringLiteral("read")), QStringLiteral("done"));
        }
        QCOMPARE(server.requestCount(), 3);
        QCOMPARE(results.at(1).at(1).toString(), QStringLiteral("tool_output_read"));
        const auto &wire = server.request(2);
        QCOMPARE(wire.at("tools"), agent.getEffectiveToolDefinitions());
        QCOMPARE(wire.at("tools").size(), json::size_type(3));
        const auto &result = wire.at("messages").back();
        QCOMPARE(result.at("tool_call_id"), json("read"));
        const auto    page     = json::parse(result.at("content").get<std::string>());
        const QString captured = QString::fromStdString(page.at("text").get<std::string>());
        QVERIFY(!captured.isEmpty());
        QVERIFY(text.startsWith(captured));
        QVERIFY(captured.contains(QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen())));
        QCOMPARE(agent.toolResultStore()->storedBytes(), stored);
        QVERIFY(QSocAgent::artifactReferences(agent.getMessages()).isEmpty());
    }

    void cancellationReachesOneDeferredContext()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        tool.deferred = true;
        registry.registerTool(&tool);
        QSocAgent agent(nullptr, &service, &registry, config());
        connect(
            &agent,
            &QSocAgent::toolCallStarted,
            &agent,
            [&agent](const QString &, const QString &name, const QString &) {
                if (name == "probe")
                    QTimer::singleShot(0, &agent, &QSocAgent::abort);
            });
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy output(&agent, &QSocAgent::toolCallOutput);
        agent.runStream(QStringLiteral("inspect"));
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 5000);
        QCOMPARE(tool.calls, 1);
        QCOMPARE(tool.cancellations, 1);
        QCOMPARE(output.count(), 0);
    }

    void permissionsChangeAtBarrier()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        QSocAgent agent(nullptr, &service, &registry, config());
        agent.setPersistenceBarrier([&](QSocAgent::PersistencePoint point, const QString &id) {
            if (point == QSocAgent::PersistencePoint::BeforeTool && id == "invoke") {
                auto updated = agent.getConfig();
                updated.toolsDeny.append("probe");
                agent.setConfig(updated);
            }
            return true;
        });
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(tool.calls, 0);
        QVERIFY(QString::fromStdString(server.request(2).dump()).contains("changed"));
    }

    void deferredPermissionChangeDiscardsReturn()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        tool.deferred = true;
        registry.registerTool(&tool);
        QSocAgent agent(nullptr, &service, &registry, config());
        tool.onExecute = [&agent] {
            QTimer::singleShot(0, &agent, [&agent] {
                auto updated = agent.getConfig();
                updated.toolsDeny.append("probe");
                agent.setConfig(updated);
            });
        };
        QSignalSpy aborted(&agent, &QSocAgent::runAborted);
        QSignalSpy results(&agent, &QSocAgent::toolCallFinished);
        agent.runStream(QStringLiteral("inspect"));
        QTRY_COMPARE_WITH_TIMEOUT(aborted.count(), 1, 5000);
        QCOMPARE(tool.calls, 1);
        QCOMPARE(results.count(), 2);
        QCOMPARE(results.at(1).at(3).value<QSocToolResultStatus>(), QSocToolResultStatus::Uncertain);
        QVERIFY(!results.at(1).at(2).toString().contains("executed"));
        QVERIFY(results.at(1).at(2).toString().contains("discarded"));
        QCOMPARE(server.requestCount(), 2);
    }

    void copiedJudgeUsesCallerModelAndEffort()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        server.respond = [](const json &, int index) {
            if (index == 0)
                return call("shell", "bash", {{"command", "inspect"}});
            if (index == 1)
                return completion(
                    {{"role", "assistant"},
                     {"content", "{\"readOnly\":true,\"reason\":\"inspection\"}"}});
            return done();
        };
        QLLMService parentService;
        configureService(parentService, server.url());
        QLLMService childService;
        configureService(childService, server.url());
        auto endpoint  = childService.getCurrentModelConfig();
        endpoint.model = "child-model";
        childService.setModel(endpoint);
        QSocToolRegistry registry;
        ProbeTool        tool(QStringLiteral("bash"));
        registry.registerTool(&tool);
        auto parentConfig        = config();
        parentConfig.planMode    = true;
        parentConfig.effortLevel = "low";
        QSocAgent parent(nullptr, &parentService, &registry, parentConfig);
        parent.setContextualBashSafetyJudge(QSocAgent::classifyBashCommand);
        auto childConfig        = parentConfig;
        childConfig.effortLevel = "high";
        childConfig.isSubAgent  = true;
        QSocAgent child(nullptr, &childService, &registry, childConfig);
        child.setContextualBashSafetyJudge(parent.contextualBashSafetyJudge());
        QCOMPARE(child.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(tool.calls, 1);
        QCOMPARE(server.requestCount(), 3);
        QCOMPARE(server.request(1).at("model").get<std::string>(), std::string("child-model"));
        QCOMPARE(server.request(1).at("reasoning_effort").get<std::string>(), std::string("high"));
        std::stop_source stopped;
        stopped.request_stop();
        const QSocBashSafetyContext context{&childService, endpoint, "high", stopped.get_token()};
        QVERIFY(!QSocAgent::classifyBashCommand("inspect", context).readOnly);
        QCOMPARE(server.requestCount(), 3);
    }

    void malformedJudgeCannotApprove()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        server.respond = [](const json &, int) {
            return completion(
                {{"role", "assistant"}, {"content", "{\"readOnly\":true,\"reason\":42}"}});
        };
        QLLMService service;
        configureService(service, server.url());
        const QSocBashSafetyContext context{&service, service.getCurrentModelConfig(), "high", {}};
        QVERIFY(!QSocAgent::classifyBashCommand("inspect", context).readOnly);
        QCOMPARE(server.requestCount(), 1);
    }

    void catalogNeverListsDeniedTools()
    {
        MockAgentServer server;
        QVERIFY(server.listen());
        server.respond = [](const json &, int index) {
            return index == 0
                       ? call("search", "tool_catalog", {{"operation", "search"}, {"query", "probe"}})
                       : done();
        };
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        auto cfg = config();
        cfg.toolsDeny.append("probe");
        QSocAgent agent(nullptr, &service, &registry, cfg);
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        const auto messages = agent.getMessages();
        bool       found    = false;
        for (const auto &message : messages) {
            if (message.value("role", std::string()) != "tool")
                continue;
            const auto response = json::parse(message.at("content").get<std::string>());
            QVERIFY(response.at("tools").empty());
            found = true;
        }
        QVERIFY(found);
        QCOMPARE(tool.calls, 0);
    }

    void preToolSideEffectBeforeBarrierPreventsReplay()
    {
#ifdef Q_OS_WIN
        QSKIP("requires a POSIX hook shell");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString logPath    = directory.filePath("effect.log");
        QString       quotedPath = logPath;
        quotedPath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        quotedPath = QLatin1Char('\'') + quotedPath + QLatin1Char('\'');
        QSocHookConfig    hooksConfig;
        HookMatcherConfig matcher;
        matcher.matcher = "probe";
        HookCommandConfig command;
        command.command = QStringLiteral("printf 'effect\\n' >> %1").arg(quotedPath);
        matcher.commands.append(command);
        hooksConfig.byEvent[QSocHookEvent::PreToolUse].append(matcher);
        QSocHookManager hooks;
        hooks.setConfig(hooksConfig);
        MockAgentServer server;
        QVERIFY(server.listen());
        server.respond = [](const json &, int) { return call("probe", "probe", json::object()); };
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        auto cfg  = config();
        cfg.hooks = hooksConfig;
        QSocAgent agent(nullptr, &service, &registry, cfg);
        agent.setHookManager(&hooks);
        agent.setPersistenceBarrier([](QSocAgent::PersistencePoint point, const QString &) {
            return point != QSocAgent::PersistencePoint::BeforeTool;
        });
        const QString          sessionPath = directory.filePath("session.jsonl");
        QSocSession            session("hook-session", sessionPath);
        QSocSession::RunRecord record;
        record.runId           = "hook-run";
        record.event           = QSocSession::RunEvent::Started;
        record.input           = "inspect";
        record.messageCount    = 0;
        record.historyDigest   = QSocSession::historyDigest(json::array());
        record.inputReplaySafe = true;
        record.contextPresent  = true;
        record.modelId         = "test-model";
        record.projectRoot     = directory.path();
        record.workingDir      = directory.path();
        QVERIFY(session.appendRun(record));
        agent.run(QStringLiteral("inspect"));
        QCOMPARE(tool.calls, 0);
        const auto recovered = QSocSession::latestRun(sessionPath);
        QVERIFY(recovered.has_value());
        QVERIFY(recovered->startedToolCallIds.isEmpty());
        auto plan = QSocSessionRecovery::makePlan(
            recovered, QSocSession::loadMessages(sessionPath), record);
        QCOMPARE(plan.action, QSocSessionRecovery::Action::ReplayInput);
        QSocSessionRecovery::guardHookReplay(plan, hooksConfig);
        if (plan.action == QSocSessionRecovery::Action::ReplayInput)
            agent.run(plan.input);
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll(), QByteArray("effect\n"));
        QCOMPARE(plan.action, QSocSessionRecovery::Action::Wait);
        QVERIFY(plan.requiresUserInput);
        QVERIFY(plan.reason.contains("uncertain"));
    }

    void catalogHooksUseCanonicalNameOnce()
    {
#ifdef Q_OS_WIN
        QSKIP("requires a POSIX hook shell");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString logPath    = directory.filePath("hooks.log");
        QString quotedPath = logPath;
        quotedPath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        quotedPath = QLatin1Char('\'') + quotedPath + QLatin1Char('\'');
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server);
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool;
        registry.registerTool(&tool);
        QSocAgent      agent(nullptr, &service, &registry, config());
        QSocHookConfig hooksConfig;
        for (const auto event : {QSocHookEvent::PreToolUse, QSocHookEvent::PostToolUse}) {
            HookMatcherConfig matcher;
            matcher.matcher = "probe";
            HookCommandConfig command;
            command.command
                = QStringLiteral("printf '%1\\n' >> %2")
                      .arg(event == QSocHookEvent::PreToolUse ? "pre" : "post", quotedPath);
            matcher.commands.append(command);
            hooksConfig.byEvent[event].append(matcher);
        }
        QSocHookManager hooks;
        hooks.setConfig(hooksConfig);
        agent.setHookManager(&hooks);
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(tool.calls, 1);
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll(), QByteArray("pre\npost\n"));
    }

    void hookFinalCommandIsJudged()
    {
#ifdef Q_OS_WIN
        QSKIP("requires a POSIX hook shell");
#endif
        MockAgentServer server;
        QVERIFY(server.listen());
        useCatalog(server, "bash");
        QLLMService service;
        configureService(service, server.url());
        QSocToolRegistry registry;
        ProbeTool        tool(QStringLiteral("bash"));
        registry.registerTool(&tool);
        auto cfg     = config();
        cfg.planMode = true;
        QSocAgent         agent(nullptr, &service, &registry, cfg);
        QSocHookConfig    hooksConfig;
        HookMatcherConfig matcher;
        matcher.matcher = "bash";
        HookCommandConfig command;
        command.command = QStringLiteral(
            "printf '%s' '{\"updatedInput\":{\"command\":\"modify\"}}'");
        matcher.commands.append(command);
        hooksConfig.byEvent[QSocHookEvent::PreToolUse].append(matcher);
        QSocHookManager hooks;
        hooks.setConfig(hooksConfig);
        agent.setHookManager(&hooks);
        QString judged;
        agent.setBashSafetyJudge([&](const QString &value) {
            judged = value;
            return QSocBashSafety{value == "inspect", QStringLiteral("writes state")};
        });
        QCOMPARE(agent.run(QStringLiteral("inspect")), QStringLiteral("done"));
        QCOMPARE(judged, QStringLiteral("modify"));
        QCOMPARE(tool.calls, 0);
    }
};
} // namespace
QSOC_TEST_MAIN(Test)
#include "test_qsoctooldispatch.moc"
