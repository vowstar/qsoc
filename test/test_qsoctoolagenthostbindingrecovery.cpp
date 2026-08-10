// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
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
 * A sub-agent dispatched to another host must sense that host's link loss and
 * try to recover it. The blocker was that a dispatched child got no probe at
 * all and the binding's connection had no rebuilder, so host B was neither
 * sensed (probe read "usable") nor recovered (reconnect answered Refused). A
 * folded-in finding: once reconnect is real, the child's config cwd must not go
 * stale after a rewind.
 *
 * The connection cases are in-memory: an unconnected QSocSshSession stands in
 * for a dropped link exactly, so a rebuilder and a probe can be exercised
 * without a network. The wiring case needs a real dispatched child, so it uses
 * the loopback sshd fixture and asks the child itself whether it now carries a
 * host-B probe.
 */

namespace {

/* The alias the fixture's ~/.ssh/config and host catalog both define. */
constexpr auto kAlias = "recoveryhost";

/* A transport the test owns. The session is unconnected, so isUsable() reads
 * false: the same state a dropped link leaves behind. */
AgentRemoteState fakeTransport(QObject *scratch, const QString &target, const QString &workspace)
{
    AgentRemoteState state;
    state.session   = new QSocSshSession(scratch);
    state.sftp      = new QSocSftpClient(*state.session);
    state.targetKey = target;
    state.workspace = workspace;
    return state;
}

/* A rebuilder that produces a fresh (still unconnected) transport and counts
 * its calls, so "a reconnect was attempted" is observable. */
QSocRemoteConnection::Rebuilder countingRebuilder(QObject *scratch, int *builds)
{
    return
        [scratch,
         builds](const QString &target, const QString &workspace, AgentRemoteState *out, QString *) {
            if (builds != nullptr) {
                ++(*builds);
            }
            *out = fakeTransport(scratch, target, workspace);
            return true;
        };
}

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

    /* In-memory: the connection cases. */
    void aBoundBindingReconnectsWhereItUsedToRefuse();
    void aChildProbeSensesTheLossAndAttemptsRecovery();
    void aReconnectKeepsTheChildConfigCwdLive();

    /* Real loopback sshd: the execute() wiring. Last, it takes the sshd down. */
    void aDispatchedChildCarriesAHostBHealthProbe();

private:
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    bool writeLlmConfig(const SilentLlm &llm) const
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
                                + QByteArrayLiteral("\n      timeout: 600000\n");
        return file.write(yaml) == yaml.size();
    }

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

#define REQUIRE_RECOVERY_FIXTURE() \
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
        return;
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
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    if (m_ready) {
        QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
    }
}

/*
 * Counterexample: the binding built by resolveHostBinding carried no rebuilder,
 * so a dropped host B could only answer Refused. installBindingRecovery gives it
 * one built from the same connect helpers, so a real attempt is made instead.
 * An unroutable target keeps it in-memory: the attempt fails fast (Exhausted),
 * which is still not Refused.
 */
void Test::aBoundBindingReconnectsWhereItUsedToRefuse()
{
    QObject              scratch;
    QSocRemoteConnection conn;
    QVERIFY(
        conn.adopt(fakeTransport(&scratch, QStringLiteral("127.0.0.1:1"), QStringLiteral("/w"))));
    QCOMPARE(conn.isUsable(), false);

    /* Before the fix: no rebuilder, so nothing to recover from. */
    QString beforeErr;
    QCOMPARE(conn.reconnect(&beforeErr), QSocRemoteConnection::ReconnectOutcome::Refused);

    /* After the fix: the production wiring installs a real, firing rebuilder. */
    QSocToolAgent::installBindingRecovery(&conn);
    conn.resetReconnectBudget();
    QString    afterErr;
    const auto outcome = conn.reconnect(&afterErr);
    QVERIFY2(
        outcome != QSocRemoteConnection::ReconnectOutcome::Refused,
        "the binding still refuses to reconnect: no rebuilder was installed");
    QCOMPARE(outcome, QSocRemoteConnection::ReconnectOutcome::Exhausted);
    QVERIFY(conn.lastReconnectAttempts() >= 1);
}

/*
 * Counterexample: a child dispatched to host B got no probe, so probeBindingHealth
 * never ran and the loss read as "usable". The probe now senses the dropped link
 * and drives the recovery in one step.
 */
