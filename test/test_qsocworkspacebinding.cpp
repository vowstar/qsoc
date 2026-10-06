// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/qsocagent.h"
#include "agent/qsocsession.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

/*
 * Project state belongs to the workspace binding that created it: a resumed
 * session, the goal, /loop tasks, the hook payload, the project rules and the
 * Environment section. Every case binds a loopback sshd through the
 * production runtime, so the remote workspace is a directory of this
 * filesystem and the session files, goal and loops are written at runtime.
 */

namespace {

constexpr auto kAlias = "bind-box";

bool spill(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(content) == content.size();
}

QByteArray slurp(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

template<typename Predicate>
bool waitFor(Predicate ready, int timeoutMs)
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

/* Chat-completions endpoint that answers every request with one final reply
 * and counts the requests. */
class MockLlm final : public QObject
{
public:
    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost);
    }

    int  port() const { return m_server.serverPort(); }
    int  requests() const { return m_requests; }
    void resetRequests() { m_requests = 0; }

private:
    void consume(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        qsizetype length = 0;
        for (QByteArray line : buffer.left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
            }
        }
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        m_buffers.remove(socket);
        ++m_requests;
        const QByteArray body = QByteArrayLiteral(
            "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n");
        socket->write(
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Connection: close\r\nContent-Length: ")
            + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QTcpServer                      m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    int                             m_requests = 0;
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aRemoteSessionResumedLocallyOffersToRebind();
    void continuingHereLeavesToolsOpen();
    void cancellingTheOfferKeepsTheCurrentSession();
    void aSingleQueryResumeRefusesToolsUntilTheUserChooses();
    void aGoalSetOnARemoteBindingWaitsAfterLocal();
    void aGoalSetLocallyStillContinues();
    void aLoopScheduledOnARemoteBindingWaitsForIt();
    void theHookPayloadNamesTheAliasAndTheProject();
    void projectRulesReloadWhenTheLinkComesBack();
    void theRemoteEnvironmentSaysWhetherTheWorkspaceIsGit();

private:
    QString root() const { return m_dir.path(); }
    QString home() const { return root() + QStringLiteral("/home"); }
    QString configHome() const { return root() + QStringLiteral("/config"); }
    QString remote(const QString &name) const
    {
        const QString dir = m_fixture.workDir() + QLatin1Char('/') + name;
        QDir().mkpath(dir);
        return dir;
    }

    QString project(const QString &name) const
    {
        const QString dir = root() + QStringLiteral("/projects/") + name;
        QDir().mkpath(dir + QStringLiteral("/.qsoc"));
        return dir;
    }

    std::unique_ptr<QSocAgentRuntime> runtime(const QString &projectDir, bool singleQuery = false)
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = projectDir;
        options.singleQuery      = singleQuery;
        auto made                = std::make_unique<QSocAgentRuntime>(options);
        connect(
            made.get(),
            &QSocAgentRuntime::eventRaised,
            this,
            [this](const QSocAgentRuntimeEvent &event) {
                if (event.kind == QSocAgentRuntimeEvent::Kind::Output) {
                    m_output += event.text;
                }
            });
        /* Every other menu answers its first row. */
        made->setMenuHandler([this](const QString &title, const auto &, const auto &, const auto &) {
            if (title.startsWith(QStringLiteral("This session last ran on"))) {
                ++m_offers;
                return m_offerAnswer;
            }
            return 0;
        });
        m_output.clear();
        m_offers = 0;
        return made;
    }

