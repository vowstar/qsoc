// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocdispatchpolicy.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "common/qsocinterrupt.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QProcess>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <pwd.h>
#include <unistd.h>
#endif
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

/*
 * `spawn-agent` with a `host` argument binds the child's tools to that host.
 * These cases hold the rest of the child to the same binding: the workspace it
 * is told about, and the host its workspace-health answers describe. A child
 * configured for the parent's host while its tools reach another one is told
 * the wrong absolute paths and can be stopped by a host it never touches.
 *
 * The far side is a real loopback sshd, because the mismatch only exists once
 * the binding does. The parent's host is a config-only fixture: no session is
 * opened for it, since nothing here has to reach it, only to not confuse it
 * with the host that answers.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing the lot.
 */

namespace {

/* The alias the fixture's ~/.ssh/config and host catalog both define, so the
 * spawn tool resolves host, port, user and key as it does for a real target. */
constexpr auto kAlias = "dispatchhost";

/* A catalog-only name whose target is kAlias, so the alias the call names and
 * the SSH target behind it differ. */
constexpr auto kCatalogAlias = "dispatchentry";

/* The parent's binding. Never connected; only its strings matter. */
constexpr auto kParentWorkspace = "/parent-host-workspace";
constexpr auto kParentTarget    = "operator@parent-host:22";

/* What the parent's probe says about the parent's host. */
constexpr auto kParentUnhealthy = "the parent host workspace stopped answering";

/* Minimal chat-completions endpoint: hands a child a queued tool call and a
 * final answer, and keeps every request body for inspection. */
class MockLlm final : public QObject
{
public:
    explicit MockLlm(QObject *parent = nullptr)
        : QObject(parent)
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

    QUrl url() const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(server_.serverAddress().toString());
        result.setPort(server_.serverPort());
        result.setPath(QStringLiteral("/chat/completions"));
        return result;
    }

    void enqueueToolCall(const QString &name)
    {
        const json chunk = {
            {"choices",
             json::array(
                 {{{"delta",
                    {{"tool_calls",
                      json::array(
                          {{{"index", 0},
                            {"id", "call_0"},
                            {"type", "function"},
                            {"function", {{"name", name.toStdString()}, {"arguments", "{}"}}}}})}}},
                   {"finish_reason", "tool_calls"}}})}};
        enqueueEventStream(chunk);
    }

    void enqueueFinal(const QString &text)
    {
        const json contentChunk = {
            {"choices", json::array({{{"delta", {{"content", text.toStdString()}}}}})}};
        const json finishChunk = {
            {"choices", json::array({{{"delta", json::object()}, {"finish_reason", "stop"}}})}};
        responses_.enqueue(
            QByteArrayLiteral("data: ") + QByteArray::fromStdString(contentChunk.dump())
            + QByteArrayLiteral("\n\ndata: ") + QByteArray::fromStdString(finishChunk.dump())
            + QByteArrayLiteral("\n\ndata: [DONE]\n\n"));
    }

    std::function<void(int)> onRequest;

