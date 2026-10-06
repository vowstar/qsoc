// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotejobwatcher.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/runtime/qsocagentruntime.h"
#include "common/qsoctaskregistry.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * Remote background jobs watched over the bound session: a runtime binds a
 * loopback sshd with `/ssh`, and every poll is driven through pollOnce(), so
 * no case waits for the watcher's own timer.
 */

namespace {

constexpr auto kAlias = "watchhost";

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

/* The value of `key: ` in a tool result. */
QString field(const QString &text, const QString &key)
{
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(key + QStringLiteral(": "))) {
            return line.mid(key.size() + 2).trimmed();
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
    void init();
    void cleanup();
    void aFinishedJobReachesTheBusAndTheTaskList();
    void aStoppedJobIsNotReported();
    void aPollIsSkippedWhileTheSessionIsBusy();
    void aReconnectResumesWatching();
    void aRemoteMonitorRunsOverTheSession();
    void aSubAgentsRemoteMonitorNotifiesThatSubAgent();
    void aTaskTailComesFromTheLastPoll();

private:
    bool prepare();
    void bind();
    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QSocRemoteConnection *conn() const { return m_runtime->remoteConnection(); }
    QSocAgent            *agent() const { return m_runtime->agent(); }

    QString run(const QString &tool, const json &args) const
    {
        return m_runtime->toolRegistry()->executeTool(tool, args, agent());
    }

    /* Poll until @p done holds, the way the watcher's timer would. */
    bool pollUntil(const std::function<bool()> &done) const
    {
        for (int i = 0; i < 200 && !done(); ++i) {
            conn()->watcher()->pollOnce();
            QTest::qWait(50);
        }
        return done();
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

#define REQUIRE_WATCH_FIXTURE() \
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
    if (!QDir().mkpath(m_home + QStringLiteral("/.ssh")) || !QDir().mkpath(m_project)
        || !QDir().mkpath(m_workspace + QStringLiteral("/sub")) || !QDir().mkpath(runtimeDir)) {
        return fail(QStringLiteral("could not lay out the fixture tree"));
    }
    QFile::setPermissions(
        runtimeDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString sshConfig = QStringLiteral(
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
                                  .arg(m_fixture.user(), m_fixture.keyPath());
    if (!writeFile(m_home + QStringLiteral("/.ssh/config"), sshConfig.toUtf8())) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    /* A shim first on PATH: a monitor that still spawns the system ssh leaves
     * this marker behind. */
    const QString shim = m_dir.path() + QStringLiteral("/bin");
    if (!QDir().mkpath(shim)
        || !writeFile(
            shim + QStringLiteral("/ssh"),
            QStringLiteral("#!/bin/sh\ntouch '%1/ssh-was-spawned'\nexit 255\n")
                .arg(m_dir.path())
                .toUtf8())) {
        return fail(QStringLiteral("could not write the ssh shim"));
    }
    QFile::setPermissions(
        shim + QStringLiteral("/ssh"),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QList<QPair<QString, QByteArray>> env{
        {QStringLiteral("HOME"), m_home.toUtf8()},
        {QStringLiteral("QSOC_HOME"), m_home.toUtf8()},
        {QStringLiteral("XDG_CONFIG_HOME"), m_home.toUtf8()},
        {QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir.toUtf8()},
        {QStringLiteral("PATH"), shim.toUtf8() + ':' + qgetenv("PATH")},
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

/* Counterexample: a remote background job that ended on its own told nobody;
 * the model learned of it only by polling bash_manage. */
void Test::aFinishedJobReachesTheBusAndTheTaskList()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    QVERIFY(!conn()->watcher()->isActive());

    const QString launch = run(
        QStringLiteral("bash"),
        {{"command", "echo watched-output; sleep 0.3; exit 3"}, {"background", true}});
    const QString jobId = field(launch, QStringLiteral("job_id"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(launch));
    QVERIFY(conn()->watcher()->isActive());
    QCOMPARE(row(QStringLiteral("rbash"), jobId).row.status, QSocTask::Status::Running);

    const QString key = QStringLiteral("rbash/") + jobId;
    QVERIFY(pollUntil([&] { return agent()->hasQueuedNotification(key); }));
    QCOMPARE(row(QStringLiteral("rbash"), jobId).row.status, QSocTask::Status::Failed);
    QVERIFY(!conn()->watcher()->isActive());

    /* The notification carries the job's last lines; one per job. */
    QCOMPARE(agent()->pendingNotificationCount(), 1);
    const QString tail = m_runtime->taskRegistry()->tailFor(QStringLiteral("rbash"), jobId, 4000);
    QVERIFY2(tail.contains(QStringLiteral("watched-output")), qPrintable(tail));
    QCOMPARE(conn()->watcher()->pollOnce(), QSocRemoteJobWatcher::Poll::Idle);
    QCOMPARE(agent()->pendingNotificationCount(), 1);
}

/* A job its owner stopped is already known to the owner: no notification. */
void Test::aStoppedJobIsNotReported()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    const QString launch
        = run(QStringLiteral("bash"), {{"command", "sleep 30"}, {"background", true}});
    const QString jobId = field(launch, QStringLiteral("job_id"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(launch));
    const QString stopped = run(
        QStringLiteral("bash_manage"), {{"job_id", jobId.toStdString()}, {"action", "terminate"}});
    QVERIFY2(stopped.contains(QStringLiteral("signal_sent: SIGTERM")), qPrintable(stopped));
    QVERIFY(pollUntil([&] {
        return row(QStringLiteral("rbash"), jobId).row.status == QSocTask::Status::Aborted;
    }));
    QCOMPARE(agent()->pendingNotificationCount(), 0);
}

/* A connect, an authentication prompt or a reconnect pumps the event loop; a
 * tick that lands inside one must leave the session alone. */
void Test::aPollIsSkippedWhileTheSessionIsBusy()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    const QString launch
        = run(QStringLiteral("bash"), {{"command", "sleep 30"}, {"background", true}});
    QVERIFY2(!field(launch, QStringLiteral("job_id")).isEmpty(), qPrintable(launch));
    QSocRemoteJobWatcher *watcher = conn()->watcher();
    const int             before  = watcher->skippedPolls();
    {
        const QSocSshSession::Operation exec(*conn()->session());
        QCOMPARE(watcher->pollOnce(), QSocRemoteJobWatcher::Poll::Skipped);
    }
    {
        const QSocRemoteConnection::Operation connecting(*conn());
        QCOMPARE(watcher->pollOnce(), QSocRemoteJobWatcher::Poll::Skipped);
    }
    QCOMPARE(watcher->skippedPolls(), before + 2);
    QCOMPARE(watcher->pollOnce(), QSocRemoteJobWatcher::Poll::Polled);
    QVERIFY(watcher->isActive());
}

/* The rebuilder runs inside reconnect(); a tick there finds the transport half
 * built and is skipped. Afterwards the ledger is kept and watching resumes. */
void Test::aReconnectResumesWatching()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    const QString launch = run(
        QStringLiteral("bash"),
        {{"command", "sleep 1; echo after-reconnect; exit 0"}, {"background", true}});
    const QString jobId = field(launch, QStringLiteral("job_id"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(launch));

    QSocRemoteJobWatcher::Poll inside = QSocRemoteJobWatcher::Poll::Polled;
    conn()->setRebuilder([&](const QString    &target,
                             const QString    &workspace,
                             AgentRemoteState *out,
                             QString          *errorMessage,
                             QDeadlineTimer    deadline) {
        inside = conn()->watcher()->pollOnce();
        AgentRemoteState fresh;
        if (!connectAgentSshSession(target, nullptr, &fresh, errorMessage, {}, {}, deadline)
            || !prepareAgentRemoteWorkspace(workspace, &fresh, errorMessage, deadline)) {
            discardAgentRemoteState(&fresh);
            return false;
        }
        *out = fresh;
        return true;
    });
    conn()->session()->markTransportDead();
    QCOMPARE(conn()->watcher()->pollOnce(), QSocRemoteJobWatcher::Poll::Skipped);
    QString error;
    QCOMPARE(conn()->reconnect(&error), QSocRemoteConnection::ReconnectOutcome::Reconnected);
    QCOMPARE(inside, QSocRemoteJobWatcher::Poll::Skipped);
    QVERIFY(conn()->watcher()->isActive());

    const QString key = QStringLiteral("rbash/") + jobId;
    QVERIFY(pollUntil([&] { return agent()->hasQueuedNotification(key); }));
    QCOMPARE(row(QStringLiteral("rbash"), jobId).row.status, QSocTask::Status::Completed);
}

/* Counterexample: a remote monitor spawned the system ssh binary, with its own
 * auth stack and host key handling, and ran from the workspace root. */
void Test::aRemoteMonitorRunsOverTheSession()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    QVERIFY(run(QStringLiteral("path_context"), {{"action", "cwd"}, {"path", "sub"}})
                .contains(QStringLiteral("/sub")));

    QStringList lines;
    QString     terminal;
    connect(
        m_runtime->taskEventQueue(),
        &QSocTaskEventQueue::taskEventQueued,
        this,
        [&](const QSocTaskEvent &event) {
            if (event.sourceTag != QStringLiteral("monitor")) {
                return;
            }
            if (event.kind == QStringLiteral("monitor_line")) {
                lines << event.content;
            } else if (event.kind == QStringLiteral("task_notification")) {
                terminal = event.status;
            }
        });
    const QString started = run(
        QStringLiteral("monitor"),
        {{"command", "pwd; echo oops >&2; for i in 1 2 3; do echo line$i; sleep 0.2; done"},
         {"description", "count"}});
    QVERIFY2(started.contains(QStringLiteral("\"started\"")), qPrintable(started));
    QVERIFY(conn()->watcher()->isActive());
    QCOMPARE(conn()->watcher()->intervalMs(), QSocRemoteJobWatcher::kFollowIntervalMs);

    QVERIFY(pollUntil([&] { return !terminal.isEmpty(); }));
    QCOMPARE(terminal, QStringLiteral("completed"));
    /* stderr is tagged as a local monitor tags it. */
    QVERIFY2(lines.removeOne(QStringLiteral("[stderr] oops")), qPrintable(lines.join('|')));
    QCOMPARE(lines.size(), 4);
    QVERIFY2(lines.first().endsWith(QStringLiteral("/ws/sub")), qPrintable(lines.join('|')));
    QCOMPARE(lines.mid(1), QStringList({"line1", "line2", "line3"}));
    QVERIFY(!QFile::exists(m_dir.path() + QStringLiteral("/ssh-was-spawned")));
}

/* Counterexample: a sub-agent's remote monitor reported to main. */
void Test::aSubAgentsRemoteMonitorNotifiesThatSubAgent()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    auto *mailbox = m_runtime->subAgentSource()->mailbox();
    auto *peer    = new QSocAgent(m_runtime.get(), nullptr, nullptr);
    mailbox->registerAgent(peer, QStringLiteral("peer"));
    const QString started = m_runtime->toolRegistry()->executeTool(
        QStringLiteral("monitor"), {{"command", "echo CHILD_LINE"}, {"description", "child"}}, peer);
    QVERIFY2(started.contains(QStringLiteral("\"started\"")), qPrintable(started));
    QVERIFY(pollUntil([&] { return peer->pendingNotificationCount() > 0; }));
    QCOMPARE(agent()->pendingNotificationCount(), 0);
}

/* Counterexample: opening a remote row ran an SSH exec on the UI path, and
 * while the session was busy the row read "(no output yet)". */
void Test::aTaskTailComesFromTheLastPoll()
{
    REQUIRE_WATCH_FIXTURE();
    bind();
    const QString launch = run(
        QStringLiteral("bash"), {{"command", "echo early-output; sleep 30"}, {"background", true}});
    const QString jobId = field(launch, QStringLiteral("job_id"));
    QVERIFY2(!jobId.isEmpty(), qPrintable(launch));
    QVERIFY(pollUntil([&] { return conn()->watcher()->savedTail(jobId).contains("early-output"); }));
    const QSocSshSession::Operation busy(*conn()->session());
    const QString tail = m_runtime->taskRegistry()->tailFor(QStringLiteral("rbash"), jobId, 4000);
    QVERIFY2(tail.contains(QStringLiteral("early-output")), qPrintable(tail));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotejobwatcher.moc"
