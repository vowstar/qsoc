// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotejobwatcher.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/runtime/qsocagentruntime.h"
#include "agent/tool/qsoctoolshell.h"
#include "common/qsocboundedcapture.h"
#include "common/qsoctaskregistry.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#ifdef Q_OS_UNIX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

/*
 * A remote command or transfer the user stops: Esc must reach it while it
 * runs, and a command that outlives its timeout must stay in reach as a job.
 * The loopback sshd serves every case; the throttled relay stands in for a
 * slow link, so a transfer is still running when the stop arrives.
 */

namespace {

constexpr auto kAlias = "aborthost";

/* When the probe starts saying stop, measured from the call starting. */
constexpr qint64 kStopAfterMs = 300;

/* What a stop may cost: a 200 ms slice, the poll in flight, and noise. */
constexpr qint64 kStopLatencyMs = 1000;

QString field(const QString &text, const QString &key)
{
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(key + QStringLiteral(": "))) {
            return line.mid(key.size() + 2).trimmed();
        }
    }
    return {};
}

#ifdef Q_OS_UNIX
/* One TCP connection relayed to the sshd, slowed on the way back. */
class SlowRelay
{
public:
    explicit SlowRelay(int target)
        : m_target(target)
    {}

    ~SlowRelay()
    {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_listen >= 0) {
            ::close(m_listen);
        }
    }

    SlowRelay(const SlowRelay &)            = delete;
    SlowRelay &operator=(const SlowRelay &) = delete;

    quint16 start()
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length     = sizeof(addr);
        if (m_listen < 0 || ::bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0
            || ::listen(m_listen, 1) != 0
            || ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&addr), &length) != 0) {
            return 0;
        }
        m_thread = std::thread([this] { run(); });
        return ntohs(addr.sin_port);
    }