    int        requestCount() const { return requestCount_; }
    QByteArray requestBody(int index) const { return bodies_.value(index); }

private:
    void enqueueEventStream(const json &chunk)
    {
        responses_.enqueue(
            QByteArrayLiteral("data: ") + QByteArray::fromStdString(chunk.dump())
            + QByteArrayLiteral("\n\ndata: [DONE]\n\n"));
    }

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
        ++requestCount_;
        if (onRequest) {
            onRequest(requestCount_ - 1);
        }
        if (responses_.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }
        const QByteArray body    = responses_.dequeue();
        QByteArray       headers = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ");
        headers += QByteArray::number(body.size());
        headers += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
        socket->write(headers + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers_;
    QQueue<QByteArray>              responses_;
    QList<QByteArray>               bodies_;
    QTcpServer                      server_;
    int                             requestCount_ = 0;
};

/* The system message of one recorded request: what the child was told about
 * where it works. */
QString systemPromptOf(const MockLlm &llm, int requestIndex)
{
    const json payload
        = json::parse(llm.requestBody(requestIndex).toStdString(), nullptr, /*throw=*/false);
    if (!payload.is_object() || !payload.contains("messages") || !payload["messages"].is_array()) {
        return {};
    }
    for (const auto &message : payload["messages"]) {
        if (message.value("role", std::string()) == "system" && message.contains("content")
            && message["content"].is_string()) {
            return QString::fromStdString(message["content"].get<std::string>());
        }
    }
    return {};
}

/* The line of a system prompt that names the remote workspace, so a failing
 * assertion reports what the child was actually told. */
QString remoteWorkspaceLine(const QString &prompt)
{
    const QStringList lines = prompt.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("- Remote workspace: "))) {
            return line.trimmed();
        }
    }
    return QStringLiteral("(the prompt names no remote workspace)");
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aDispatchedChildIsToldTheWorkspaceItsToolsReach();
    void forkLoadsRemoteRulesAtTheBindingBoundary();
    void aDispatchedChildIsNotStoppedByTheParentHostHealth();
    void worktreeIsolationIsRefusedOnRemoteWorkspaces();
    void aResumedRunReturnsToItsAliasAndWorkspace();
    void aGrantedSshConfigAliasDispatchesWithAWorkspace();
    void aNamedWorkspaceMustBeAProjectDirectory();

