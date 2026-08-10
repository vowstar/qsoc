// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

/*
 * Who owns the remote working directory, and whether the name it publishes is
 * where the session actually is.
 *
 * Both cases are asserted against the decoded request log the mock LLM writes,
 * because the system prompt on the wire is the only place the directory the
 * model was told about is visible. Reading it off the `path_context` tool
 * result instead would pass while the config carried a stale copy into every
 * later turn, which is the first defect here.
 *
 * The host is a loopback sshd, so the remote filesystem is this filesystem.
 * That is what lets the second case check where a name resolves to rather than
 * how it is spelled.
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
    void aPathToolCwdChangeIsWhatTheNextTurnIsTold();
    void aCwdReachingOutOfTheWorkspaceThroughALinkIsRefused();

private:
    /** @brief Everything the model was sent, as one blob. */
    QByteArray wireLog() const
    {
        QFile file(m_requestLog);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /**
     * @brief The working directory the last system prompt declared.
     * @details Empty when no request carried one, which is itself a failure
     *          the caller must report rather than compare against.
     */
    QString declaredWorkingDir() const
    {
        static const QString marker = QStringLiteral("- Working directory: ");
        const QString        blob   = QString::fromUtf8(wireLog());
        const int            at     = blob.lastIndexOf(marker);
        if (at < 0) {
            return {};
        }
        const int from = at + marker.size();
        /* The prompt is JSON-encoded inside the logged request, so the line
         * ends at an escaped newline rather than a real one. */
        const int end = blob.indexOf(QStringLiteral("\\n"), from);
        return end < 0 ? QString() : blob.mid(from, end - from);
    }

    /* Bounded wait on an observed condition. */
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

    /* Run one `qsoc agent -q` turn whose single tool call is path_context with
     * @p cwdArg, then wait for it to finish. */
    bool runOneCwdTurn(const QString &cwdArg);
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
    QString       m_escape;
    QString       m_requestLog;
    QString       m_home;
    QString       m_xdg;
};

#define REQUIRE_CWD_FIXTURE() \
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
    m_escape           = root + QStringLiteral("/elsewhere");
    m_requestLog       = root + QStringLiteral("/requests.jsonl");
    QDir().mkpath(m_home + QStringLiteral("/.ssh"));
    QDir().mkpath(m_xdg);
    QDir().mkpath(m_workspace + QStringLiteral("/sub"));
    QDir().mkpath(m_escape);

    /* An in-workspace name whose target is not in the workspace. Its spelling
     * passes a byte-prefix clamp; only the host can say where it goes. */
    if (!QFile::link(m_escape, m_workspace + QStringLiteral("/linkdir"))) {
        return fail(QStringLiteral("could not create the escaping symlink"));
    }

    QFile sshCfg(m_home + QStringLiteral("/.ssh/config"));
    if (!sshCfg.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    sshCfg.write(QStringLiteral(
                     "Host box\n"
                     "  HostName 127.0.0.1\n"
                     "  Port %1\n"
                     "  User %2\n"
                     "  IdentityFile %3\n"
                     "  IdentitiesOnly yes\n"
                     "  StrictHostKeyChecking no\n"
                     "  UserKnownHostsFile /dev/null\n")
                     .arg(m_fixture.port())
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

bool Test::runOneCwdTurn(const QString &cwdArg)
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
    mockEnv.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnv.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnv.insert(QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("path_context"));
    mockEnv.insert(
        QStringLiteral("MOCK_TOOL_ARGS"),
        QStringLiteral("{\"action\":\"cwd\",\"path\":\"%1\"}").arg(cwdArg));
    mockEnv.insert(QStringLiteral("MOCK_TOOL_MAX"), QStringLiteral("1"));
    mockEnv.insert(QStringLiteral("MOCK_REQUEST_LOG"), m_requestLog);

    m_mock.setProcessEnvironment(mockEnv);
    m_mock.setStandardOutputFile(m_dir.path() + QStringLiteral("/mock.log"));
    m_mock.setStandardErrorFile(m_dir.path() + QStringLiteral("/mock.err"));
    m_mock.start(
        QStandardPaths::findExecutable(QStringLiteral("python3")),
        {m_mockScript, QString::number(m_mockPort), QStringLiteral("none")});
    if (!m_mock.waitForStarted(5000)) {
        return false;
    }
    if (!waitFor(
            [this] {
                QTcpSocket probe;
                probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_mockPort));
                return probe.waitForConnected(200);
            },
            8000)) {
        return false;
    }

    m_agent.setProcessEnvironment(env);
    m_agent.setWorkingDirectory(m_dir.path());
    m_agent.setStandardOutputFile(m_dir.path() + QStringLiteral("/agent.out"));
    m_agent.setStandardErrorFile(m_dir.path() + QStringLiteral("/agent.err"));
    m_agent.start(
        m_qsoc,
        {QStringLiteral("agent"),
         QStringLiteral("--ssh"),
         QStringLiteral("box"),
         QStringLiteral("--workspace"),
         m_workspace,
         QStringLiteral("-q"),
         QStringLiteral("change the remote working directory")});
    if (!m_agent.waitForStarted(10000)) {
        return false;
    }
    /* Two requests: the one that gets the tool call, and the one that carries
     * its result. The second is the one whose system prompt has to name the
     * directory the tool moved to. */
    return waitFor([this] { return m_agent.state() == QProcess::NotRunning; }, 120000);
}

/*
 * A1. The working directory lives on the connection, and the agent config
 * carries a copy of it into the system prompt. A `path` tool call moved the
 * directory without touching the copy, so every later turn was told about a
 * directory the session had left.
 */
void Test::aPathToolCwdChangeIsWhatTheNextTurnIsTold()
{
    REQUIRE_CWD_FIXTURE();
    QVERIFY2(runOneCwdTurn(QStringLiteral("sub")), "the agent turn never completed");

    const QString declared = declaredWorkingDir();
    QVERIFY2(!declared.isEmpty(), "no request declared a remote working directory");
    QCOMPARE(declared, m_workspace + QStringLiteral("/sub"));
}

/*
 * A2. A directory reached through a symlink keeps its in-workspace spelling
 * while the host resolves it somewhere else. The clamp is a byte-prefix test on
 * that spelling, so the move was accepted and the working directory then named
 * a place outside the workspace. The harm is measured where the name resolves
 * to, not how it reads.
 */
void Test::aCwdReachingOutOfTheWorkspaceThroughALinkIsRefused()
{
    REQUIRE_CWD_FIXTURE();
    QVERIFY2(runOneCwdTurn(QStringLiteral("linkdir")), "the agent turn never completed");

    const QString declared = declaredWorkingDir();
    QVERIFY2(!declared.isEmpty(), "no request declared a remote working directory");

    const QString reached   = QFileInfo(declared).canonicalFilePath();
    const QString workspace = QFileInfo(m_workspace).canonicalFilePath();
    QVERIFY2(
        !reached.isEmpty(),
        qPrintable(QStringLiteral("the declared working directory does not resolve at all: %1")
                       .arg(declared)));
    QVERIFY2(
        reached == workspace || reached.startsWith(workspace + QStringLiteral("/")),
        qPrintable(QStringLiteral(
                       "the agent was told its working directory is %1, which the host "
                       "resolves to %2, outside the workspace %3")
                       .arg(declared, reached, workspace)));

    /* Only once the directory is known to be inside: the model also has to be
     * told the move did not happen, or it will build paths on the name it
     * asked for. */
    QVERIFY2(
        wireLog().contains("The working directory is unchanged"),
        "the model was not told the working-directory change was refused");
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotecwdowner.moc"