    bool bind(QSocAgentRuntime *session, const QString &workspace)
    {
        QString    err;
        const bool ok = session->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = workspace, .remember = false},
            &err);
        if (!ok) {
            qWarning("connectRemote: %s", qPrintable(err));
        }
        return ok;
    }

    /* A session file whose last run was on kAlias:@p workspace. */
    QString remoteSession(const QString &projectDir, const QString &workspace) const
    {
        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(projectDir)).filePath(id + ".jsonl");
        QDir().mkpath(QSocSession::sessionsDir(projectDir));
        QSocSession            file(id, path, QSocSession::StorageMode::Fresh);
        QSocSession::RunRecord run;
        run.runId          = QStringLiteral("run-1");
        run.event          = QSocSession::RunEvent::Started;
        run.messageCount   = 0;
        run.historyDigest  = QSocSession::historyDigest(nlohmann::json::array());
        run.contextPresent = true;
        run.modelId        = QStringLiteral("mock");
        run.remoteMode     = true;
        run.remoteName     = QString::fromLatin1(kAlias);
        run.projectRoot    = workspace;
        run.workingDir     = workspace;
        if (!file.appendMeta(QStringLiteral("cwd"), projectDir) || !file.appendRun(run)) {
            return {};
        }
        run.event = QSocSession::RunEvent::Completed;
        return file.appendRun(run) ? id : QString();
    }

    void writeConfig(int port)
    {
        QVERIFY(spill(
            configHome() + QStringLiteral("/qsoc/qsoc.yml"),
            QStringLiteral(
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
                "agent:\n"
                "  max_iterations: 3\n"
                "  session_title: false\n"
                "  away_summary: false\n"
                "  memory_extract: false\n"
                "  memory_dream: false\n"
                "  memory_recall: false\n"
                "proxy:\n"
                "  type: none\n")
                .arg(port)
                .toUtf8()));
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    MockLlm       m_llm;
    QString       m_output;
    int           m_offers      = 0;
    int           m_offerAnswer = 0;
};

void Test::initTestCase()
{
    qRegisterMetaType<QSocAgentRuntimeEvent>("QSocAgentRuntimeEvent");
    m_fixture.start();
    QVERIFY(m_dir.isValid());
    QVERIFY(m_llm.listen());
    const QString data = root() + QStringLiteral("/data");
    for (const QString &dir :
         {home() + QStringLiteral("/.ssh"), configHome() + QStringLiteral("/qsoc"), data}) {
        QVERIFY(QDir().mkpath(dir));
    }
    QVERIFY(qputenv("HOME", home().toUtf8()));
    QVERIFY(qputenv("XDG_CONFIG_HOME", configHome().toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", data.toUtf8()));
    QVERIFY(qputenv("QSOC_HOME", (configHome() + QStringLiteral("/qsoc")).toUtf8()));
    QVERIFY(qputenv("NO_PROXY", "*"));
    QVERIFY(qputenv("no_proxy", "*"));
    QVERIFY(spill(
        home() + QStringLiteral("/.ssh/config"),
        QStringLiteral(
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
            .toUtf8()));
    QFile::setPermissions(
        home() + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    writeConfig(m_llm.port());
}

void Test::cleanupTestCase()
{
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so destructors would not clean up. */
    QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Counterexample: /resume of a session that ran on a remote workspace opened
 * it on the local binding with no word, and its remote paths went to local
 * tools. */
void Test::aRemoteSessionResumedLocallyOffersToRebind()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir       = project(QStringLiteral("rebind"));
    const QString workspace = remote(QStringLiteral("rebind"));
    const QString id        = remoteSession(dir, workspace);
    QVERIFY(!id.isEmpty());

    auto session  = runtime(dir);
    m_offerAnswer = 0;
    QVERIFY2(session->openSessionById(id), qPrintable(session->lastError()));
    QCOMPARE(m_offers, 1);
    QVERIFY2(session->isRemote(), qPrintable(m_output));
    QCOMPARE(session->remoteWorkspace(), workspace);
    QCOMPARE(session->sessionId(), id);
    QVERIFY2(session->agent()->probeWorkspaceHealth().isEmpty(), qPrintable(m_output));
    session->disconnectRemote();
}

void Test::continuingHereLeavesToolsOpen()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir = project(QStringLiteral("keep"));
    const QString id  = remoteSession(dir, remote(QStringLiteral("keep")));
    QVERIFY(!id.isEmpty());

    auto session  = runtime(dir);
    m_offerAnswer = 1;
    QVERIFY2(session->openSessionById(id), qPrintable(session->lastError()));
    QCOMPARE(m_offers, 1);
    QVERIFY(!session->isRemote());
    QCOMPARE(session->sessionId(), id);
    QVERIFY2(session->agent()->probeWorkspaceHealth().isEmpty(), qPrintable(m_output));
}

void Test::cancellingTheOfferKeepsTheCurrentSession()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir = project(QStringLiteral("cancel"));
    const QString id  = remoteSession(dir, remote(QStringLiteral("cancel")));
    QVERIFY(!id.isEmpty());

    auto session = runtime(dir);
    QVERIFY(session->openSession());
    const QString before = session->sessionId();
    m_offerAnswer        = 2;
    QVERIFY(!session->openSessionById(id));
    QCOMPARE(m_offers, 1);
    QCOMPARE(session->sessionId(), before);
    QVERIFY(!session->isRemote());
}

/* Nobody can answer a menu in -q, so the stale binding is stated and every
 * tool call is refused until a binding command decides. */
void Test::aSingleQueryResumeRefusesToolsUntilTheUserChooses()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir       = project(QStringLiteral("query"));
    const QString workspace = remote(QStringLiteral("query"));
    const QString id        = remoteSession(dir, workspace);
    QVERIFY(!id.isEmpty());

    auto session = runtime(dir, /*singleQuery=*/true);
    QVERIFY2(session->openSessionById(id), qPrintable(session->lastError()));
    QCOMPARE(m_offers, 0);
    QVERIFY(!session->isRemote());
    const QString label = QStringLiteral("%1:%2").arg(QString::fromLatin1(kAlias), workspace);
    QVERIFY2(m_output.contains(label), qPrintable(m_output));
    const QString fence = session->agent()->probeWorkspaceHealth();
    QVERIFY2(fence.contains(QStringLiteral("Tool calls are refused")), qPrintable(fence));

    QVERIFY(session->executeCommand(QStringLiteral("/local")));
    QVERIFY2(session->agent()->probeWorkspaceHealth().isEmpty(), "/local did not decide");
}

/* Counterexample: a goal set on a remote binding kept continuing after /local
 * and drove the local tree. */
void Test::aGoalSetOnARemoteBindingWaitsAfterLocal()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString workspace = remote(QStringLiteral("goal"));
    auto          session   = runtime(project(QStringLiteral("goal")));
    QVERIFY(session->openSession());
    QVERIFY(bind(session.get(), workspace));
    QVERIFY(session->executeCommand(QStringLiteral("/goal port the bus")));
    QVERIFY(session->executeCommand(QStringLiteral("/local")));

    m_output.clear();
    m_llm.resetRequests();
    session->runTurn(QStringLiteral("hello"));
    QCOMPARE(m_llm.requests(), 1);
    QVERIFY2(m_output.contains(QStringLiteral("Goal paused")), qPrintable(m_output));

    m_output.clear();
    QVERIFY(session->executeCommand(QStringLiteral("/goal")));
    QVERIFY2(
        m_output.contains(
            QStringLiteral("waits for %1:%2").arg(QString::fromLatin1(kAlias), workspace)),
        qPrintable(m_output));
    QVERIFY(session->executeCommand(QStringLiteral("/goal clear")));
}