private:
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    /* Written per case: the endpoint port is only known after listen(). */
    bool writeLlmConfig(const MockLlm &llm)
    {
        QFile file(m_home + QStringLiteral("/qsoc.yml"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray yaml = QByteArrayLiteral(
                                    "llm:\n"
                                    "  model: test-model\n"
                                    "  models:\n"
                                    "    test-model:\n"
                                    "      url: ")
                                + llm.url().toString().toUtf8()
                                + QByteArrayLiteral("\n      timeout: 10000\n");
        return file.write(yaml) == yaml.size();
    }

    /* The catalog entry the `host` argument resolves through. The target is
     * the alias itself, so the connect path reads the fixture's ssh config. */
    bool registerHostB(QSocHostCatalog *catalog) const
    {
        catalog->load(QString(), m_project);
        QSocHostProfile profile;
        profile.alias      = QString::fromLatin1(kAlias);
        profile.workspace  = m_workspace;
        profile.capability = QStringLiteral("loopback dispatch target");
        profile.target     = QString::fromLatin1(kAlias);
        return catalog->upsert(profile, /*allowOverwrite=*/true);
    }

    /* A parent bound to a host that is not the dispatch target. */
    static QSocAgentConfig parentOnItsOwnHost()
    {
        QSocAgentConfig config;
        config.verbose             = false;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxIterations       = 4;
        config.maxRetries          = 1;
        config.autoBackgroundMs    = 0;
        config.remoteMode          = true;
        config.remoteName          = QString::fromLatin1(kParentTarget);
        config.remoteDisplay       = QString::fromLatin1(kParentTarget) + QStringLiteral(":")
                                     + QString::fromLatin1(kParentWorkspace);
        config.remoteWorkspace     = QString::fromLatin1(kParentWorkspace);
        config.remoteWorkingDir    = QString::fromLatin1(kParentWorkspace);
        config.remoteWritableDirs  = {QString::fromLatin1(kParentWorkspace)};
        return config;
    }

    static json spawnArgs()
    {
        return json{
            {"subagent_type", "general-purpose"},
            {"description", "dispatch to the loopback host"},
            {"prompt", "report the workspace you are working in"},
            {"host", kAlias},
            {"run_in_background", false}};
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    bool          m_ready = false;
    QString       m_failure;
    QString       m_home;
    QString       m_project;
    QString       m_workspace;
    QByteArray    m_oldHome;
    QByteArray    m_oldQsocHome;
    QByteArray    m_oldXdgHome;
    bool          m_hadHome     = false;
    bool          m_hadQsocHome = false;
    bool          m_hadXdgHome  = false;
};

/* First line of every case: the sshd fixture's own three-state policy, then
 * the wiring this test builds on top of it. */
#define REQUIRE_DISPATCH_FIXTURE() \
    do { \
        QSOC_REQUIRE_SSHD(m_fixture); \
        if (!m_ready) { \
            QSOC_TEST_FIXTURE_FAILED(m_failure); \
        } \
    } while (false)

void Test::initTestCase()
{
    m_fixture.start();
    if (m_fixture.state() != QSocTestSshd::State::Ready) {
        return; /* the fixture's own state decides skip versus fail */
    }
    QVERIFY(QSocInterrupt::installBridge());
    m_ready = prepare();
}

bool Test::prepare()
{
    if (!m_dir.isValid()) {
        return fail(QStringLiteral("temporary directory: %1").arg(m_dir.errorString()));
    }
    const QString root = m_dir.path();
    m_home             = root + QStringLiteral("/home");
    m_project          = root + QStringLiteral("/project");
    m_workspace        = m_fixture.workDir() + QStringLiteral("/dispatched");
    if (!QDir().mkpath(m_home + QStringLiteral("/.ssh")) || !QDir().mkpath(m_project)
        || !QDir().mkpath(m_workspace)) {
        return fail(QStringLiteral("could not lay out the fixture tree"));
    }

    QFile sshCfg(m_home + QStringLiteral("/.ssh/config"));
    if (!sshCfg.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    sshCfg.write(QStringLiteral(
                     "Host %1\n"
                     "  HostName 127.0.0.1\n"
                     "  Port %2\n"
                     "  User %3\n"
                     "  IdentityFile %4\n"
                     "  IdentitiesOnly yes\n"
                     "  StrictHostKeyChecking no\n"
                     "  UserKnownHostsFile /dev/null\n")
                     .arg(QString::fromLatin1(kAlias))
                     .arg(m_fixture.port())
                     .arg(m_fixture.user(), m_fixture.keyPath())
                     .toUtf8());
    sshCfg.close();
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    /* HOME carries the ssh config; QSOC_HOME carries the endpoint definition
     * QLLMService::clone() rebuilds every child from. Both are redirected for
     * the whole run so no developer config leaks in. */
    m_hadHome     = qEnvironmentVariableIsSet("HOME");
    m_hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
    m_hadXdgHome  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_oldHome     = qgetenv("HOME");
    m_oldQsocHome = qgetenv("QSOC_HOME");
    m_oldXdgHome  = qgetenv("XDG_CONFIG_HOME");
    if (!qputenv("HOME", m_home.toUtf8()) || !qputenv("QSOC_HOME", m_home.toUtf8())
        || !qputenv("XDG_CONFIG_HOME", m_home.toUtf8())) {
        return fail(QStringLiteral("could not redirect the environment"));
    }
    return true;
}

void Test::cleanupTestCase()
{
    if (m_ready) {
        m_hadHome ? qputenv("HOME", m_oldHome) : qunsetenv("HOME");
        m_hadQsocHome ? qputenv("QSOC_HOME", m_oldQsocHome) : qunsetenv("QSOC_HOME");
        m_hadXdgHome ? qputenv("XDG_CONFIG_HOME", m_oldXdgHome) : qunsetenv("XDG_CONFIG_HOME");
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs
     * and the keys plus the work tree would survive every run. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Counterexample: the child's config came from the parent, so a child whose
 * tools reached the dispatch host was told to use the parent host's absolute
 * paths, and in a local-parent session was told there was no remote at all. */
void Test::aDispatchedChildIsToldTheWorkspaceItsToolsReach()
{
    REQUIRE_DISPATCH_FIXTURE();

    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueFinal(QStringLiteral("delegated work done"));

    QSocConfig  serviceConfig;
    QLLMService service(nullptr, &serviceConfig);
    QVERIFY(service.hasEndpoint());
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    QSocToolRegistry       registry;
    QSocHostCatalog        catalog;
    QVERIFY(registerHostB(&catalog));

    const QSocAgentConfig config = parentOnItsOwnHost();
    QSocAgent             parent(nullptr, &service, &registry, config);
    QSocToolAgent         tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    registry.registerTool(&tool);

    const QString raw      = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    response = json::parse(raw.toStdString());
    QCOMPARE(QString::fromStdString(response.value("status", std::string())), QStringLiteral("ok"));
    QCOMPARE(llm.requestCount(), 1);

    const QString prompt = systemPromptOf(llm, 0);
    QVERIFY2(!prompt.isEmpty(), qPrintable(QStringLiteral("no child system prompt: ") + raw));
    QVERIFY2(
        prompt.contains(m_workspace),
        qPrintable(QStringLiteral("its tools reach %1 but it was told \"%2\"")
                       .arg(m_workspace, remoteWorkspaceLine(prompt))));
    QVERIFY2(
        !prompt.contains(QString::fromLatin1(kParentWorkspace)),
        qPrintable(QStringLiteral("it was told it works in the parent host's workspace: \"%1\"")
                       .arg(remoteWorkspaceLine(prompt))));
    QVERIFY2(
        !prompt.contains(QString::fromLatin1(kParentTarget)),
        "the child was told its target is the parent's host");
}

void Test::forkLoadsRemoteRulesAtTheBindingBoundary()
{
    REQUIRE_DISPATCH_FIXTURE();
    const auto writeRules = [](const QString &root, const QByteArray &text) {
        QFile file(QDir(root).filePath(QStringLiteral("AGENTS.md")));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write(text) == text.size();
    };
    QVERIFY(writeRules(m_project, QByteArrayLiteral("Local parent sentinel")));
    QVERIFY(writeRules(m_workspace, QByteArrayLiteral("Bound remote sentinel")));
    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueToolCall(QStringLiteral("path_context"));
    llm.enqueueFinal(QStringLiteral("done"));
    llm.enqueueFinal(QStringLiteral("done again"));
    bool changed  = false;
    llm.onRequest = [&](int index) {
        if (index == 0) {
            changed = writeRules(m_workspace, QByteArrayLiteral("Rebound remote sentinel"));
        }
    };
    QSocConfig                  serviceConfig;
    QLLMService                 service(nullptr, &serviceConfig);
    QSocAgentDefinitionRegistry definitions;
    QSocSubAgentTaskSource      tasks;
    QSocToolRegistry            registry;
    QSocHostCatalog             catalog;
    QVERIFY(registerHostB(&catalog));
    auto config         = parentOnItsOwnHost();
    config.projectPath  = m_project;
    config.skillListing = QStringLiteral("Parent skill sentinel");
    config.remoteProjectRules
        = {config.remoteName, config.remoteWorkspace, QStringLiteral("Other remote sentinel")};
    QSocAgent     parent(nullptr, &service, &registry, config);
    QSocToolAgent tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    registry.registerTool(&tool);
    auto args             = spawnArgs();
    args["subagent_type"] = "fork";
    const auto first      = json::parse(tool.execute(args).toStdString());
    QCOMPARE(first.value("status", std::string()), std::string("ok"));
    QVERIFY(changed);
    QCOMPARE(llm.requestCount(), 2);
    const QString prompt = systemPromptOf(llm, 0);
    QCOMPARE(prompt, systemPromptOf(llm, 1));
    QCOMPARE(prompt.count(QStringLiteral("Bound remote sentinel")), 1);
    QCOMPARE(prompt.count(QStringLiteral("Local parent sentinel")), 1);
    QVERIFY(
        prompt.indexOf(QStringLiteral("Local parent sentinel"))
        < prompt.indexOf(QStringLiteral("Bound remote sentinel")));
    QVERIFY(!prompt.contains(QStringLiteral("Parent skill sentinel")));
    QVERIFY(!prompt.contains(QStringLiteral("Other remote sentinel")));
    QVERIFY(!prompt.contains(QStringLiteral("Rebound remote sentinel")));
    const auto second = json::parse(tool.execute(args).toStdString());
    QCOMPARE(second.value("status", std::string()), std::string("ok"));
    QCOMPARE(llm.requestCount(), 3);
    const QString rebound = systemPromptOf(llm, 2);
    QCOMPARE(rebound.count(QStringLiteral("Rebound remote sentinel")), 1);
    QVERIFY(!rebound.contains(QStringLiteral("Bound remote sentinel")));
    QVERIFY(writeRules(m_workspace, QByteArray(256 * 1024 + 1, 'x')));
    llm.enqueueFinal(QStringLiteral("bounded rules"));
    const auto oversized = json::parse(tool.execute(args).toStdString());
    QCOMPARE(oversized.value("status", std::string()), std::string("ok"));
    QCOMPARE(llm.requestCount(), 4);
    const QString limited = systemPromptOf(llm, 3);
    QVERIFY(limited.contains(QStringLiteral("AGENTS.md were not loaded")));
    QVERIFY(!limited.contains(QString(1024, QLatin1Char('x'))));
    QFile::remove(QDir(m_workspace).filePath(QStringLiteral("AGENTS.md")));
}

/* Counterexample: the child inherited the parent's workspace-health probe, so
 * an unreachable parent host ended the turn of a child that never touched it. */
void Test::aDispatchedChildIsNotStoppedByTheParentHostHealth()
{
    REQUIRE_DISPATCH_FIXTURE();

    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    /* One tool call, because the probe is consulted after each of them. */
    llm.enqueueToolCall(QStringLiteral("path_context"));
    llm.enqueueFinal(QStringLiteral("delegated work done"));

    QSocConfig  serviceConfig;
    QLLMService service(nullptr, &serviceConfig);
    QVERIFY(service.hasEndpoint());
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    QSocToolRegistry       registry;
    QSocHostCatalog        catalog;
    QVERIFY(registerHostB(&catalog));

    const QSocAgentConfig config = parentOnItsOwnHost();
    QSocAgent             parent(nullptr, &service, &registry, config);
    QSocToolAgent         tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    registry.registerTool(&tool);

    int parentProbeCalls = 0;
    parent.setWorkspaceHealthProbe([&parentProbeCalls]() -> QString {
        ++parentProbeCalls;
        return QString::fromLatin1(kParentUnhealthy);
    });

    const QString raw      = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    response = json::parse(raw.toStdString());
    const QString result   = QString::fromStdString(response.value("result", std::string()));

    QVERIFY2(
        !result.contains(QString::fromLatin1(kParentUnhealthy)),
        qPrintable(QStringLiteral("the child was stopped by the parent host's health: ") + result));
    QVERIFY2(
        parentProbeCalls == 0,
        qPrintable(QStringLiteral(
                       "the child asked the parent's host about its own workspace %1 "
                       "time(s)")
                       .arg(parentProbeCalls)));
    /* It ran to the end on the dispatch host instead of stopping on turn one. */
    QCOMPARE(QString::fromStdString(response.value("status", std::string())), QStringLiteral("ok"));
    QCOMPARE(result, QStringLiteral("delegated work done"));
    QCOMPARE(llm.requestCount(), 2);
}

/* Count of the worktrees git knows for @p repo, or -1 when git fails. */
int worktreeCount(const QString &repo)
{
    QProcess git;
    git.setWorkingDirectory(repo);
    git.start(QStringLiteral("git"), {QStringLiteral("worktree"), QStringLiteral("list")});
    if (!git.waitForFinished(15000) || git.exitCode() != 0) {
        return -1;
    }
    return static_cast<int>(git.readAllStandardOutput().count('\n'));
}

bool initRepo(const QString &repo)
{
    const QList<QStringList> steps
        = {{QStringLiteral("init"), QStringLiteral("-q")},
           {QStringLiteral("-c"),
            QStringLiteral("user.name=qsoc-test"),
            QStringLiteral("-c"),
            QStringLiteral("user.email=qsoc-test@example.invalid"),
            QStringLiteral("commit"),
            QStringLiteral("-q"),
            QStringLiteral("--allow-empty"),
            QStringLiteral("-m"),
            QStringLiteral("init")}};
    for (const QStringList &args : steps) {
        QProcess git;
        git.setWorkingDirectory(repo);
        git.start(QStringLiteral("git"), args);
        if (!git.waitForFinished(15000) || git.exitCode() != 0) {
            return false;
        }
    }
    return true;
}

/* Counterexample: a remote child asked for worktree isolation got a git
 * worktree of the local project, which none of its remote tools ever read. */
void Test::worktreeIsolationIsRefusedOnRemoteWorkspaces()
{
    REQUIRE_DISPATCH_FIXTURE();
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("git"));
    }
    const QString repo = m_dir.path() + QStringLiteral("/repo");
    QVERIFY(QDir().mkpath(repo));
    QVERIFY(initRepo(repo));
    QCOMPARE(worktreeCount(repo), 1);

    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueFinal(QStringLiteral("ran without isolation"));
    llm.enqueueFinal(QStringLiteral("ran without isolation"));

    QSocConfig                  serviceConfig;
    QLLMService                 service(nullptr, &serviceConfig);
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    QSocToolRegistry       registry;
    QSocHostCatalog        catalog;
    QVERIFY(registerHostB(&catalog));
    auto config        = parentOnItsOwnHost();
    config.projectPath = repo;
    QSocAgent     parent(nullptr, &service, &registry, config);
    QSocToolAgent tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    registry.registerTool(&tool);

    auto onParentHost         = spawnArgs();
    onParentHost["isolation"] = "worktree";
    onParentHost.erase("host");
    auto onDispatchHost         = spawnArgs();
    onDispatchHost["isolation"] = "worktree";
    for (const json &args : {onParentHost, onDispatchHost}) {
        const json response = json::parse(tool.execute(args).toStdString());
        QCOMPARE(response.value("status", std::string()), std::string("error"));
        QCOMPARE(
            response.value("error", std::string()),
            std::string("isolation=worktree is not supported on remote workspaces"));
    }
    QCOMPARE(llm.requestCount(), 0);
    QCOMPARE(worktreeCount(repo), 1);
}

/* Counterexample: a run stored the SSH target as its host, so resuming it named
 * a host the tool cannot dispatch to, and a catalog workspace changed since
 * the run moved the rebuilt child away from the tree its history describes. */
void Test::aResumedRunReturnsToItsAliasAndWorkspace()
{
    REQUIRE_DISPATCH_FIXTURE();
    const QString moved = m_fixture.workDir() + QStringLiteral("/moved");
    QVERIFY(QDir().mkpath(moved));
    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueFinal(QStringLiteral("first run done"));
    llm.enqueueFinal(QStringLiteral("resumed run done"));

    QTemporaryDir   runs;
    QSocHostCatalog catalog;
    catalog.load(QString(), m_project);
    QSocHostProfile profile;
    profile.alias     = QString::fromLatin1(kCatalogAlias);
    profile.workspace = m_workspace;
    profile.target    = QString::fromLatin1(kAlias);
    QVERIFY(catalog.upsert(profile, /*allowOverwrite=*/true));

    struct Harness
    {
        explicit Harness(const QString &runDir, QSocHostCatalog *catalog)
            : service(nullptr, &serviceConfig)
            , parent(nullptr, &service, &registry, parentOnItsOwnHost())
            , tool(nullptr, &service, &registry, parentOnItsOwnHost(), &definitions, &tasks)
        {
            definitions.registerBuiltins();
            tasks.setTranscriptDir(runDir);
            tool.setParentAgent(&parent);
            tool.setHostCatalog(catalog);
            registry.registerTool(&tool);
        }
        QSocConfig                  serviceConfig;
        QLLMService                 service;
        QSocAgentDefinitionRegistry definitions;
        QSocSubAgentTaskSource      tasks;
        QSocToolRegistry            registry;
        QSocAgent                   parent;
        QSocToolAgent               tool;
    };

    QString taskId;
    {
        Harness first(runs.path(), &catalog);
        auto    args    = spawnArgs();
        args["host"]    = kCatalogAlias;
        const json done = json::parse(first.tool.execute(args).toStdString());
        QCOMPARE(done.value("status", std::string()), std::string("ok"));
        taskId = QString::fromStdString(done.value("task_id", std::string()));
        QSocSubAgentTaskSource::HistoricalRun meta;
        QVERIFY(first.tasks.findHistoricalRun(taskId, &meta));
        QCOMPARE(meta.host, QString::fromLatin1(kCatalogAlias));
        QVERIFY2(meta.endpoint.contains(QString::fromLatin1(kAlias)), qPrintable(meta.endpoint));
        QCOMPARE(meta.workspace, m_workspace);
    }
    profile.workspace = moved;
    QVERIFY(catalog.upsert(profile, /*allowOverwrite=*/true));

    Harness    later(runs.path(), &catalog);
    const json resumed
        = later.tool.resumeRun(taskId, QStringLiteral("go on"), QSocAgentMailbox::userSender());
    QVERIFY2(resumed.value("resume", std::string()) == "history", resumed.dump().c_str());
    QTRY_COMPARE(llm.requestCount(), 2);
    const QString prompt = systemPromptOf(llm, 1);
    QVERIFY2(prompt.contains(m_workspace), qPrintable(remoteWorkspaceLine(prompt)));
    QVERIFY2(!prompt.contains(moved), qPrintable(remoteWorkspaceLine(prompt)));
}

/* Counterexample: a host in ~/.ssh/config without a catalog entry could not be
 * used even when the user granted it, because it had no workspace. */
void Test::aGrantedSshConfigAliasDispatchesWithAWorkspace()
{
    REQUIRE_DISPATCH_FIXTURE();
    const QString workspace = m_fixture.workDir() + QStringLiteral("/sshonly");
    QVERIFY(QDir().mkpath(workspace));
    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueFinal(QStringLiteral("ran on the ssh-config host"));

    QSocConfig                  serviceConfig;
    QLLMService                 service(nullptr, &serviceConfig);
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    QSocToolRegistry       registry;
    QSocHostCatalog        catalog;
    QFile::remove(m_project + QStringLiteral("/.qsoc/host.yml"));
    catalog.load(QString(), m_project);
    QVERIFY(catalog.find(QString::fromLatin1(kAlias)) == nullptr);
    QSocSshConfigParser parser;
    QVERIFY(parser.parse(m_home + QStringLiteral("/.ssh/config")));

    const QSocAgentConfig config = parentOnItsOwnHost();
    QSocAgent             parent(nullptr, &service, &registry, config);
    QSocToolAgent         tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    tool.setSshConfigParser(&parser);
    registry.registerTool(&tool);

    auto args            = spawnArgs();
    args["workspace"]    = workspace.toStdString();
    const json ungranted = json::parse(tool.execute(args).toStdString());
    QCOMPARE(ungranted.value("status", std::string()), std::string("error"));
    QCOMPARE(llm.requestCount(), 0);

    tool.setDispatchPolicy(
        QSocDispatchPolicy::fromNodes(
            YAML::Load(
                QStringLiteral("hosts:\n  %1: {}\n").arg(QString::fromLatin1(kAlias)).toStdString()),
            YAML::Node(),
            &catalog,
            &parser));
    const QString raw      = registry.executeTool(QStringLiteral("agent"), args);
    const json    response = json::parse(raw.toStdString());
    QVERIFY2(response.value("status", std::string()) == "ok", qPrintable(raw));
    QCOMPARE(llm.requestCount(), 1);
    const QString prompt = systemPromptOf(llm, 0);
    QVERIFY2(prompt.contains(workspace), qPrintable(remoteWorkspaceLine(prompt)));
}

/* Counterexample: the model could send a child to any directory of the host,
 * the root, the login directory and a tree outside the granted workspace
 * included, spelled through a symlink, and the refusal named the login path. */
void Test::aNamedWorkspaceMustBeAProjectDirectory()
{
    REQUIRE_DISPATCH_FIXTURE();
    const QString granted  = m_fixture.workDir() + QStringLiteral("/granted");
    const QString rootLink = granted + QStringLiteral("/rootlink");
    const QString escape   = granted + QStringLiteral("/escape");
    QVERIFY(QDir().mkpath(granted + QStringLiteral("/block")));
    QFile::remove(rootLink);
    QFile::remove(escape);
    QVERIFY(QFile::link(QStringLiteral("/"), rootLink));
    QVERIFY(QFile::link(m_workspace, escape));
    /* The loopback sshd logs in through the password database, not $HOME. */
    QString loginDir;
#ifdef Q_OS_UNIX
    const passwd *account = getpwuid(getuid());
    QVERIFY(account != nullptr);
    loginDir = QString::fromLocal8Bit(account->pw_dir);
#else
    QSKIP("needs the POSIX password database");
#endif
    const QString missing = granted + QStringLiteral("/never-created");

    MockLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));
    llm.enqueueFinal(QStringLiteral("inside the grant"));
    QSocConfig                  serviceConfig;
    QLLMService                 service(nullptr, &serviceConfig);
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    QSocToolRegistry       registry;
    QSocHostCatalog        catalog;
    QVERIFY(registerHostB(&catalog));
    const QSocAgentConfig config = parentOnItsOwnHost();
    QSocAgent             parent(nullptr, &service, &registry, config);
    QSocToolAgent         tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);

    const QList<QPair<QString, QString>> open
        = {{rootLink, QStringLiteral("is / or the login directory")},
           {loginDir, QStringLiteral("is / or the login directory")},
           {missing, QStringLiteral("does not exist")}};
    for (const auto &[workspace, expected] : open) {
        auto args              = spawnArgs();
        args["workspace"]      = workspace.toStdString();
        const json    response = json::parse(tool.execute(args).toStdString());
        const QString error    = QString::fromStdString(response.value("error", std::string()));
        QVERIFY2(error.contains(expected), qPrintable(workspace + QStringLiteral(": ") + error));
        QVERIFY2(workspace == loginDir || !error.contains(loginDir), qPrintable(error));
    }

    tool.setDispatchPolicy(
        QSocDispatchPolicy::fromNodes(
            YAML::Load(QStringLiteral("hosts:\n  %1: {workspace: %2}\n")
                           .arg(QString::fromLatin1(kAlias), granted)
                           .toStdString()),
            YAML::Node(),
            &catalog,
            nullptr));
    for (const QString &outside : {escape, m_workspace}) {
        auto args              = spawnArgs();
        args["workspace"]      = outside.toStdString();
        const json    response = json::parse(tool.execute(args).toStdString());
        const QString error    = QString::fromStdString(response.value("error", std::string()));
        QVERIFY2(error.contains(QStringLiteral("outside")), qPrintable(outside + ": " + error));
    }
    QCOMPARE(llm.requestCount(), 0);
    auto inside         = spawnArgs();
    inside["workspace"] = (granted + QStringLiteral("/block")).toStdString();
    const json done     = json::parse(tool.execute(inside).toStdString());
    QVERIFY2(done.value("status", std::string()) == "ok", done.dump().c_str());
    QVERIFY(!QFileInfo::exists(missing));
    QFile::remove(rootLink);
    QFile::remove(escape);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagenthostdispatch.moc"