private:
    static bool forward(int from, int to, bool slow)
    {
        char          buffer[4096];
        const ssize_t n = ::read(from, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        if (slow) {
            ::usleep(20000);
        }
        for (ssize_t sent = 0; sent < n;) {
            const ssize_t w = ::write(to, buffer + sent, static_cast<size_t>(n - sent));
            if (w <= 0) {
                return false;
            }
            sent += w;
        }
        return true;
    }

    void run()
    {
        pollfd accepting{m_listen, POLLIN, 0};
        while (!m_stop && ::poll(&accepting, 1, 100) == 0) {
        }
        if (m_stop) {
            return;
        }
        const int   client = ::accept(m_listen, nullptr, nullptr);
        const int   server = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(static_cast<quint16>(m_target));
        if (client >= 0 && server >= 0
            && ::connect(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            pollfd fds[2] = {{client, POLLIN, 0}, {server, POLLIN, 0}};
            while (!m_stop) {
                if (::poll(fds, 2, 100) <= 0) {
                    continue;
                }
                if (((fds[0].revents & (POLLIN | POLLHUP)) && !forward(client, server, false))
                    || ((fds[1].revents & (POLLIN | POLLHUP)) && !forward(server, client, true))) {
                    break;
                }
            }
        }
        if (client >= 0) {
            ::close(client);
        }
        if (server >= 0) {
            ::close(server);
        }
    }

    int               m_target = 0;
    int               m_listen = -1;
    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
};
#endif

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void bashAndBashManageTakeOneSchema();
    void anExecStopsWithinASliceOfTheStop();
    void anSftpReadStopsMidStream();
    void anAbortedCommandStopsItsJob();
    void aTimedOutCommandBecomesAJob();
    void aTimedOutCommandCanBeKilled();
    void aTaskKillDoesNotWaitForTheLink();
    void aShellEscapeKeepsBoundedOutput();
    void aCommandCostsOneExecAndNoSftp();
    void aReplacedJobsRootIsVerifiedAgain();

private:
    bool prepare();
    void bind();
    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QSocRemoteConnection *conn() const { return m_runtime->remoteConnection(); }

    QString run(const QString &tool, const json &args) const
    {
        return m_runtime->toolRegistry()->executeTool(tool, args, m_runtime->agent());
    }

    /* A session on the fixture, with a probe that says stop after kStopAfterMs. */
    std::unique_ptr<QSocSshSession> stoppingSession(QElapsedTimer *clock, quint16 port = 0) const
    {
        auto       session = std::make_unique<QSocSshSession>();
        const auto host    = port == 0 ? m_fixture.hostConfig() : m_fixture.hostConfig(port);
        if (session->connectTo(host, nullptr) != QSocSshSession::ConnectStatus::Ok) {
            return nullptr;
        }
        session->setAbortProbe(
            [clock] { return clock->isValid() && clock->elapsed() >= kStopAfterMs; });
        return session;
    }

    /* Session operations @p body begins: SFTP ones, and the rest (execs). */
    struct Cost
    {
        quint64 sftp = 0;
        quint64 exec = 0;
    };
    Cost costOf(const std::function<void()> &body) const
    {
        const quint64 sftpBefore    = conn()->sftp()->operationsBegun();
        const quint64 sessionBefore = conn()->session()->operationsBegun();
        body();
        Cost cost;
        cost.sftp = conn()->sftp()->operationsBegun() - sftpBefore;
        cost.exec = conn()->session()->operationsBegun() - sessionBefore - cost.sftp;
        return cost;
    }

    QSocTaskRegistry::TaggedRow row(const QString &tag, const QString &id) const
    {
        for (const auto &tagged : m_runtime->taskRegistry()->listAll()) {
            if (tagged.sourceTag == tag && tagged.row.id == id) {
                return tagged;
            }
        }
        return {};
    }

    bool pollUntil(const std::function<bool()> &done) const
    {
        for (int i = 0; i < 200 && !done(); ++i) {
            conn()->watcher()->pollOnce();
            QTest::qWait(50);
        }
        return done();
    }

    QSocTestSshd                      m_fixture;
    QTemporaryDir                     m_dir;
    std::unique_ptr<QSocAgentRuntime> m_runtime;
    bool                              m_ready = false;
    QString                           m_failure;
    QString                           m_home;
    QString                           m_project;
    QString                           m_workspace;
    QStringList                       m_savedNames;
    QList<QByteArray>                 m_savedValues;
    QList<bool>                       m_savedSet;
};

#define REQUIRE_ABORT_FIXTURE() \
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
        return;
    }
    m_ready = prepare();
}

bool Test::prepare()
{
    if (!m_dir.isValid()) {
        return fail(QStringLiteral("temporary directory: %1").arg(m_dir.errorString()));
    }
    m_home                   = m_dir.path() + QStringLiteral("/home");
    m_project                = m_dir.path() + QStringLiteral("/project");
    m_workspace              = m_fixture.workDir() + QStringLiteral("/ws");
    const QString runtimeDir = m_dir.path() + QStringLiteral("/run");
    if (!QDir().mkpath(m_project) || !QDir().mkpath(m_workspace) || !QDir().mkpath(runtimeDir)) {
        return fail(QStringLiteral("could not lay out the fixture tree"));
    }
    QFile::setPermissions(
        runtimeDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (!m_fixture.writeClientSshConfig(m_home, QString::fromLatin1(kAlias))) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    const QList<QPair<QString, QByteArray>> env{
        {QStringLiteral("HOME"), m_home.toUtf8()},
        {QStringLiteral("QSOC_HOME"), m_home.toUtf8()},
        {QStringLiteral("XDG_CONFIG_HOME"), m_home.toUtf8()},
        {QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir.toUtf8()},
    };
    for (const auto &[name, value] : env) {
        m_savedNames << name;
        m_savedSet << qEnvironmentVariableIsSet(qPrintable(name));
        m_savedValues << qgetenv(qPrintable(name));
        if (!qputenv(qPrintable(name), value)) {
            return fail(QStringLiteral("could not redirect %1").arg(name));
        }
    }
    return true;
}

void Test::cleanupTestCase()
{
    for (int i = 0; i < m_savedNames.size(); ++i) {
        const QByteArray name = m_savedNames.at(i).toUtf8();
        m_savedSet.at(i) ? qputenv(name.constData(), m_savedValues.at(i))
                         : qunsetenv(name.constData());
    }
    m_fixture.stop();
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

void Test::init()
{
    if (!m_ready) {
        return;
    }
    QSocAgentRuntimeOptions options;
    options.projectDirectory = m_project;
    options.workspace        = m_workspace;
    m_runtime                = std::make_unique<QSocAgentRuntime>(options);
    QVERIFY(m_runtime->openSession());
}

void Test::cleanup()
{
    if (m_runtime) {
        m_runtime->disconnectRemote();
        m_runtime.reset();
    }
}

void Test::bind()
{
    QString error;
    QVERIFY2(
        m_runtime->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = m_workspace, .remember = false},
            &error),
        qPrintable(error));
}

