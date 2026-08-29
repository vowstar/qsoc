// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

/*
 * End-to-end recovery behaviour of the agent against a remote workspace whose
 * link dies. A loopback sshd stands in for the remote host and a controllable
 * relay sits in front of it, so the link can be blackholed and healed at an
 * exact point without touching the host.
 *
 * Every assertion is made against the decoded request log the mock LLM
 * writes, not against terminal output. That is deliberate: an earlier version
 * of this scenario read "turns kept coming" off the screen and concluded the
 * workflow had recovered, when in fact the mock was looping. The wire is the
 * only place where what the model was actually told is visible.
 *
 * Waits are bounded predicates on that log, never fixed sleeps, so the
 * ordering is decided by observed progress rather than by a race.
 *
 * A dependency this fixture cannot supply itself (sshd, ssh-keygen, a login
 * name, a built qsoc) skips these cases, unless
 * QSOC_TEST_DEPS_REQUIRED is set, which CI does after installing the lot.
 * Anything else, a fixture that has its dependencies and still will not come
 * up, always fails: QtTest exits 0 on a skip, so a silent skip is
 * indistinguishable from coverage.
 */

namespace {

int pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = probe.serverPort();
    probe.close();
    return port;
}

/* MACOSX_BUNDLE puts the binary inside qsoc.app on macOS, so both layouts
 * have to be tried or the whole file is dead there. */