void Test::aGoalSetLocallyStillContinues()
{
    auto session = runtime(project(QStringLiteral("goal-local")));
    QVERIFY(session->openSession());
    QVERIFY(session->executeCommand(QStringLiteral("/goal port the bus")));

    m_llm.resetRequests();
    session->runTurn(QStringLiteral("hello"));
    QVERIFY2(m_llm.requests() >= 2, "the goal never continued on its own binding");
    QVERIFY(session->executeCommand(QStringLiteral("/goal clear")));
}

/* Counterexample: a durable task scheduled while bound fired into whatever
 * binding was live when it came due. */
void Test::aLoopScheduledOnARemoteBindingWaitsForIt()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString     workspace = remote(QStringLiteral("loop"));
    const QString     dir       = project(QStringLiteral("loop"));
    const qint64      longAgo   = QDateTime::currentMSecsSinceEpoch() - 3600 * 1000;
    const QJsonObject task{
        {QStringLiteral("id"), QStringLiteral("cafe0001")},
        {QStringLiteral("prompt"), QStringLiteral("check the build")},
        {QStringLiteral("cron"), QStringLiteral("*/5 * * * *")},
        {QStringLiteral("recurring"), true},
        {QStringLiteral("createdAt"), QString::number(longAgo)},
        {QStringLiteral("lastFiredAt"), QStringLiteral("0")},
        {QStringLiteral("binding"),
         QJsonObject{
             {QStringLiteral("target"), QString::fromLatin1(kAlias)},
             {QStringLiteral("workspace"), workspace}}}};
    QVERIFY(spill(
        dir + QStringLiteral("/.qsoc/loops.json"),
        QJsonDocument(
            QJsonObject{{QStringLiteral("schema"), 2}, {QStringLiteral("tasks"), QJsonArray{task}}})
            .toJson()));

    auto    session = runtime(project(QStringLiteral("loop-start")));
    QString err;
    QVERIFY2(session->switchProject(dir, &err), qPrintable(err));
    QVERIFY2(
        waitFor([&] { return m_output.contains(QStringLiteral("Loop cafe0001 waits")); }, 5000),
        qPrintable(m_output));
    QVERIFY2(!session->hasPendingAutoInputs(), "the task fired into the local binding");

    QVERIFY(bind(session.get(), workspace));
    QVERIFY2(
        waitFor([&] { return session->hasPendingAutoInputs(); }, 5000),
        "the task did not fire when its binding returned");
    session->disconnectRemote();
}

