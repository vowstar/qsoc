// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

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
    void aDispatchedChildIsNotStoppedByTheParentHostHealth();

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
                     "  IdentitiesOnly yes\n")
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
    QCOMPARE(service.endpointCount(), 1);
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
    QCOMPARE(service.endpointCount(), 1);
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

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagenthostdispatch.moc"
