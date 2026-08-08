// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_relay.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <unistd.h>
#endif

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
 * QSKIPs (never fails) when the environment cannot host the fixture: no
 * sshd, no ssh-keygen, no python3, no built binary, or key generation fails.
 */

namespace {

QString findExe(const QStringList &candidates)
{
    for (const QString &path : candidates) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

/* $USER is unset in CI non-login shells, so resolve via the password
 * database and fall back to the env var only off-POSIX. */
QString currentUser()
{
#ifndef Q_OS_WIN
    if (const struct passwd *pwd = getpwuid(getuid())) {
        if (pwd->pw_name != nullptr && pwd->pw_name[0] != '\0') {
            return QString::fromLocal8Bit(pwd->pw_name);
        }
    }
#endif
    return qEnvironmentVariable("USER");
}

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

/* RSA in classic PEM: qsoc pins userauth to the rsa-sha2 family, and this
 * libssh2 build signs RSA pubkey auth reliably only from a "BEGIN RSA
 * PRIVATE KEY" file. Generated at runtime; no key is ever committed. */
bool runKeygen(const QString &keygen, const QString &keyPath)
{
    QProcess proc;
    proc.start(
        keygen,
        {QStringLiteral("-t"),
         QStringLiteral("rsa"),
         QStringLiteral("-b"),
         QStringLiteral("2048"),
         QStringLiteral("-m"),
         QStringLiteral("PEM"),
         QStringLiteral("-N"),
         QString(),
         QStringLiteral("-q"),
         QStringLiteral("-f"),
         keyPath});
    return proc.waitForStarted(5000) && proc.waitForFinished(20000)
           && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0
           && QFile::exists(keyPath);
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

    QTemporaryDir m_dir;
    QProcess      m_sshd;
    QProcess      m_mock;
    QProcess      m_agent;
    int           m_sshdPort = 0;
    int           m_mockPort = 0;
    bool          m_ready    = false;
    QString       m_qsoc;
    QString       m_mockScript;
    QString       m_keyPath;
    QString       m_user;
    QString       m_workspace;
    QString       m_requestLog;
    QString       m_home;
    QString       m_xdg;

    std::unique_ptr<QSocTestRelay> m_relay;
};

void Test::initTestCase()
{
    const QString sshd = findExe(
        {QStringLiteral("/usr/sbin/sshd"), QStringLiteral("/usr/bin/sshd")});
    const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    m_qsoc               = QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).absoluteFilePath("../qsoc");
    /* The tracked copy, not one under .claude: that directory is ignored here,
     * so a fresh clone would find nothing and every case below would skip
     * while still looking like coverage. */
    m_mockScript = QDir(QStringLiteral(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("qsoc_mock_llm.py");
    m_user       = currentUser();

    if (sshd.isEmpty() || keygen.isEmpty() || python.isEmpty() || m_user.isEmpty()
        || !m_dir.isValid() || !QFile::exists(m_qsoc) || !QFile::exists(m_mockScript)) {
        return; /* m_ready stays false -> every case QSKIPs */
    }

    const QString root = m_dir.path();
    m_home             = root + QStringLiteral("/home");
    m_xdg              = root + QStringLiteral("/xdg");
    m_workspace        = root + QStringLiteral("/work");
    m_requestLog       = root + QStringLiteral("/requests.jsonl");
    m_keyPath          = root + QStringLiteral("/client_rsa");
    QDir().mkpath(m_home + QStringLiteral("/.ssh"));
    QDir().mkpath(m_xdg);
    QDir().mkpath(m_workspace);

    const QString hostKey = root + QStringLiteral("/host_rsa");
    if (!runKeygen(keygen, hostKey) || !runKeygen(keygen, m_keyPath)) {
        return;
    }
    if (!QFile::copy(m_keyPath + QStringLiteral(".pub"), root + QStringLiteral("/authorized_keys"))) {
        return;
    }
    QFile::setPermissions(
        root + QStringLiteral("/authorized_keys"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_sshdPort = pickFreePort();
    if (m_sshdPort == 0) {
        return;
    }
    const QString cfgPath = root + QStringLiteral("/sshd_config");
    QFile         cfg(cfgPath);
    if (!cfg.open(QIODevice::WriteOnly)) {
        return;
    }
    cfg.write(QStringLiteral(
                  "Port %1\n"
                  "ListenAddress 127.0.0.1\n"
                  "HostKey %2\n"
                  "PidFile %3/sshd.pid\n"
                  "AuthorizedKeysFile %3/authorized_keys\n"
                  "UsePAM no\n"
                  "StrictModes no\n"
                  "PasswordAuthentication no\n"
                  "KbdInteractiveAuthentication no\n"
                  "PubkeyAuthentication yes\n"
                  "Subsystem sftp internal-sftp\n"
                  "LogLevel ERROR\n")
                  .arg(m_sshdPort)
                  .arg(hostKey)
                  .arg(root)
                  .toUtf8());
    cfg.close();

    m_sshd.setStandardErrorFile(root + QStringLiteral("/sshd.err"));
    m_sshd.start(sshd, {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), cfgPath});
    if (!m_sshd.waitForStarted(5000)) {
        return;
    }
    const bool listening = waitFor(
        [this] {
            if (m_sshd.state() != QProcess::Running) {
                return false;
            }
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_sshdPort));
            return probe.waitForConnected(200);
        },
        8000);
    if (!listening) {
        return;
    }

    m_relay = std::make_unique<QSocTestRelay>(static_cast<quint16>(m_sshdPort));
    m_relay->start();
    if (!m_relay->waitUntilListening(5000) || m_relay->port() == 0) {
        return;
    }

    QFile sshCfg(m_home + QStringLiteral("/.ssh/config"));
    if (!sshCfg.open(QIODevice::WriteOnly)) {
        return;
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
                     .arg(m_user, m_keyPath)
                     .toUtf8());
    sshCfg.close();
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_mockPort = pickFreePort();
    if (m_mockPort == 0) {
        return;
    }
    QFile projectCfg(root + QStringLiteral("/.qsoc.yml"));
    if (!projectCfg.open(QIODevice::WriteOnly)) {
        return;
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

    m_ready = true;
}

void Test::cleanupTestCase()
{
    stopAgent();
    stopMock();
    if (m_relay) {
        m_relay->stop();
        m_relay.reset();
    }
    if (m_sshd.state() != QProcess::NotRunning) {
        m_sshd.terminate();
        if (!m_sshd.waitForFinished(3000)) {
            m_sshd.kill();
            m_sshd.waitForFinished(2000);
        }
    }
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs
     * and the keys, work tree and logs would be left behind on every run. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
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
    if (!m_ready) {
        QSKIP("loopback sshd, python3 or the built binary is unavailable here");
    }
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
    if (!m_ready) {
        QSKIP("loopback sshd, python3 or the built binary is unavailable here");
    }
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