/* Counterexample: the same tool name took `timeout` locally and `timeout_ms`
 * remotely, and remote bash_manage had no `wait`. Only the id differs: a local
 * process id is a number, a remote job id a string. */
void Test::bashAndBashManageTakeOneSchema()
{
    QSocToolShellBash        localBash;
    QSocToolBashManage       localManage;
    QSocToolRemoteShellBash  remoteBash(nullptr, nullptr, nullptr);
    QSocToolRemoteBashManage remoteManage(nullptr, nullptr, nullptr);
    QCOMPARE(remoteBash.getParametersSchema(), localBash.getParametersSchema());

    json local  = localManage.getParametersSchema();
    json remote = remoteManage.getParametersSchema();
    QCOMPARE(local["properties"]["process_id"]["type"], json("integer"));
    QCOMPARE(remote["properties"]["job_id"]["type"], json("string"));
    local["properties"].erase("process_id");
    remote["properties"].erase("job_id");
    local.erase("required");
    remote.erase("required");
    QCOMPARE(remote, local);
}

/* Counterexample: an exec polled its socket and nothing else, so a stop the
 * user asked for was read only when the command ended or timed out. */
void Test::anExecStopsWithinASliceOfTheStop()
{
    REQUIRE_ABORT_FIXTURE();
    QElapsedTimer clock;
    auto          session = stoppingSession(&clock);
    QVERIFY(session != nullptr);
    QSocSshExec exec(*session);
    clock.start();
    const auto   result  = exec.run(QStringLiteral("sleep 30"), 10000);
    const qint64 elapsed = clock.elapsed();
    QVERIFY2(result.aborted, qPrintable(result.errorText));
    QVERIFY(!result.timedOut);
    QVERIFY2(elapsed < kStopLatencyMs, qPrintable(QString::number(elapsed)));

    /* The probe still says stop; it said so before this call began, so the
     * session serves the next command. */
    QVERIFY(session->isConnected());
    const auto next = QSocSshExec(*session).run(QStringLiteral("echo still-usable"), 5000);
    QCOMPARE(next.exitCode, 0);
    QCOMPARE(next.stdoutBytes, QByteArray("still-usable\n"));
}

/* Counterexample: an SFTP read waited on its socket alone, so a large file
 * over a slow link could not be stopped. */
void Test::anSftpReadStopsMidStream()
{
    REQUIRE_ABORT_FIXTURE();
#ifndef Q_OS_UNIX
    QSKIP("The relay uses POSIX sockets");
#else
    const QString big = m_fixture.workDir() + QStringLiteral("/big.bin");
    {
        QFile file(big);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(QByteArray(16 * 1024 * 1024, 'x')) == 16 * 1024 * 1024);
    }
    SlowRelay     relay(m_fixture.port());
    const quint16 port = relay.start();
    QVERIFY(port != 0);
    QElapsedTimer clock;
    auto          session = stoppingSession(&clock, port);
    QVERIFY(session != nullptr);
    QSocSftpClient sftp(*session);
    sftp.setOperationTimeoutMs(60000);
    qint64  received = 0;
    QString error;
    clock.start();
    const bool ok = sftp.readStream(
        big,
        [&received](const QByteArray &chunk) {
            received += chunk.size();
            return true;
        },
        &error);
    const qint64 elapsed = clock.elapsed();
    QVERIFY2(!ok, "the read ran to the end");
    QVERIFY2(received > 0 && received < 16 * 1024 * 1024, qPrintable(QString::number(received)));
    QVERIFY2(elapsed < kStopLatencyMs + 1000, qPrintable(QString::number(elapsed)));
    QVERIFY2(error.contains(QStringLiteral("Interrupted")), qPrintable(error));
    QFile::remove(big);
