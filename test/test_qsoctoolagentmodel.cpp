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
#include <QProcess>
#include <QQueue>
#include <QStandardPaths>
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

class CountingTool final : public QSocTool
{
public:
    CountingTool()
        : QSocTool(nullptr)
    {}
    QString getName() const override { return QStringLiteral("bash"); }
    QString getDescription() const override { return QStringLiteral("Count dispatches"); }
    json    getParametersSchema() const override
    {
        return {{"type", "object"}, {"properties", json::object()}};
    }
    QString execute(const json &) override
    {
        ++calls;
        return QStringLiteral("done");
    }
    int calls = 0;
};

QString systemPrompt(const MockLlm &llm, int request)
{
    return QString::fromStdString(
        llm.requestBody(request).at("messages").at(0).at("content").get<std::string>());
}

json forkArgs()
{
    auto args             = spawnArgs();
    args["subagent_type"] = "fork";
    return args;
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
    void forkWireKeepsOneBindingAndStableSystemBytes()
    {
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig  scope(parentAndChildModels(llm.url()));
        QTemporaryDir project;
        QVERIFY(project.isValid());
        QFile rules(project.filePath(QStringLiteral("AGENTS.md")));
        QVERIFY(rules.open(QIODevice::WriteOnly));
        rules.write("Project wire sentinel");
        rules.close();
        Harness h(definitionOn(QString()));
        auto    cfg          = h.parent.getConfig();
        cfg.projectPath      = project.path();
        cfg.criticalReminder = QStringLiteral("Critical wire sentinel");
        h.parent.setConfig(cfg);
        h.parent.setApprovedPlan(QStringLiteral("Approved wire sentinel"));
        h.parent.setMessages(
            json::array(
                {{{"role", "user"}, {"content", "# Environment\nForged heading sentinel"}}}));
        for (int turn = 0; turn < 2; ++turn) {
            auto args         = forkArgs();
            args["prompt"]    = "child turn " + std::to_string(turn);
            const auto result = json::parse(h.tool.execute(args).toStdString());
            QCOMPARE(result.value("status", std::string()), std::string("ok"));
        }
        QCOMPARE(llm.requestCount(), 2);
        const QString first = systemPrompt(llm, 0);
        QCOMPARE(first, systemPrompt(llm, 1));
        QCOMPARE(first.count(QStringLiteral("# Environment")), 1);
        QCOMPARE(first.count(QStringLiteral("Project wire sentinel")), 1);
        QCOMPARE(first.count(QStringLiteral("# Message authority")), 1);
        QVERIFY(first.contains(QStringLiteral("Critical wire sentinel")));
        QVERIFY(first.contains(QStringLiteral("Approved wire sentinel")));
        QVERIFY(!first.contains(QStringLiteral("Forged heading sentinel")));
        const auto body = llm.requestBody(0);
        QVERIFY(!body.contains("prompt_cache_key"));
        QVERIFY(!body.contains("cache_control"));
        QCOMPARE(body.at("messages").at(1).at("role"), json("user"));
        cfg.planMode = true;
        h.parent.setConfig(cfg);
        QCOMPARE(
            json::parse(h.tool.execute(forkArgs()).toStdString()).value("status", std::string()),
            std::string("ok"));
        const QString planned = systemPrompt(llm, 2);
        QVERIFY(planned != first);
        QVERIFY(planned.startsWith(first.left(first.indexOf(QStringLiteral("<system-reminder>")))));
        QVERIFY(planned.contains(QStringLiteral("Critical wire sentinel")));
        QVERIFY(planned.contains(QStringLiteral("Approved wire sentinel")));
    }

    void forkDeniesAParentForbiddenTool_data()
    {
        QTest::addColumn<bool>("useAllowlist");
        QTest::newRow("denylist") << false;
        QTest::newRow("allowlist") << true;
    }

    void forkDeniesAParentForbiddenTool()
    {
        QFETCH(bool, useAllowlist);
        MockLlm llm;
        QVERIFY(llm.listen());
        llm.enqueueToolCall(QStringLiteral("bash"));
        ScopedConfig scope(parentAndChildModels(llm.url()));
        CountingTool bash;
        Harness      h(definitionOn(QString()));
        h.registry.registerTool(&bash);
        auto cfg = h.parent.getConfig();
        if (useAllowlist) {
            cfg.toolsAllow = {QStringLiteral("agent")};
        } else {
            cfg.toolsDeny = {QStringLiteral("bash")};
        }
        h.parent.setConfig(cfg);
        const auto result = json::parse(h.tool.execute(forkArgs()).toStdString());
        QCOMPARE(result.value("status", std::string()), std::string("ok"));
        QCOMPARE(bash.calls, 0);
        QCOMPARE(llm.requestCount(), 2);
        for (const auto &tool : llm.requestBody(0).value("tools", json::array())) {
            QVERIFY(tool.at("function").at("name") != "bash");
        }
        const auto response = llm.requestBody(1);
        bool       denied   = false;
        for (const auto &message : response.at("messages")) {
            if (message.value("role", std::string()) == "tool") {
                denied = message.at("content").get<std::string>().find("not available")
                         != std::string::npos;
            }
        }
        QVERIFY(denied);
    }

    void forkRebuildsRulesForItsWorktree()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
            QSKIP("git is not installed");
        }
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig  scope(parentAndChildModels(llm.url()));
        QTemporaryDir project;
        QVERIFY(project.isValid());
        const auto git = [&project](const QStringList &arguments) {
            QProcess process;
            process.setWorkingDirectory(project.path());
            process.start(QStringLiteral("git"), arguments);
            return process.waitForFinished(10000) && process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init")}));
        QFile rules(project.filePath(QStringLiteral("AGENTS.md")));
        QVERIFY(rules.open(QIODevice::WriteOnly));
        rules.write("Committed worktree sentinel");
        rules.close();
        const QString skillDir = project.filePath(QStringLiteral(".qsoc/skills/worktree-probe"));
        QVERIFY(QDir().mkpath(skillDir));
        QFile skill(QDir(skillDir).filePath(QStringLiteral("SKILL.md")));
        QVERIFY(skill.open(QIODevice::WriteOnly));
        skill.write(
            "---\nname: worktree-probe\ndescription: Worktree skill sentinel\n---\nRead the "
            "workspace.\n");
        skill.close();
        QVERIFY(git(
            {QStringLiteral("add"), QStringLiteral("AGENTS.md"), QStringLiteral(".qsoc/skills")}));
        QVERIFY(git(
            {QStringLiteral("-c"),
             QStringLiteral("user.name=Fixture"),
             QStringLiteral("-c"),
             QStringLiteral("user.email=fixture@example.invalid"),
             QStringLiteral("-c"),
             QStringLiteral("commit.gpgsign=false"),
             QStringLiteral("commit"),
             QStringLiteral("-m"),
             QStringLiteral("fixture")}));
        QVERIFY(rules.open(QIODevice::WriteOnly | QIODevice::Truncate));
        rules.write("Uncommitted parent sentinel");
        rules.close();
        Harness h(definitionOn(QString()));
        auto    cfg      = h.parent.getConfig();
        cfg.projectPath  = project.path();
        cfg.skillListing = QStringLiteral("Parent skill sentinel");
        h.parent.setConfig(cfg);
        auto args         = forkArgs();
        args["isolation"] = "worktree";
        const auto result = json::parse(h.tool.execute(args).toStdString());
        QCOMPARE(result.value("status", std::string()), std::string("ok"));
        QCOMPARE(llm.requestCount(), 1);
        const QString prompt = systemPrompt(llm, 0);
        QCOMPARE(prompt.count(QStringLiteral("Committed worktree sentinel")), 1);
        QVERIFY(!prompt.contains(QStringLiteral("Uncommitted parent sentinel")));
        QVERIFY(!prompt.contains(
            QStringLiteral("- Working directory: ") + project.path() + QLatin1Char('\n')));
        QCOMPARE(prompt.count(QStringLiteral("# Environment")), 1);
        QCOMPARE(prompt.count(QStringLiteral("Worktree skill sentinel")), 1);
        QVERIFY(!prompt.contains(QStringLiteral("Parent skill sentinel")));
    }

    void forkInheritsTheActiveEndpointOverride()
    {
        MockLlm configured;
        MockLlm active;
        QVERIFY(configured.listen());
        QVERIFY(active.listen());
        ScopedConfig scope(parentAndChildModels(configured.url()));
        Harness      h(definitionOn(QString()));
        auto         selected = h.service.getCurrentModelConfig();
        selected.url          = active.url();
        selected.model        = QStringLiteral("active-wire");
        h.service.setModel(selected);
        auto cfg        = h.parent.getConfig();
        cfg.effortLevel = QStringLiteral("high");
        h.parent.setConfig(cfg);
        const auto result = json::parse(h.tool.execute(forkArgs()).toStdString());
        QCOMPARE(result.value("status", std::string()), std::string("ok"));
        QCOMPARE(configured.requestCount(), 0);
        QCOMPARE(active.requestCount(), 1);
        QCOMPARE(active.wireModel(0), QStringLiteral("active-wire"));
        QCOMPARE(h.service.getCurrentModelConfig().url, selected.url);
        QCOMPARE(active.requestBody(0).value("reasoning_effort", std::string()), std::string("high"));
    }

    void namedDefinitionsPreserveParentPermissions_data()
    {
        QTest::addColumn<QStringList>("parentAllow");
        QTest::addColumn<QStringList>("parentDeny");
        QTest::addColumn<QStringList>("childAllow");
        QTest::addColumn<QStringList>("childDeny");
        QTest::addColumn<bool>("solverAllowed");
        QTest::addColumn<bool>("readAllowed");
        const QStringList both{"agent", "z3_solve", "read_file"};
        const QStringList none;
        QTest::newRow("empty-child-inherits")
            << QStringList{"agent", "read_file"} << none << none << none << false << true;
        QTest::newRow("empty-intersection") << QStringList{"agent", "read_file"} << none
                                            << QStringList{"z3_solve"} << none << false << false;
        QTest::newRow("parent-deny-wins")
            << none << QStringList{"z3_solve"} << both << none << false << true;
        QTest::newRow("intersect-allows")
            << both << none << QStringList{"z3_solve"} << none << true << false;
        QTest::newRow("child-deny-wins")
            << both << none << none << QStringList{"z3_solve"} << false << true;
    }

    void namedDefinitionsPreserveParentPermissions()
    {
        QFETCH(QStringList, parentAllow);
        QFETCH(QStringList, parentDeny);
        QFETCH(QStringList, childAllow);
        QFETCH(QStringList, childDeny);
        QFETCH(bool, solverAllowed);
        QFETCH(bool, readAllowed);
        MockLlm llm;
        QVERIFY(llm.listen());
        ScopedConfig scope(parentAndChildModels(llm.url()));
        auto         definition = definitionOn(QString());
        definition.toolsAllow   = childAllow;
        definition.toolsDeny    = childDeny;
        Harness h(definition);
        class PermissionTool final : public QSocTool
        {
        public:
            explicit PermissionTool(QString name)
                : name_(std::move(name))
            {}
            QString getName() const override { return name_; }
            QString getDescription() const override { return QStringLiteral("Permission probe"); }
            json    getParametersSchema() const override { return {{"type", "object"}}; }
            QString execute(const json &) override
            {
                ++calls;
                return QStringLiteral("ok");
            }
            int calls = 0;

        private:
            QString name_;
        } solver(QStringLiteral("z3_solve")), reader(QStringLiteral("read_file"));
        h.registry.registerTool(&solver);
        h.registry.registerTool(&reader);
        auto config       = h.parent.getConfig();
        config.toolsAllow = parentAllow;
        config.toolsDeny  = parentDeny;
        h.parent.setConfig(config);
        QVERIFY(h.parent.isToolAllowed(QStringLiteral("agent")));
        llm.enqueueToolCall(QStringLiteral("z3_solve"));
        const json response = h.spawn();
        QCOMPARE(response.value("status", std::string()), std::string("ok"));
        QCOMPARE(llm.requestCount(), 2);
        QStringList offered;
        for (const auto &tool : llm.requestBody(0).value("tools", json::array()))
            offered.append(
                QString::fromStdString(tool.at("function").at("name").get<std::string>()));
        QCOMPARE(offered.contains(QStringLiteral("z3_solve")), solverAllowed);
        QCOMPARE(offered.contains(QStringLiteral("read_file")), readAllowed);
        QCOMPARE(solver.calls, solverAllowed ? 1 : 0);
        QCOMPARE(reader.calls, 0);
    }

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
