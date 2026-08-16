// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

/*
 * What a turn costs once the link to the remote workspace has gone quiet.
 *
 * The whole sequence runs on the Qt event loop, so the elapsed time measured
 * here is time the interface is frozen and the user cannot act. That is the
 * quantity under test: a configured cap is not a measurement, and the two came
 * apart badly here before.
 *
 * A blackholed relay is the case worth measuring. Closing the socket would send
 * FIN and every wait would end early; a host that accepts and never answers is
 * what leaves each attempt sitting on its full budget.
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

/* What one turn on a link that stopped answering may hold the event loop for.
 * Above one tool call plus one bounded reconnect, below the sum of a reconnect
 * per tool call, so the bound distinguishes the two. */
constexpr qint64 kTurnBudgetMs = 80000;

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aDeadLinkTurnIsBoundedAndSaysWhyItStopped();
    void aFlappingLinkIsNotReconnectedOncePerToolCall();

private:
    QByteArray agentOutput() const
    {
        QFile file(m_dir.path() + QStringLiteral("/agent.out"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

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

    void startAgent(int llmDelaySec = 0);
    void stopAgent();
    void stopMock();
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
    QString       m_mockScript;
    QString       m_workspace;
    QString       m_requestLog;
    QString       m_home;
    QString       m_xdg;

    std::unique_ptr<QSocTestRelay> m_relay;
};

#define REQUIRE_COST_FIXTURE() \
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
    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()) {
        absent << QStringLiteral("python3");
    }
    m_qsoc = builtQsoc();
    if (m_qsoc.isEmpty()) {
        absent << QStringLiteral("a built qsoc binary");
    }
    m_mockScript = QDir(QStringLiteral(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("qsoc_mock_llm.py");
    if (!QFile::exists(m_mockScript)) {
        absent << QStringLiteral("qsoc_mock_llm.py");
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

void Test::startAgent(int llmDelaySec)
{
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
    mockEnv.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("600"));
    if (llmDelaySec > 0) {
        /* Widen the gap between one answer and the next tool call, so the link
         * can be taken away inside it. Without that the gap is one loopback
         * round trip and the cut always lands on the wrong side of it. */
        mockEnv.insert(QStringLiteral("MOCK_DELAY"), QString::number(llmDelaySec));
    }
    mockEnv.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnv.insert(QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("bash"));
    mockEnv.insert(
        QStringLiteral("MOCK_TOOL_ARGS"),
        QStringLiteral("{\"command\":\"printf REMOTE_ALIVE\",\"timeout_ms\":3000}"));
    /* No cap: left alone the mock answers with the same tool call forever, so
     * any bound on the turn has to come from the agent. */
    mockEnv.insert(QStringLiteral("MOCK_TOOL_MAX"), QStringLiteral("0"));
    mockEnv.insert(QStringLiteral("MOCK_REQUEST_LOG"), m_requestLog);

    m_mock.setProcessEnvironment(mockEnv);
    m_mock.setStandardOutputFile(m_dir.path() + QStringLiteral("/mock.log"));
    m_mock.setStandardErrorFile(m_dir.path() + QStringLiteral("/mock.err"));
    m_mock.start(
        QStandardPaths::findExecutable(QStringLiteral("python3")),
        {m_mockScript, QString::number(m_mockPort), QStringLiteral("none")});
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
         QStringLiteral("do the remote work")});
    QVERIFY(m_agent.waitForStarted(10000));
}

/*
 * The measurement, and the sentence the user is left with. The clock starts at
 * the cut, because everything before it is a healthy turn.
 */
void Test::aDeadLinkTurnIsBoundedAndSaysWhyItStopped()
{
    REQUIRE_COST_FIXTURE();
    m_relay->heal();
    startAgent();

    QVERIFY2(waitFor([this] { return wireRequests() >= 1; }, 60000), "the model was never asked");

    QElapsedTimer frozen;
    frozen.start();
    m_relay->blackhole();
    const bool finished
        = waitFor([this] { return m_agent.state() == QProcess::NotRunning; }, 420000);
    const qint64 costMs = frozen.elapsed();

    qInfo("dead-link turn cost: %lld ms", costMs);
    QVERIFY2(
        finished,
        qPrintable(QStringLiteral(
                       "the turn never ended; %1 ms and counting after "
                       "the link went quiet")
                       .arg(costMs)));
    QVERIFY2(
        costMs < kTurnBudgetMs,
        qPrintable(QStringLiteral(
                       "the interface was frozen for %1 ms after the link went quiet, "
                       "over the %2 ms one turn is allowed")
                       .arg(costMs)
                       .arg(kTurnBudgetMs)));

    /* And the sentence must not claim the host was asked when it was not. */
    const QByteArray out = agentOutput();
    if (out.contains("it was not retried")) {
        QVERIFY2(
            !out.contains("reconnect failed after"),
            "the notice both refused to retry and claimed the host had been retried");
    }
}

/*
 * A link that comes back and dies again is where the per-call cost multiplies.
 * The reconnect succeeds, the agent is handed a re-observation brief, the run
 * restarts on it, and the next tool call finds the link gone once more. Nothing
 * has been observed working in between, so paying for a second full connect
 * sequence buys nothing and costs the interface another one of these.
 */
void Test::aFlappingLinkIsNotReconnectedOncePerToolCall()
{
    REQUIRE_COST_FIXTURE();
    m_relay->heal();
    startAgent(1);

    /* The second request proves one remote tool completed before the cut. */
    QVERIFY2(
        waitFor([this] { return wireRequests() >= 2; }, 60000),
        "the first remote tool never completed");

    m_relay->blackholeUntilNextConnection();
    QVERIFY2(
        waitFor([this] { return agentOutput().contains("re-established"); }, 240000),
        "the link was never re-established");

    /* Now the turn has reconnected once. Take the link away again: the next
     * tool call in it must not pay for a second full connect sequence. */
    QElapsedTimer frozen;
    frozen.start();
    m_relay->blackhole();
    const bool finished
        = waitFor([this] { return m_agent.state() == QProcess::NotRunning; }, 420000);
    const qint64 costMs = frozen.elapsed();

    qInfo("flapping-link run cost: %lld ms", costMs);
    for (const QByteArray &line : agentOutput().split('\n')) {
        if (line.contains("SSH") || line.contains("reconnect") || line.contains("retried")) {
            qInfo("notice: %s", line.constData());
        }
    }
    QVERIFY2(
        finished, qPrintable(QStringLiteral("the run never ended; %1 ms and counting").arg(costMs)));
    QVERIFY2(
        agentOutput().contains("it was not retried"),
        "the second failure in this turn paid for another full connect sequence instead of "
        "reporting that the turn had already reconnected");
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotedeadlinkcost.moc"