#endif
}

/* Counterexample: Esc during a remote command was not seen until the command
 * ended, and the command kept running on the host after the call gave up. */
void Test::anAbortedCommandStopsItsJob()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString childFile = m_workspace + QStringLiteral("/child.pid");
    QFile::remove(childFile);
    QTimer::singleShot(kStopAfterMs, m_runtime.get(), [this] { m_runtime->abort(); });
    QElapsedTimer clock;
    clock.start();
    const QString result = run(
        QStringLiteral("bash"),
        {{"command", "sleep 30 & echo $! > child.pid; wait"}, {"timeout", 20000}});
    const qint64 elapsed = clock.elapsed();
    QCOMPARE(result, QStringLiteral("Command aborted."));
    QVERIFY2(elapsed < kStopLatencyMs + 1000, qPrintable(QString::number(elapsed)));
    QVERIFY(conn()->isUsable());
    QVERIFY(conn()->jobs()->liveJobIds().isEmpty());
#ifdef Q_OS_LINUX
    /* What the command started is stopped with it. */
    QFile child(childFile);
    QVERIFY(child.open(QIODevice::ReadOnly));
    const QString proc = QStringLiteral("/proc/") + QString::fromLatin1(child.readAll()).trimmed();
    QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(proc), 3000);
#endif
    const QString after = run(QStringLiteral("bash"), {{"command", "echo next"}});
    QVERIFY2(after.startsWith(QStringLiteral("status: ok\nexit_code: 0\nnext\n")), qPrintable(after));
}

/* Counterexample: a remote command that hit its timeout had its channel
 * closed and kept running on the host with no id to reach it by. */
void Test::aTimedOutCommandBecomesAJob()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString timedOut
        = run(QStringLiteral("bash"), {{"command", "sleep 2; echo done-late"}, {"timeout", 500}});
    QVERIFY2(timedOut.startsWith(QStringLiteral("status: dispatched\n")), qPrintable(timedOut));
    QVERIFY2(timedOut.contains(QStringLiteral("STILL RUNNING")), qPrintable(timedOut));
    const QString jobId = field(timedOut, QStringLiteral("Job ID"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(timedOut));
    QVERIFY(conn()->jobs()->liveJobIds().contains(jobId));

    const QString waited = run(
        QStringLiteral("bash_manage"),
        {{"job_id", jobId.toStdString()}, {"action", "wait"}, {"timeout", 10000}});
    QVERIFY2(waited.contains(QStringLiteral("exit_code: 0\n")), qPrintable(waited));
    QVERIFY2(waited.contains(QStringLiteral("done-late")), qPrintable(waited));
    QVERIFY(!conn()->jobs()->liveJobIds().contains(jobId));
}

void Test::aTimedOutCommandCanBeKilled()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString timedOut
        = run(QStringLiteral("bash"), {{"command", "sleep 30"}, {"timeout", 300}});
    const QString jobId = field(timedOut, QStringLiteral("Job ID"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(timedOut));
    const QString killed
        = run(QStringLiteral("bash_manage"), {{"job_id", jobId.toStdString()}, {"action", "kill"}});
    QVERIFY2(killed.contains(QStringLiteral("signal_sent: SIGKILL")), qPrintable(killed));
    const QString status = run(
        QStringLiteral("bash_manage"),
        {{"job_id", jobId.toStdString()}, {"action", "wait"}, {"timeout", 5000}});
    QVERIFY2(status.contains(QStringLiteral("exit_code: ")), qPrintable(status));
}