QString builtQsoc()
{
    const QDir buildDir(QStringLiteral(QT_TESTCASE_BUILDDIR));
    for (const QString &candidate :
         {QStringLiteral("../qsoc"), QStringLiteral("../qsoc.app/Contents/MacOS/qsoc")}) {
        const QString path = buildDir.absoluteFilePath(candidate);
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aDeadWorkspaceEndsTheTurnInsteadOfBurningTheCap();
    void aHealedLinkIsRebuiltAndTheAgentIsToldToReObserve();

private:
    /* Lines currently in the mock's decoded request log. */
    int wireRequests() const
    {
        QFile file(m_requestLog);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return 0;
        }
        int lines = 0;
        while (!file.atEnd()) {
            if (!file.readLine().trimmed().isEmpty()) {
                ++lines;
            }
        }
        return lines;
    }

    /** @brief Everything the model was sent, as one blob. */
    QByteArray wireLog() const
    {
        QFile file(m_requestLog);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /** @brief Everything the agent printed, as one blob. */
    QByteArray agentOutput() const
    {
        QFile file(m_dir.path() + QStringLiteral("/agent.out"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /* Bounded wait on an observed condition. Returns false on timeout so a
     * caller can assert rather than hang. */
    template<typename Predicate>
    static bool waitFor(Predicate ready, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            if (ready()) {
                return true;
            }
            QTest::qWait(50);
        }
        return ready();
    }

    /* Launch `qsoc agent -q` bound to the relay. The environment is built
     * from scratch so no proxy or user config leaks in. */
    void startAgent(const QString &query, int toolCallCap);
    void stopAgent();
    void stopMock();

    /* Everything after the dependency check. False leaves m_failure set, so
     * every early return here is already a failure and none has to be
     * remembered. */
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    QProcess      m_mock;
    QProcess      m_agent;
    int           m_mockPort = 0;
    bool          m_ready    = false;
    QString       m_missing;
    QString       m_failure;
    QString       m_qsoc;
    QString       m_mockBinary;
    QString       m_workspace;
    QString       m_requestLog;
    QString       m_home;
    QString       m_xdg;

    std::unique_ptr<QSocTestRelay> m_relay;
};

/* First line of every case: the sshd fixture's own three-state policy, then
 * the dependencies this test adds on top of it. */
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
    m_fixture.start();
    if (m_fixture.state() != QSocTestSshd::State::Ready) {
        return; /* the fixture's own state decides skip versus fail */
    }

    QStringList absent;
    m_qsoc = builtQsoc();
    if (m_qsoc.isEmpty()) {
        absent << QStringLiteral("a built qsoc binary");
    }
    /* The tracked copy, not one under .claude: that directory is ignored here,
     * so a fresh clone would find nothing and every case below would skip
     * while still looking like coverage. */
    m_mockBinary = QString::fromUtf8(QSOC_MOCK_LLM_PATH);
    if (!QFile::exists(m_mockBinary)) {
        absent << QStringLiteral("qsoc_mock_llm");
    }
    if (!absent.isEmpty()) {
        m_missing = absent.join(QStringLiteral(", "));
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
    m_xdg              = root + QStringLiteral("/xdg");
    m_workspace        = root + QStringLiteral("/work");
    m_requestLog       = root + QStringLiteral("/requests.jsonl");
    QDir().mkpath(m_home + QStringLiteral("/.ssh"));
    QDir().mkpath(m_xdg);
    QDir().mkpath(m_workspace);

    m_relay = std::make_unique<QSocTestRelay>(static_cast<quint16>(m_fixture.port()));
    m_relay->start();
    if (!m_relay->waitUntilListening(5000) || m_relay->port() == 0) {
        return fail(QStringLiteral("the relay never started listening"));
    }

    QFile sshCfg(m_home + QStringLiteral("/.ssh/config"));
    if (!sshCfg.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    sshCfg.write(QStringLiteral(
                     "Host relay\n"
                     "  HostName 127.0.0.1\n"
                     "  Port %1\n"
                     "  User %2\n"
                     "  IdentityFile %3\n"
                     "  IdentitiesOnly yes\n"
                     "  StrictHostKeyChecking no\n"
                     "  UserKnownHostsFile /dev/null\n")
                     .arg(m_relay->port())
                     .arg(m_fixture.user(), m_fixture.keyPath())
                     .toUtf8());
    sshCfg.close();
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_mockPort = pickFreePort();
    if (m_mockPort == 0) {
        return fail(QStringLiteral("no free loopback port for the mock"));
    }
    QFile projectCfg(root + QStringLiteral("/.qsoc.yml"));
    if (!projectCfg.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("could not write the project config"));
    }
    projectCfg.write(QStringLiteral(
                         "llm:\n"
                         "  model: mock\n"
                         "  models:\n"
                         "    mock:\n"
                         "      name: Mock\n"
                         "      url: \"http://127.0.0.1:%1/v1/chat/completions\"\n"
                         "      key: placeholder\n"
                         "      timeout: 20000\n"
                         "      context: 131072\n"
                         "      max_output_tokens: 4096\n"
                         "      reasoning: false\n"
                         "proxy:\n"
                         "  type: none\n")
                         .arg(m_mockPort)
                         .toUtf8());
    projectCfg.close();

    return true;
}

void Test::cleanupTestCase()
{
    stopAgent();
    stopMock();
    if (m_relay) {
        m_relay->stop();
        m_relay.reset();
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs
     * and the keys, work tree and logs would be left behind on every run. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

void Test::stopMock()
{
    if (m_mock.state() == QProcess::NotRunning) {
        return;
    }
    m_mock.terminate();
    if (!m_mock.waitForFinished(3000)) {
        m_mock.kill();
        m_mock.waitForFinished(2000);
    }
}

void Test::startAgent(const QString &query, int toolCallCap)
{
    /* Each case gets its own mock and its own log: a leftover process from a
     * previous case would silently keep serving, and the request log would
     * then mix two runs. */
    stopAgent();
    stopMock();
    QFile::remove(m_requestLog);

    QProcessEnvironment env;
    env.insert(QStringLiteral("HOME"), m_home);
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), m_xdg);
    env.insert(QStringLiteral("QSOC_HOME"), m_home + QStringLiteral("/.qsoc"));
    env.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
    env.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));

    QProcessEnvironment mockEnv = env;
    mockEnv.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("180"));
    mockEnv.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnv.insert(QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("bash"));
    mockEnv.insert(
        QStringLiteral("MOCK_TOOL_ARGS"),
        QStringLiteral("{\"command\":\"printf REMOTE_ALIVE\",\"timeout_ms\":3000}"));
    mockEnv.insert(QStringLiteral("MOCK_TOOL_MAX"), QString::number(toolCallCap));
    mockEnv.insert(QStringLiteral("MOCK_REQUEST_LOG"), m_requestLog);

    m_mock.setProcessEnvironment(mockEnv);
    m_mock.setStandardOutputFile(m_dir.path() + QStringLiteral("/mock.log"));
    m_mock.setStandardErrorFile(m_dir.path() + QStringLiteral("/mock.err"));
    m_mock.start(m_mockBinary, {QString::number(m_mockPort), QStringLiteral("none")});
    QVERIFY(m_mock.waitForStarted(5000));
    QVERIFY(waitFor(
        [this] {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_mockPort));
            return probe.waitForConnected(200);
        },
        8000));

    m_agent.setProcessEnvironment(env);
    m_agent.setWorkingDirectory(m_dir.path());
    m_agent.setStandardOutputFile(m_dir.path() + QStringLiteral("/agent.out"));
    m_agent.setStandardErrorFile(m_dir.path() + QStringLiteral("/agent.err"));
    m_agent.start(
        m_qsoc,
        {QStringLiteral("agent"),
         QStringLiteral("--ssh"),
         QStringLiteral("relay"),
         QStringLiteral("--workspace"),
         m_workspace,
         QStringLiteral("-q"),
         query});
    QVERIFY(m_agent.waitForStarted(10000));
}