void Test::aChildProbeSensesTheLossAndAttemptsRecovery()
{
    QObject              scratch;
    QSocRemoteConnection conn;
    QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
    QCOMPARE(conn.isUsable(), false);

    /* Before the fix: with no rebuilder the loss is felt but nothing recovers. */
    QString sensedNoBuilder = QSocToolAgent::probeBindingHealth(&conn);
    QVERIFY2(!sensedNoBuilder.isEmpty(), "the probe reported host B usable while its link was down");

    /* After the fix: a rebuilder is installed and the probe reconnects. */
    int builds = 0;
    conn.setRebuilder(countingRebuilder(&scratch, &builds));
    conn.resetReconnectBudget();
    const QString reported = QSocToolAgent::probeBindingHealth(&conn);
    QVERIFY2(!reported.isEmpty(), "the probe reported host B usable while its link was down");
    QCOMPARE(builds, 1);
    QVERIFY2(
        reported.contains(QStringLiteral("re-established")),
        qPrintable(QStringLiteral("the probe did not drive a recovery: ") + reported));
}

/*
 * Folded-in counterexample: with reconnect real, the child's config cwd would go
 * stale on a rewind the way the primary connection's did. bindChildCwd routes the
 * binding's cwd into the child's config, so after a reconnect the config names the
 * directory the binding actually landed on.
 */
void Test::aReconnectKeepsTheChildConfigCwdLive()
{
    QObject              scratch;
    QSocRemoteConnection conn;
    QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
    /* The host confirms the working directory, so a rebind keeps it. */
    conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return true; });
    conn.path()->setCwd(QStringLiteral("/w/live"));
    conn.setRebuilder(countingRebuilder(&scratch, nullptr));

    QSocAgentConfig config  = parentConfig();
    config.remoteWorkingDir = QStringLiteral("/w/stale");
    QSocAgent child(nullptr, nullptr, nullptr, config);

    /* Before the fix: no observer, so the config keeps its stale snapshot. */
    conn.resetReconnectBudget();
    QString reconnectErr;
    QCOMPARE(conn.reconnect(&reconnectErr), QSocRemoteConnection::ReconnectOutcome::Reconnected);
    QCOMPARE(conn.path()->cwd(), QStringLiteral("/w/live"));
    QCOMPARE(child.getConfig().remoteWorkingDir, QStringLiteral("/w/stale"));

    /* After the fix: the observer writes the live cwd into the config. */
    QSocToolAgent::bindChildCwd(&conn, &child);
    conn.resetReconnectBudget();
    QCOMPARE(conn.reconnect(&reconnectErr), QSocRemoteConnection::ReconnectOutcome::Reconnected);
    QCOMPARE(conn.path()->cwd(), QStringLiteral("/w/live"));
    QCOMPARE(child.getConfig().remoteWorkingDir, conn.path()->cwd());
}

/*
 * The execute() wiring, against a real dispatched child: before the fix a host-B
 * child carried no probe, so probeWorkspaceHealth() reported "" (usable) even
 * after the host went away. It now carries one, and that probe senses the loss.
 */
void Test::aDispatchedChildCarriesAHostBHealthProbe()
{
    REQUIRE_RECOVERY_FIXTURE();

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

    const QString firstRaw = registry.executeTool(QStringLiteral("agent"), spawnArgs());
    const json    first    = json::parse(firstRaw.toStdString());
    QVERIFY2(field(first, "status") == QStringLiteral("async_launched"), qPrintable(firstRaw));
    QTRY_COMPARE(llm.heldRequests(), 1);

    const QList<QSocAgent *> children = tasks.findChildren<QSocAgent *>();
    QCOMPARE(children.size(), 1);
    QSocAgent *child = children.first();

    /* The wiring the fix adds: a child dispatched to host B now carries a probe
     * of its own. Before the fix this was false. */
    QVERIFY2(
        child->hasWorkspaceHealthProbe(),
        "the dispatched child carries no health probe, so host B is never sensed");

    /* While the host answers, the probe reports it usable. */
    QVERIFY2(child->probeWorkspaceHealth().isEmpty(), "the probe reported a live host B unusable");

    QVERIFY2(silenceHost(), qPrintable(m_failure));

    /* Once the host is gone, the probe senses it rather than reading usable. */
    QVERIFY2(
        !child->probeWorkspaceHealth().isEmpty(),
        "the probe reported host B usable after its link was silenced");
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagenthostbindingrecovery.moc"