/* Counterexample: Ctrl+B kill ran an exec of up to 5 s on the UI path, and
 * refused outright while the session was busy. */
void Test::aTaskKillDoesNotWaitForTheLink()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString launch
        = run(QStringLiteral("bash"), {{"command", "sleep 30"}, {"background", true}});
    const QString jobId = field(launch, QStringLiteral("job_id"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(launch));
    {
        const QSocSshSession::Operation busy(*conn()->session());
        QElapsedTimer                   clock;
        clock.start();
        QVERIFY(m_runtime->taskRegistry()->killTask(QStringLiteral("rbash"), jobId));
        QVERIFY2(clock.elapsed() < 100, qPrintable(QString::number(clock.elapsed())));
    }
    QVERIFY(pollUntil([&] {
        return row(QStringLiteral("rbash"), jobId).row.status == QSocTask::Status::Aborted;
    }));
}

/* Counterexample: a remote `!` line kept all its output and stopped at 30 s. */
void Test::aShellEscapeKeepsBoundedOutput()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString out
        = runBoundRemoteShellEscape(conn(), QStringLiteral("yes | head -c 12000000; echo LAST-LINE"));
    QVERIFY(QSocBoundedCapture::isElided(out));
    QVERIFY2(
        out.toUtf8().size() <= QSocBoundedCapture::kDefaultLimit + 512, qPrintable(out.right(80)));
    QVERIFY2(out.contains(QStringLiteral("LAST-LINE")), qPrintable(out.right(120)));
}

/* Counterexample: every foreground command verified its job directory over
 * SFTP first, about ten round trips before the command even started. Once a
 * binding has verified its jobs root, a command is one exec and nothing more
 * than resolving the working directory, as before commands ran as jobs. */
void Test::aCommandCostsOneExecAndNoSftp()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    QString    cwd;
    const auto cwdCost = costOf([&] { QVERIFY(conn()->resolveBoundCwd(&cwd)); });
    QVERIFY2(run(QStringLiteral("bash"), {{"command", "true"}}).contains("exit_code: 0"), "warm-up");

    QString    result;
    const auto cost = costOf(
        [&] { result = run(QStringLiteral("bash"), {{"command", "echo counted"}}); });
    QVERIFY2(result.contains(QStringLiteral("counted")), qPrintable(result));
    qInfo("per call: %llu exec, %llu SFTP (cwd alone %llu)", cost.exec, cost.sftp, cwdCost.sftp);
    QCOMPARE(cost.exec, quint64{1});
    QCOMPARE(cost.sftp, cwdCost.sftp);
}

/* The cached root is trusted only while the host still shows a plain
 * directory there: a removed root is created again, a root replaced by a link
 * is never launched into. */
void Test::aReplacedJobsRootIsVerifiedAgain()
{
    REQUIRE_ABORT_FIXTURE();
    bind();
    const QString jobs = m_workspace + QStringLiteral("/.qsoc-agent/jobs");
    QVERIFY(run(QStringLiteral("bash"), {{"command", "true"}}).contains("exit_code: 0"));
    QVERIFY(QDir(jobs).exists());

    QVERIFY(QDir(jobs).removeRecursively());
    const QString recreated = run(QStringLiteral("bash"), {{"command", "echo again"}});
    QVERIFY2(
        recreated.startsWith(QStringLiteral("status: ok\nexit_code: 0\nagain\n")),
        qPrintable(recreated));
    QVERIFY(QDir(jobs).exists());

    QTemporaryDir elsewhere;
    QVERIFY(QDir(jobs).removeRecursively());
    QVERIFY(QFile::link(elsewhere.path(), jobs));
    const QString linked = run(QStringLiteral("bash"), {{"command", "echo attached"}});
    QVERIFY2(linked.contains(QStringLiteral("attached")), qPrintable(linked));
    QVERIFY(QDir(elsewhere.path()).isEmpty());
    QVERIFY(QFile::remove(jobs));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteexecabort.moc"