/* `remote.target` is the alias and never the user@host behind it; `cwd` keeps
 * the value hook scripts already read. */
void Test::theHookPayloadNamesTheAliasAndTheProject()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir     = project(QStringLiteral("hook"));
    const QString payload = root() + QStringLiteral("/hook-payload.json");
    QVERIFY(spill(
        dir + QStringLiteral("/.qsoc.yml"),
        QStringLiteral(
            "agent:\n"
            "  hooks:\n"
            "    user_prompt_submit:\n"
            "      - hooks:\n"
            "          - type: command\n"
            "            command: cat > '%1'; exit 2\n")
            .arg(payload)
            .toUtf8()));
    auto session = runtime(dir);
    QVERIFY(session->openSession());
    QVERIFY(bind(session.get(), remote(QStringLiteral("hook"))));
    session->runTurn(QStringLiteral("hello"));

    const QJsonObject sent = QJsonDocument::fromJson(slurp(payload)).object();
    QVERIFY2(!sent.isEmpty(), qPrintable(m_output));
    QCOMPARE(sent.value(QStringLiteral("cwd")).toString(), QDir::currentPath());
    QCOMPARE(
        QFileInfo(sent.value(QStringLiteral("project_dir")).toString()).canonicalFilePath(),
        QFileInfo(dir).canonicalFilePath());
    const QJsonObject remoteSection = sent.value(QStringLiteral("remote")).toObject();
    QCOMPARE(remoteSection.value(QStringLiteral("target")).toString(), QString::fromLatin1(kAlias));
    session->disconnectRemote();
}

/* Counterexample: the AGENTS.md snapshot was taken at bind only, so
 * a reconnect kept serving the text the host no longer had. */
void Test::projectRulesReloadWhenTheLinkComesBack()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString workspace = remote(QStringLiteral("rules"));
    QVERIFY(spill(workspace + QStringLiteral("/AGENTS.md"), "rule version one\n"));
    auto session = runtime(project(QStringLiteral("rules")));
    QVERIFY(bind(session.get(), workspace));
    QVERIFY(session->agent()->getConfig().remoteProjectRules.text.contains("rule version one"));

    QVERIFY(spill(workspace + QStringLiteral("/AGENTS.md"), "rule version two\n"));
    session->remoteConnection()->session()->disconnectFromHost();
    const QString notice = session->agent()->probeWorkspaceHealth();
    QVERIFY2(notice.contains(QStringLiteral("re-established")), qPrintable(notice));
    const QString rules = session->agent()->getConfig().remoteProjectRules.text;
    QVERIFY2(rules.contains(QStringLiteral("rule version two")), qPrintable(rules));
    QVERIFY2(notice.contains(QStringLiteral("instructions changed")), qPrintable(notice));
    session->disconnectRemote();
}

void Test::theRemoteEnvironmentSaysWhetherTheWorkspaceIsGit()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("git"));
    }
    const QString repo = remote(QStringLiteral("git-repo"));
    QCOMPARE(
        QProcess::execute(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q"), repo}),
        0);
    const QString plain       = remote(QStringLiteral("git-plain"));
    const auto    environment = [](const QString &prompt) {
        const qsizetype from = prompt.indexOf(QStringLiteral("\n# Environment\n"));
        return prompt.mid(from, prompt.indexOf(QStringLiteral("\n# "), from + 2) - from);
    };

    auto session = runtime(project(QStringLiteral("git")));
    QVERIFY(bind(session.get(), repo));
    QString section = environment(session->agent()->buildSystemPromptWithMemory());
    QVERIFY2(section.contains(QStringLiteral("- Git repository: yes")), qPrintable(section));

    QVERIFY(bind(session.get(), plain));
    section = environment(session->agent()->buildSystemPromptWithMemory());
    QVERIFY2(!section.contains(QStringLiteral("Git repository")), qPrintable(section));
    session->disconnectRemote();
}

QSOC_TEST_MAIN(Test)
#include "test_qsocworkspacebinding.moc"
