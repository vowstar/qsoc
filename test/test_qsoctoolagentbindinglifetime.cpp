// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocinterrupt.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * A sub-agent dispatched to a host resolves its tools through the binding the
 * spawn tool handed it, on every call, for as long as it runs. When that host
 * stops answering, the next dispatch to the same alias must stop handing the
 * binding out, so no sibling inherits a dead session.
 *
 * This case holds the memory to the child dispatched earlier: dropping the
 * binding from the cache must not free the registry a running child is still
 * calling through.
 *
 * The far side is a real loopback sshd, because a binding only exists once a
 * session does. The model endpoint accepts the child's request and never
 * answers it, which is what keeps the child mid-turn for the whole case.
 * Destruction is observed rather than inferred: the registry is a QObject, so
 * a QPointer to it reads null exactly when it was destroyed, whatever the
 * allocator then does with the memory.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name, pkill) skips this case, unless QSOC_TEST_DEPS_REQUIRED is set, which
 * CI does after installing the lot.
 */

namespace {

/* The alias the fixture's ~/.ssh/config and host catalog both define. */
constexpr auto kAlias = "bindinghost";

/* An endpoint that accepts a request and never answers it, so the child that
 * sent one stays mid-turn until the case is over. */
class SilentLlm final : public QObject
{
public:
    explicit SilentLlm(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                held_.append(server_.nextPendingConnection());
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

    /* How many requests are being held unanswered. */
    int heldRequests() const { return static_cast<int>(held_.size()); }

private:
    QTcpServer          server_;
    QList<QTcpSocket *> held_; /* owned by server_, deliberately never answered */
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aBindingOutlivesTheSpawnToolThatOpenedIt();
    /* Last: it takes the fixture's sshd down with it. */
    void anUnpublishedBindingOutlivesTheChildDispatchedOntoIt();

private:
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    /* Written per case: the endpoint port is only known after listen(). */
    bool writeLlmConfig(const SilentLlm &llm) const
    {
        QFile file(m_home + QStringLiteral("/qsoc.yml"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        /* Far longer than the case needs: an endpoint that never answers must
         * not time the child's turn out while the assertions run. */
        const QByteArray yaml = QByteArrayLiteral(
                                    "llm:\n"
                                    "  model: test-model\n"
                                    "  models:\n"
                                    "    test-model:\n"
                                    "      url: ")
                                + llm.url().toString().toUtf8()
                                + QByteArrayLiteral("\n      timeout: 600000\n");
        return file.write(yaml) == yaml.size();
    }

    /* The catalog entry the `host` argument resolves through. The target is
     * the alias itself, so the connect path reads the fixture's ssh config. */
    bool registerDispatchHost(QSocHostCatalog *catalog) const
    {
        catalog->load(QString(), m_project);
        QSocHostProfile profile;
        profile.alias      = QString::fromLatin1(kAlias);
        profile.workspace  = m_workspace;
        profile.capability = QStringLiteral("loopback dispatch target");
        profile.target     = QString::fromLatin1(kAlias);
        return catalog->upsert(profile, /*allowOverwrite=*/true);
    }

    /*
     * Make the host stop answering. Terminating the listener alone leaves the
     * per-connection session process serving SFTP, so the binding would still
     * pass a liveness probe; the session processes are signalled first, then
     * the listener, so nothing is left to accept a fresh connect either.
     */
    bool silenceHost()
    {
        QFile pidFile(m_fixture.root() + QStringLiteral("/sshd.pid"));
        if (!pidFile.open(QIODevice::ReadOnly)) {
            return fail(QStringLiteral("could not read the sshd pid file"));
        }
        const qint64 listener = QString::fromUtf8(pidFile.readAll()).trimmed().toLongLong();
        pidFile.close();
        if (listener <= 0) {
            return fail(QStringLiteral("the sshd pid file holds no pid"));
        }
        QProcess killer;
        killer.start(
            m_pkill, {QStringLiteral("-TERM"), QStringLiteral("-P"), QString::number(listener)});
        if (!killer.waitForFinished(5000)) {
            return fail(QStringLiteral("pkill did not finish"));
        }
        m_fixture.stop();
        return true;
    }

    static QSocAgentConfig parentConfig()
    {
        QSocAgentConfig config;
        config.verbose             = false;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxIterations       = 4;
        config.maxRetries          = 1;
        config.autoBackgroundMs    = 0;
        return config;
    }

    static json spawnArgs()
    {
        return json{
            {"subagent_type", "general-purpose"},
            {"description", "dispatch to the loopback host"},
            {"prompt", "report the workspace you are working in"},
            {"host", kAlias},
            {"run_in_background", true}};
    }

    static QString field(const json &response, const char *key)
    {
        return QString::fromStdString(response.value(key, std::string()));
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    bool          m_ready = false;
    QString       m_failure;
    QString       m_missing;
    QString       m_home;
    QString       m_project;
    QString       m_workspace;
    QString       m_transcripts;
    QString       m_pkill;
    QByteArray    m_oldHome;
    QByteArray    m_oldQsocHome;
    QByteArray    m_oldXdgHome;
    bool          m_hadHome     = false;
    bool          m_hadQsocHome = false;
    bool          m_hadXdgHome  = false;
};

/* First line of the case: the sshd fixture's own three-state policy, then the
 * dependencies and wiring this test adds on top of it. */
#define REQUIRE_BINDING_FIXTURE() \
    do { \
        QSOC_REQUIRE_SSHD(m_fixture); \
        if (!m_missing.isEmpty()) { \
            QSOC_TEST_MISSING_DEPENDENCY(m_missing); \
        } \
        if (!m_ready) { \
            QSOC_TEST_FIXTURE_FAILED(m_failure); \
        } \
    } while (false)

void Test::initTestCase()
{
    m_pkill = QStandardPaths::findExecutable(QStringLiteral("pkill"));
    if (m_pkill.isEmpty()) {
        m_missing = QStringLiteral("pkill");
        return;
    }
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
    m_transcripts      = root + QStringLiteral("/transcripts");
    m_workspace        = m_fixture.workDir() + QStringLiteral("/dispatched");
    if (!QDir().mkpath(m_home + QStringLiteral("/.ssh")) || !QDir().mkpath(m_project)
        || !QDir().mkpath(m_transcripts) || !QDir().mkpath(m_workspace)) {
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
    if (m_ready) {
        QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
    }
}

/* Counterexample: the binding's registry and tools hung off the spawn tool's
 * own Qt tree, so a parent that finished before its background child took the
 * child's whole tool set with it. */
void Test::aBindingOutlivesTheSpawnToolThatOpenedIt()
{
    REQUIRE_BINDING_FIXTURE();

    SilentLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));

    QSocConfig  serviceConfig;
    QLLMService service(nullptr, &serviceConfig);
    QCOMPARE(service.endpointCount(), 1);
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    tasks.setTranscriptDir(m_transcripts);
    QSocToolRegistry registry;
    QSocHostCatalog  catalog;
    QVERIFY(registerDispatchHost(&catalog));

    const QSocAgentConfig config = parentConfig();
    QSocAgent             parent(nullptr, &service, &registry, config);
    /* On the heap: the run under test is the spawn tool going away while the
     * child it dispatched is still working. */
    auto *tool = new QSocToolAgent(nullptr, &service, &registry, config, &definitions, &tasks);
    tool->setParentAgent(&parent);
    tool->setHostCatalog(&catalog);
    registry.registerTool(tool);

    const QString firstRaw  = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    first     = json::parse(firstRaw.toStdString());
    const QString firstTask = field(first, "task_id");
    QVERIFY2(field(first, "status") == QStringLiteral("async_launched"), qPrintable(firstRaw));
    QTRY_COMPARE(llm.heldRequests(), 1);

    const QList<QSocAgent *> children = tasks.findChildren<QSocAgent *>();
    QCOMPARE(children.size(), 1);
    const QPointer<QSocAgent>        childGuard(children.first());
    const QPointer<QSocToolRegistry> childRegistry(children.first()->getToolRegistry());
    QVERIFY2(
        childRegistry.data() != &registry,
        "the dispatched child runs on the parent's registry, so this case proves nothing");

    delete tool;

    QVERIFY2(!childGuard.isNull(), "the dispatched child was destroyed with the spawn tool");
    QSocTask::Row row;
    QVERIFY2(tasks.findRow(firstTask, &row), qPrintable(QStringLiteral("no run ") + firstTask));
    QVERIFY2(
        row.status == QSocTask::Status::Running,
        "the dispatched child already reached a terminal state, so it is not a live holder");
    QVERIFY2(
        !childRegistry.isNull(),
        "a live child called through a freed registry: the spawn tool's destructor destroyed the "
        "binding's tool registry while the child dispatched onto it was still running");
    QVERIFY2(
        childRegistry->getTool(QStringLiteral("read_file")) != nullptr,
        "the surviving registry lost the remote tools the live child dispatches through");
}

/* Counterexample: the sibling spawn that found the host gone deleted the
 * binding's registry, and the child already dispatched onto it kept calling
 * through the freed object. */
void Test::anUnpublishedBindingOutlivesTheChildDispatchedOntoIt()
{
    REQUIRE_BINDING_FIXTURE();

    SilentLlm llm;
    QVERIFY(llm.listen());
    QVERIFY(writeLlmConfig(llm));

    QSocConfig  serviceConfig;
    QLLMService service(nullptr, &serviceConfig);
    QCOMPARE(service.endpointCount(), 1);
    QSocAgentDefinitionRegistry definitions;
    definitions.registerBuiltins();
    QSocSubAgentTaskSource tasks;
    tasks.setTranscriptDir(m_transcripts);
    QSocToolRegistry registry;
    QSocHostCatalog  catalog;
    QVERIFY(registerDispatchHost(&catalog));

    const QSocAgentConfig config = parentConfig();
    QSocAgent             parent(nullptr, &service, &registry, config);
    QSocToolAgent         tool(nullptr, &service, &registry, config, &definitions, &tasks);
    tool.setParentAgent(&parent);
    tool.setHostCatalog(&catalog);
    registry.registerTool(&tool);

    /* First dispatch: opens the binding, starts the child, returns at once. */
    const QString firstRaw  = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    first     = json::parse(firstRaw.toStdString());
    const QString firstTask = field(first, "task_id");
    QVERIFY2(field(first, "status") == QStringLiteral("async_launched"), qPrintable(firstRaw));
    /* The child is mid-turn once its request is in the endpoint's hands. */
    QTRY_COMPARE(llm.heldRequests(), 1);

    /* The task source owns every registered child, so the run under test is
     * reachable without a back channel from the spawn tool. */
    const QList<QSocAgent *> children = tasks.findChildren<QSocAgent *>();
    QCOMPARE(children.size(), 1);
    const QPointer<QSocAgent>        childGuard(children.first());
    const QPointer<QSocToolRegistry> childRegistry(children.first()->getToolRegistry());
    QVERIFY2(
        childRegistry.data() != &registry,
        "the dispatched child runs on the parent's registry, so this case proves nothing");

    QVERIFY2(silenceHost(), qPrintable(m_failure));

    /* Second dispatch to the same alias. It must not reuse the dead binding,
     * and with the host gone it cannot open a new one either. */
    const QString secondRaw = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    second    = json::parse(secondRaw.toStdString());
    QVERIFY2(
        field(second, "status") == QStringLiteral("error")
            && field(second, "error").contains(QStringLiteral("not dispatchable")),
        qPrintable(
            QStringLiteral(
                "the stale binding was handed to a second spawn, so nothing was "
                "unpublished: ")
            + secondRaw));

    /* The first child is still running: it will resolve its next tool call
     * through the registry the second spawn just unpublished. */
    QVERIFY2(!childGuard.isNull(), "the dispatched child was destroyed before it could be used");
    QSocTask::Row row;
    QVERIFY2(tasks.findRow(firstTask, &row), qPrintable(QStringLiteral("no run ") + firstTask));
    QVERIFY2(
        row.status == QSocTask::Status::Running,
        "the dispatched child already reached a terminal state, so it is not a live holder");
    QCOMPARE(llm.heldRequests(), 1);

    QVERIFY2(
        !childRegistry.isNull(),
        "a live child called through a freed registry: a sibling spawn to the same host "
        "destroyed the binding's tool registry while the child dispatched onto it was still "
        "running");
    QVERIFY2(
        childRegistry->getTool(QStringLiteral("read_file")) != nullptr,
        "the surviving registry lost the remote tools the live child dispatches through");
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagentbindinglifetime.moc"