void Test::stopAgent()
{
    if (m_agent.state() == QProcess::NotRunning) {
        return;
    }
    m_agent.terminate();
    if (!m_agent.waitForFinished(5000)) {
        m_agent.kill();
        m_agent.waitForFinished(3000);
    }
}

/*
 * I16. A workspace that stopped answering does not answer the next call
 * either, so the turn must end. Without that the loop feeds the same refusal
 * back to the model until the iteration cap, which cost a hundred requests
 * and a slice of context to rediscover.
 */
void Test::aDeadWorkspaceEndsTheTurnInsteadOfBurningTheCap()
{
    REQUIRE_RECOVERY_FIXTURE();
    m_relay->heal();

    /* No cap on the mock: left alone it would answer with the same tool call
     * forever, so the bound has to come from the agent. */
    startAgent(QStringLiteral("do the remote work"), /*toolCallCap=*/0);

    /* Wait until the model has actually been asked something, then cut. */
    QVERIFY2(waitFor([this] { return wireRequests() >= 1; }, 60000), "the model was never asked");
    m_relay->blackhole();

    QVERIFY2(
        waitFor([this] { return m_agent.state() == QProcess::NotRunning; }, 120000),
        "the agent never finished after the workspace died");

    const int served = wireRequests();
    QVERIFY2(
        served >= 2,
        qPrintable(QStringLiteral(
                       "expected the failure to be observed, "
                       "only %1 request(s) served")
                       .arg(served)));
    /* The iteration cap is 100. Ending the turn means a handful of requests,
     * not that. */
    QVERIFY2(
        served < 20, qPrintable(QStringLiteral("turn did not end: %1 requests served").arg(served)));
    QVERIFY2(
        !wireLog().contains("Agent safety limit reached"),
        "the run hit the iteration cap instead of ending the turn");
}

/*
 * I20. A partition that heals must not leave an unattended run dead. The link
 * is rebuilt, and the model is told on the wire that remote state is stale
 * and must be verified before acting.
 */
void Test::aHealedLinkIsRebuiltAndTheAgentIsToldToReObserve()
{
    REQUIRE_RECOVERY_FIXTURE();
    m_relay->heal();
    startAgent(QStringLiteral("do the remote work"), /*toolCallCap=*/0);

    QVERIFY2(waitFor([this] { return wireRequests() >= 1; }, 60000), "the model was never asked");
    const int connectedBeforeCut = m_relay->acceptedConnectionCount();
    QVERIFY2(connectedBeforeCut > 0, "the initial SSH connection never reached the relay");
    m_relay->blackholeUntilNextConnection();

    /* A new accepted socket is the reconnect itself. Waiting for another
     * model request is circular: the agent sends that request only after the
     * reconnect succeeds. */
    QVERIFY2(
        waitFor(
            [this, connectedBeforeCut] {
                return m_relay->acceptedConnectionCount() > connectedBeforeCut;
            },
            120000),
        "the agent never opened a replacement SSH connection");
    QVERIFY2(
        waitFor([this] { return wireLog().contains("treat every belief"); }, 120000),
        "the agent was never told to re-observe remote state");

    const QByteArray wire = wireLog();
    QVERIFY(wire.contains("has been re-established"));
    QVERIFY(wire.contains("write_file and edit_file will refuse"));
    QVERIFY(wire.contains("Do not re-run a command whose effect you have not verified"));

    /* The reason has to reach the human as well. The soft stop restarts on the
     * queued brief instead of ending the run, so a notice delivered only at
     * run teardown is dropped exactly when a workflow carries on unattended. */
    QVERIFY2(
        waitFor(
            [this] { return agentOutput().contains("The SSH link was re-established after"); },
            60000),
        "the terminal never said the link had been re-established");

    stopAgent();
}

QSOC_TEST_MAIN(Test)
#include "test_qsocagentremoterecovery.moc"
