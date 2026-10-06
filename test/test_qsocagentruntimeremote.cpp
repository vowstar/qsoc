// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/qsocagent.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/runtime/qsocagentruntime.h"
#include "agent/tool/qsoctoolagent.h"
#include "common/qsoclocalendpoint.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

/*
 * How /ssh picks a host and a workspace, what it remembers, and when a session
 * reconnects on its own. Every case dials a loopback sshd through the
 * production runtime, with HOME and the XDG directories redirected to the
 * fixture so the ssh config and the remembered bindings are written at runtime
 * and nothing of the real user is read or written.
 */

namespace {

constexpr int kHeaderBytes = 8;

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

bool spill(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(content) == content.size();
}

bool spillDefinition(const QString &root, const QString &name, const QString &body)
{
    const QString dir = root + QStringLiteral("/.qsoc/agents");
    return QDir().mkpath(dir)
           && spill(
               dir + '/' + name + QStringLiteral(".md"),
               QStringLiteral("---\nname: %1\ndescription: %2\n---\n\n%2 body\n")
                   .arg(name, body)
                   .toUtf8());
}

bool spillSkill(const QString &root, const QString &name, const QString &description)
{
    const QString dir = root + QStringLiteral("/.qsoc/skills/") + name;
    return QDir().mkpath(dir)
           && spill(
               dir + QStringLiteral("/SKILL.md"),
               QStringLiteral("---\nname: %1\ndescription: %2\n---\n\n%2 body\n")
                   .arg(name, description)
                   .toUtf8());
}

constexpr const char *kRemoteRules = "Remote rules sentinel";
constexpr const char *kLocalRules  = "Local rules sentinel";

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

/* A minimal qsoc-agentd client: length-prefixed JSON frames. */
class DaemonClient
{
public:
    explicit DaemonClient(const QString &socketPath)
    {
        m_socket.connectToServer(QSocLocalEndpoint::resolve(socketPath));
        m_connected = m_socket.waitForConnected(5000);
    }

    bool connected() const { return m_connected; }

    void send(const QJsonObject &object)
    {
        const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
        m_socket.write(
            QByteArray::number(payload.size(), 16).rightJustified(kHeaderBytes, '0') + payload);
        m_socket.flush();
    }

    QJsonObject receive(int timeoutMs = 30000)
    {
        while (true) {
            if (m_buffer.size() >= kHeaderBytes) {
                const int length = m_buffer.left(kHeaderBytes).toInt(nullptr, 16);
                if (m_buffer.size() >= kHeaderBytes + length) {
                    const QByteArray payload = m_buffer.mid(kHeaderBytes, length);
                    m_buffer.remove(0, kHeaderBytes + length);
                    return QJsonDocument::fromJson(payload).object();
                }
            }
            if (!m_socket.waitForReadyRead(timeoutMs)) {
                return {};
            }
            m_buffer += m_socket.readAll();
        }
    }

    /* Frames until the reply to @p id; events seen on the way are kept. */
    QJsonObject waitForReply(qint64 id)
    {
        for (int round = 0; round < 500; ++round) {
            const QJsonObject frame = receive();
            if (frame.isEmpty()) {
                return {};
            }
            if (frame.contains(QStringLiteral("event"))) {
                m_events.append(frame.value(QStringLiteral("event")).toObject());
            } else if (frame.value(QStringLiteral("id")).toInteger() == id) {
                return frame;
            }
        }
        return {};
    }

    /* Events until one of @p kind arrives; empty on timeout. */
    QJsonObject waitForEvent(const QString &kind)
    {
        for (const QJsonObject &event : std::as_const(m_events)) {
            if (event.value(QStringLiteral("kind")).toString() == kind) {
                return event;
            }
        }
        for (int round = 0; round < 500; ++round) {
            const QJsonObject frame = receive();
            if (frame.isEmpty()) {
                return {};
            }
            const QJsonObject event = frame.value(QStringLiteral("event")).toObject();
            m_events.append(event);
            if (event.value(QStringLiteral("kind")).toString() == kind) {
                return event;
            }
        }
        return {};
    }

private:
    QLocalSocket       m_socket;
    QByteArray         m_buffer;
    QList<QJsonObject> m_events;
    bool               m_connected = false;
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void bareSshOffersCatalogAndSshConfigHosts();
    void aLocalWorkspaceIsNeverTheRemoteWorkspace();
    void withoutAPickedWorkspaceNothingConnects();
    void aCatalogAliasDialsItsTargetAndWorkspace();
    void aProjectActiveEntryIsNotPresentedAsTheBinding();
    void theHostSectionShowsTheLiveBinding();
    void aRememberedBindingReconnectsWithoutAsking();
    void aProjectActiveEntryNeverConnects();
    void aFailedAutoConnectStaysLocal();
    void anInteractiveOpenAutoConnectsOnlyWhenItSaysSo();
    void aSingleQueryNeverAutoConnects();
    void shellEscapeRefusesARetargetedCwd();
    void shellEscapeReportsStderrAndExitCode();
    void remoteInstructionsLoadOnBindWithANotice();
    void localInstructionsComeFirstAndRemoteWins();
    void remoteDefinitionsShadowLocalOnes();
    void localAndRemoteSkillsAreBothListed();
    void skillCreateScopesFollowTheBinding();

private:
    static constexpr const char *kAlias = "menu-box";

    QString root() const { return m_dir.path(); }
    QString home() const { return root() + QStringLiteral("/home"); }
    QString configHome() const { return root() + QStringLiteral("/config"); }
    QString dataHome() const { return root() + QStringLiteral("/data"); }
    QString remote(const QString &name) const { return m_fixture.workDir() + '/' + name; }

    /* A fresh local project, optionally with a project host.yml. */
    QString project(const QString &name, const QByteArray &hostYml = {}) const
    {
        const QString dir = root() + QStringLiteral("/projects/") + name;
        QDir().mkpath(dir + QStringLiteral("/.qsoc"));
        if (!hostYml.isEmpty()) {
            spill(dir + QStringLiteral("/.qsoc/host.yml"), hostYml);
        }
        return dir;
    }

    /* A catalog entry that has no ssh-config block of its own. */
    QByteArray catalogOnly(const QString &workspace) const
    {
        return QStringLiteral(
                   "hostList:\n"
                   "  - alias: cat-only\n"
                   "    target: %1\n"
                   "    workspace: %2\n"
                   "    capability: |\n"
                   "      loopback catalog host\n")
            .arg(QString::fromLatin1(kAlias), workspace)
            .toUtf8();
    }

    std::unique_ptr<QSocAgentRuntime> runtime(
        const QString &projectDir, const QString &workspace = QString())
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = projectDir;
        options.workspace        = workspace;
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
        made->setDirectoryPicker(
            [this](const QString &, const QString &start, const QString &, const auto &, const auto &) {
                m_pickerStart = start;
                ++m_pickerCalls;
                return m_pick;
            });
        m_output.clear();
        m_pickerCalls = 0;
        m_pickerStart.clear();
        m_pick.clear();
        return made;
    }

    /* A bound session on kAlias whose workspace is @p workspace. */
    std::unique_ptr<QSocAgentRuntime> boundRuntime(const QString &name, const QString &workspace)
    {
        auto    session = runtime(project(name));
        QString err;
        if (!session->connectRemote(
                {.target = QString::fromLatin1(kAlias), .workspace = workspace, .remember = false},
                &err)) {
            qWarning("connectRemote: %s", qPrintable(err));
            return {};
        }
        return session;
    }

    QSocHostBinding remembered(const QString &projectDir) const
    {
        return QSocHostBindingStore::load(QSocHostBindingStore::defaultDir(), projectDir);
    }

    QProcessEnvironment childEnvironment() const
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("HOME"), home());
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome());
        env.insert(QStringLiteral("XDG_DATA_HOME"), dataHome());
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), root() + QStringLiteral("/run"));
        env.insert(QStringLiteral("QSOC_HOME"), configHome() + QStringLiteral("/qsoc"));
        env.insert(QStringLiteral("TMPDIR"), root() + QStringLiteral("/tmp"));
        env.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
        env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
        env.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));
        return env;
    }

    /* Remember kAlias for @p projectDir where the qsoc binaries look for it. */
    bool rememberForBinaries(const QString &projectDir, const QString &workspace) const
    {
        return QSocHostBindingStore::save(
            dataHome() + QStringLiteral("/QSoC/host-bindings"),
            projectDir,
            {QString::fromLatin1(kAlias), workspace});
    }

    /* A remote workspace holding an AGENTS.md with the remote sentinel. */
    QString rulesWorkspace(const QString &name) const
    {
        const QString dir = remote(name);
        QDir().mkpath(dir);
        spill(dir + QStringLiteral("/AGENTS.md"), QByteArray(kRemoteRules) + '\n');
        return dir;
    }

    /* A local project holding an AGENTS.md with the local sentinel. */
    QString rulesProject(const QString &name) const
    {
        const QString dir = project(name);
        spill(dir + QStringLiteral("/AGENTS.md"), QByteArray(kLocalRules) + '\n');
        return dir;
    }

    static QString runTool(QSocAgentRuntime *session, const QString &name, const json &args)
    {
        QSocTool *tool = session->agent()->getToolRegistry()->getTool(name);
        return tool == nullptr ? QString() : tool->execute(args);
    }

    static QString skills(QSocAgentRuntime *session)
    {
        return runTool(session, QStringLiteral("skill_find"), {{"action", "list"}});
    }

    static QString prompt(QSocAgentRuntime *session)
    {
        return session->agent()->buildSystemPromptWithMemory();
    }

    /* The description the session's definition named @p name carries, or empty. */
    static QString definition(QSocAgentRuntime *session, const QString &name)
    {
        auto *spawn = dynamic_cast<QSocToolAgent *>(
            session->agent()->getToolRegistry()->getTool(QStringLiteral("agent")));
        const QSocAgentDefinition *def = spawn != nullptr && spawn->definitionRegistry() != nullptr
                                             ? spawn->definitionRegistry()->find(name)
                                             : nullptr;
        return def != nullptr ? def->description : QString();
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    QString       m_output;
    QString       m_pick;
    QString       m_pickerStart;
    int           m_pickerCalls = 0;
    int           m_mockPort    = 0;
};

void Test::initTestCase()
{
    qRegisterMetaType<QSocAgentRuntimeEvent>("QSocAgentRuntimeEvent");
    m_fixture.start();
    QVERIFY(m_dir.isValid());
    for (const QString &dir :
         {home() + QStringLiteral("/.ssh"),
          configHome() + QStringLiteral("/qsoc"),
          dataHome(),
          root() + QStringLiteral("/run"),
          root() + QStringLiteral("/tmp")}) {
        QVERIFY(QDir().mkpath(dir));
    }
    QFile::setPermissions(
        root() + QStringLiteral("/run"),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const auto env = childEnvironment();
    for (const QString &key : env.keys()) {
        QVERIFY(qputenv(key.toUtf8().constData(), env.value(key).toUtf8()));
    }

    /* A second alias that nothing listens on, for the failure case. */
    const int        deadPort = pickFreePort();
    const QByteArray config   = QStringLiteral(
                                    "Host %1\n"
                                    "  HostName 127.0.0.1\n"
                                    "  Port %2\n"
                                    "  User %3\n"
                                    "  IdentityFile %4\n"
                                    "  IdentitiesOnly yes\n"
                                    "  StrictHostKeyChecking no\n"
                                    "  UserKnownHostsFile /dev/null\n"
                                    "Host dead-box\n"
                                    "  HostName 127.0.0.1\n"
                                    "  Port %5\n"
                                    "  User %3\n"
                                    "  IdentityFile %4\n"
                                    "  StrictHostKeyChecking no\n"
                                    "  UserKnownHostsFile /dev/null\n"
                                    "Host *.wild\n"
                                    "  User nobody\n")
                                    .arg(QString::fromLatin1(kAlias))
                                    .arg(m_fixture.port())
                                    .arg(m_fixture.user(), m_fixture.keyPath())
                                    .arg(deadPort)
                                    .toUtf8();
    QVERIFY(spill(home() + QStringLiteral("/.ssh/config"), config));
    QFile::setPermissions(
        home() + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_mockPort = pickFreePort();
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
            "  session_title: false\n"
            "  away_summary: false\n"
            "  memory_extract: false\n"
            "  memory_dream: false\n"
            "proxy:\n"
            "  type: none\n")
            .arg(m_mockPort)
            .toUtf8()));
}

void Test::cleanupTestCase()
{
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so destructors would not clean up. */
    QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Counterexample: bare /ssh failed with "empty SSH target"; the menu of
 * catalog and ssh-config hosts was lost with the daemon refactor. */
void Test::bareSshOffersCatalogAndSshConfigHosts()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir     = project(QStringLiteral("menu"), catalogOnly(remote("cat")));
    auto          session = runtime(dir);
    QStringList   offered;
    QStringList   hints;
    session->setMenuHandler(
        [&](const QString &, const QStringList &items, const QStringList &rowHints, const auto &) {
            offered = items;
            hints   = rowHints;
            return static_cast<int>(items.indexOf(QString::fromLatin1(kAlias)));
        });
    m_pick = remote(QStringLiteral("picked"));

    QVERIFY(session->executeCommand(QStringLiteral("/ssh")));
    QVERIFY2(session->isRemote(), qPrintable(m_output));
    QCOMPARE(offered.value(0), QStringLiteral("cat-only"));
    QCOMPARE(hints.value(0), QStringLiteral("catalog: loopback catalog host"));
    QVERIFY(offered.contains(QString::fromLatin1(kAlias)));
    QVERIFY(offered.contains(QStringLiteral("dead-box")));
    QVERIFY2(!offered.contains(QStringLiteral("*.wild")), qPrintable(offered.join(',')));
    QCOMPARE(m_pickerCalls, 1);
    QCOMPARE(session->remoteWorkspace(), m_pick);

    /* Remembered for this user, never written into the project. */
    QCOMPARE(remembered(dir).target, QString::fromLatin1(kAlias));
    QCOMPARE(remembered(dir).workspace, m_pick);
    QVERIFY(!slurp(dir + QStringLiteral("/.qsoc/host.yml")).contains("active"));
}

/* Counterexample: /ssh reused the launch --workspace, a LOCAL path, as the
 * remote workspace, or fell back to `/`. */
void Test::aLocalWorkspaceIsNeverTheRemoteWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString local = root() + QStringLiteral("/local-workspace");
    QVERIFY(QDir().mkpath(local));
    auto session = runtime(project(QStringLiteral("x1")), local);
    m_pick       = remote(QStringLiteral("x1"));

    QVERIFY(session->executeCommand(QStringLiteral("/ssh %1").arg(QString::fromLatin1(kAlias))));
    QVERIFY2(session->isRemote(), qPrintable(m_output));
    QCOMPARE(m_pickerCalls, 1);
    QCOMPARE(session->remoteWorkspace(), m_pick);
}

void Test::withoutAPickedWorkspaceNothingConnects()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    auto session = runtime(project(QStringLiteral("nopick")));

    QVERIFY(session->executeCommand(QStringLiteral("/ssh %1").arg(QString::fromLatin1(kAlias))));
    QVERIFY2(!session->isRemote(), qPrintable(session->remoteWorkspace()));
    QCOMPARE(m_pickerCalls, 1);
    QVERIFY2(!m_pickerStart.isEmpty(), "the picker was given no start directory");
    QVERIFY2(m_output.contains(QStringLiteral("No remote workspace selected")), qPrintable(m_output));
}

/* Counterexample: /ssh dialled a catalog-only alias as a raw host name, so its
 * `target` and `workspace` were ignored. */
void Test::aCatalogAliasDialsItsTargetAndWorkspace()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString workspace = remote(QStringLiteral("catalog"));
    auto          session   = runtime(project(QStringLiteral("x2"), catalogOnly(workspace)));

    QVERIFY(session->executeCommand(QStringLiteral("/ssh cat-only")));
    QVERIFY2(session->isRemote(), qPrintable(m_output));
    QCOMPARE(session->remoteWorkspace(), workspace);
    QCOMPARE(m_pickerCalls, 0);
}

/* Counterexample: the Host Catalog section named the project file's `active:`
 * as the current binding while the session was local. */
void Test::aProjectActiveEntryIsNotPresentedAsTheBinding()
{
    const QByteArray hostYml = catalogOnly(remote(QStringLiteral("p")))
                               + "active:\n  target: planted-host\n  workspace: /\n";
    auto             session = runtime(project(QStringLiteral("x3"), hostYml));
    const QString    prompt  = session->agent()->buildSystemPromptWithMemory();
    QVERIFY2(prompt.contains(QStringLiteral("# Host Catalog")), qPrintable(prompt));
    QVERIFY2(!prompt.contains(QStringLiteral("planted-host")), qPrintable(prompt));
    QVERIFY2(!prompt.contains(QStringLiteral("use the active binding")), qPrintable(prompt));
}

void Test::theHostSectionShowsTheLiveBinding()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString workspace = remote(QStringLiteral("live"));
    auto          session   = runtime(project(QStringLiteral("live"), catalogOnly(workspace)));
    QVERIFY(session->executeCommand(QStringLiteral("/ssh cat-only")));
    QVERIFY2(session->isRemote(), qPrintable(m_output));

    const QString prompt = session->agent()->buildSystemPromptWithMemory();
    const QString host   = prompt.mid(prompt.indexOf(QStringLiteral("# Host Catalog")));
    QVERIFY2(
        host.contains(
            QStringLiteral("Active: %1\nWorkspace: %2\n").arg(session->remoteTarget(), workspace)),
        qPrintable(host));
}

void Test::aRememberedBindingReconnectsWithoutAsking()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir = project(QStringLiteral("remember"));
    {
        auto first = runtime(dir);
        m_pick     = remote(QStringLiteral("remember"));
        QVERIFY(first->executeCommand(QStringLiteral("/ssh %1").arg(QString::fromLatin1(kAlias))));
        QVERIFY2(first->isRemote(), qPrintable(m_output));
        first->disconnectRemote();
    }
    auto second = runtime(dir);
    second->connectRememberedRemote();
    QVERIFY2(second->isRemote(), qPrintable(m_output));
    QCOMPARE(second->remoteWorkspace(), remote(QStringLiteral("remember")));
    QCOMPARE(m_pickerCalls, 0);
    QVERIFY2(m_output.contains(QStringLiteral("Auto-connecting")), qPrintable(m_output));

    /* An explicit /ssh to the same target reuses the remembered workspace. */
    second->disconnectRemote();
    QVERIFY(second->executeCommand(QStringLiteral("/ssh %1").arg(QString::fromLatin1(kAlias))));
    QVERIFY(second->isRemote());
    QCOMPARE(m_pickerCalls, 0);
}

/* A cloned repository must not choose where qsoc connects. */
void Test::aProjectActiveEntryNeverConnects()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QByteArray hostYml = QStringLiteral("active:\n  target: %1\n  workspace: %2\n")
                                   .arg(QString::fromLatin1(kAlias), remote(QStringLiteral("p")))
                                   .toUtf8();
    auto             session = runtime(project(QStringLiteral("planted"), hostYml));
    session->connectRememberedRemote();
    QVERIFY(!session->isRemote());
    QVERIFY2(m_output.contains(QStringLiteral("Ignoring active:")), qPrintable(m_output));
}

void Test::aFailedAutoConnectStaysLocal()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir = project(QStringLiteral("dead"));
    QVERIFY(
        QSocHostBindingStore::save(
            QSocHostBindingStore::defaultDir(),
            dir,
            {QStringLiteral("dead-box"), remote(QStringLiteral("dead"))}));
    auto session = runtime(dir);
    session->connectRememberedRemote();
    QVERIFY(!session->isRemote());
    QVERIFY(session->remoteWorkspace().isEmpty());
    QVERIFY(!session->agent()->getConfig().remoteMode);
    QVERIFY2(m_output.contains(QStringLiteral("Staying local")), qPrintable(m_output));
}

/* The daemon cannot see -q on its own, so only an open that says it is
 * interactive reconnects; an older client that says nothing never does. */
void Test::anInteractiveOpenAutoConnectsOnlyWhenItSaysSo()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString dir = project(QStringLiteral("daemon"));
    QVERIFY(rememberForBinaries(dir, remote(QStringLiteral("daemon"))));

    const QString socketPath = root() + QStringLiteral("/run/d.sock");
    QProcess      daemon;
    daemon.setProcessEnvironment(childEnvironment());
    daemon.setStandardErrorFile(root() + QStringLiteral("/agentd.err"));
    daemon.start(QStringLiteral(QSOC_AGENTD_PATH), {QStringLiteral("-s"), socketPath});
    QVERIFY2(daemon.waitForStarted(5000), qPrintable(daemon.errorString()));
    QVERIFY(waitFor([&] { return QFile::exists(socketPath); }, 10000));
    const auto stop = qScopeGuard([&] {
        daemon.terminate();
        if (!daemon.waitForFinished(5000)) {
            daemon.kill();
            daemon.waitForFinished(3000);
        }
    });

    {
        DaemonClient silent(socketPath);
        QVERIFY(silent.connected());
        QVERIFY(!silent.receive().isEmpty()); /* greeting */
        silent.send(
            {{"id", 1}, {"method", "open"}, {"params", QJsonObject{{"project_directory", dir}}}});
        QVERIFY(silent.waitForReply(1).value("result").toObject().value("ok").toBool());
        silent.send({{"id", 2}, {"method", "status"}});
        const QJsonObject status = silent.waitForReply(2).value("result").toObject();
        QVERIFY2(status.contains("session_id"), qPrintable(QJsonDocument(status).toJson()));
        QVERIFY(!status.contains("remote"));
    }

    DaemonClient interactive(socketPath);
    QVERIFY(interactive.connected());
    QVERIFY(!interactive.receive().isEmpty());
    interactive.send(
        {{"id", 1},
         {"method", "open"},
         {"params", QJsonObject{{"project_directory", dir}, {"single_query", false}}}});
    QVERIFY(interactive.waitForReply(1).value("result").toObject().value("ok").toBool());
    const QJsonObject changed = interactive.waitForEvent(QStringLiteral("remote_changed"));
    QVERIFY2(changed.value("flag").toBool(), qPrintable(slurp(root() + "/agentd.err")));
    QCOMPARE(changed.value("secondary").toString(), remote(QStringLiteral("daemon")));
}

void Test::aSingleQueryNeverAutoConnects()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString qsoc = builtQsoc();
    const QString mock = QString::fromUtf8(QSOC_MOCK_LLM_PATH);
    if (qsoc.isEmpty() || !QFile::exists(mock)) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a built qsoc and qsoc_mock_llm"));
    }
    const QString dir = project(QStringLiteral("query"));
    QVERIFY(rememberForBinaries(dir, remote(QStringLiteral("query"))));
    const QString requestLog = root() + QStringLiteral("/requests.jsonl");

    QProcess            mockLlm;
    QProcessEnvironment mockEnv = childEnvironment();
    mockEnv.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnv.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnv.insert(QStringLiteral("MOCK_REQUEST_LOG"), requestLog);
    mockLlm.setProcessEnvironment(mockEnv);
    mockLlm.start(mock, {QString::number(m_mockPort), QStringLiteral("none")});
    QVERIFY(mockLlm.waitForStarted(5000));
    const auto stopMock = qScopeGuard([&] {
        mockLlm.terminate();
        if (!mockLlm.waitForFinished(3000)) {
            mockLlm.kill();
            mockLlm.waitForFinished(2000);
        }
    });
    QVERIFY(waitFor(
        [&] {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_mockPort));
            return probe.waitForConnected(200);
        },
        8000));

    QProcess agent;
    agent.setProcessEnvironment(childEnvironment());
    agent.setWorkingDirectory(dir);
    agent.start(qsoc, {QStringLiteral("agent"), QStringLiteral("-q"), QStringLiteral("hello")});
    QVERIFY(agent.waitForStarted(10000));
    QVERIFY2(agent.waitForFinished(120000), "qsoc agent -q never finished");
    const QByteArray out = agent.readAllStandardOutput() + agent.readAllStandardError();
    QVERIFY2(out.contains("MOCKDONE"), out.constData());
    const QByteArray wire = slurp(requestLog);
    QVERIFY2(!wire.isEmpty(), "the model was never asked");
    QVERIFY2(!wire.contains("# Remote Workspace"), "a -q run connected the remembered binding");
}

void Test::shellEscapeRefusesARetargetedCwd()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString work     = remote(QStringLiteral("escape_retargeted/work"));
    const QString outside  = remote(QStringLiteral("escape_retargeted/outside"));
    const QString inside   = work + QStringLiteral("/inside");
    const QString selected = work + QStringLiteral("/selected");
    const QString victim   = outside + QStringLiteral("/victim.txt");
    QVERIFY(QDir().mkpath(inside));
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(QFile::link(inside, selected));

    auto session = boundRuntime(QStringLiteral("escape_retargeted"), work);
    QVERIFY(session);
    QString err;
    QVERIFY2(session->setWorkingDirectory(QStringLiteral("selected"), &err), qPrintable(err));
    QVERIFY(QFile::remove(selected));
    QVERIFY(QFile::link(outside, selected));

    m_output.clear();
    QVERIFY(session->executeCommand(QStringLiteral("!printf wrong > victim.txt")));
    QVERIFY2(!QFileInfo::exists(victim), "! ran from a cwd outside the workspace");
    QVERIFY2(m_output.contains(QStringLiteral("outside the workspace")), qPrintable(m_output));
    session->disconnectRemote();
}

void Test::shellEscapeReportsStderrAndExitCode()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString work = remote(QStringLiteral("escape_status"));
    QVERIFY(QDir().mkpath(work));
    auto session = boundRuntime(QStringLiteral("escape_status"), work);
    QVERIFY(session);

    m_output.clear();
    QVERIFY(session->executeCommand(QStringLiteral("!echo to-stderr >&2; exit 4")));
    QVERIFY2(m_output.contains(QStringLiteral("to-stderr")), qPrintable(m_output));
    QVERIFY2(m_output.contains(QStringLiteral("(exit code: 4)")), qPrintable(m_output));
    QVERIFY2(m_output.contains(QStringLiteral("(shell: ")), qPrintable(m_output));
    session->disconnectRemote();
}

/* Counterexample: /ssh never loaded the remote project's AGENTS.md into the
 * main session. */
void Test::remoteInstructionsLoadOnBindWithANotice()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString ws      = rulesWorkspace(QStringLiteral("rules"));
    auto          session = boundRuntime(QStringLiteral("rules"), ws);
    QVERIFY(session);
    QVERIFY2(
        prompt(session.get()).contains(QLatin1String(kRemoteRules)),
        "AGENTS.md is not in the prompt");
    QVERIFY2(
        m_output.contains(
            QStringLiteral("Loaded AGENTS.md from %1:%2 (").arg(QString::fromLatin1(kAlias), ws)),
        qPrintable(m_output));
    QVERIFY2(!m_output.contains(QStringLiteral("AGENTS.local.md")), qPrintable(m_output));

    /* /local drops what the remote workspace supplied. */
    session->disconnectRemote();
    QVERIFY(!prompt(session.get()).contains(QLatin1String(kRemoteRules)));
}

/* Counterexample: binding a remote workspace replaced the local project's
 * AGENTS.md instead of adding the remote one after it. */
void Test::localInstructionsComeFirstAndRemoteWins()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    rulesProject(QStringLiteral("layered"));
    auto session
        = boundRuntime(QStringLiteral("layered"), rulesWorkspace(QStringLiteral("layered")));
    QVERIFY(session);
    const QString text   = prompt(session.get());
    const auto    local  = text.indexOf(QLatin1String(kLocalRules));
    const auto    remote = text.indexOf(QLatin1String(kRemoteRules));
    QVERIFY2(local >= 0 && remote > local, qPrintable(text));
    QCOMPARE(text.count(QStringLiteral("# Project instructions")), 1);
    QVERIFY2(
        text.indexOf(QStringLiteral("remote workspace instructions win")) > remote,
        qPrintable(text));

    session->disconnectRemote();
    const QString back = prompt(session.get());
    QVERIFY(back.contains(QLatin1String(kLocalRules)));
    QVERIFY(!back.contains(QLatin1String(kRemoteRules)));
}

/* Counterexample: /local dropped every project-scope definition, including
 * the local project's own, until the next restart. */
void Test::remoteDefinitionsShadowLocalOnes()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString ws  = remote(QStringLiteral("defs"));
    const QString dir = project(QStringLiteral("defs"));
    QVERIFY(spillDefinition(ws, QStringLiteral("remote-only"), QStringLiteral("Remote def")));
    QVERIFY(spillDefinition(dir, QStringLiteral("local-only"), QStringLiteral("Local def")));
    QVERIFY(spillDefinition(ws, QStringLiteral("shared"), QStringLiteral("Remote shared")));
    QVERIFY(spillDefinition(dir, QStringLiteral("shared"), QStringLiteral("Local shared")));

    auto session = boundRuntime(QStringLiteral("defs"), ws);
    QVERIFY(session);
    QCOMPARE(definition(session.get(), QStringLiteral("remote-only")), QStringLiteral("Remote def"));
    QCOMPARE(definition(session.get(), QStringLiteral("shared")), QStringLiteral("Remote shared"));
    QCOMPARE(definition(session.get(), QStringLiteral("local-only")), QStringLiteral("Local def"));
    m_output.clear();
    QVERIFY(session->executeCommand(QStringLiteral("/agents")));
    const auto remoteAt = m_output.indexOf(QStringLiteral("Remote workspace (.qsoc/agents/):"));
    QVERIFY2(remoteAt >= 0, qPrintable(m_output));
    QVERIFY2(
        m_output.indexOf(QStringLiteral("remote-only"), remoteAt) > remoteAt, qPrintable(m_output));

    session->disconnectRemote();
    QCOMPARE(definition(session.get(), QStringLiteral("local-only")), QStringLiteral("Local def"));
    QCOMPARE(definition(session.get(), QStringLiteral("shared")), QStringLiteral("Local shared"));
    QCOMPARE(definition(session.get(), QStringLiteral("remote-only")), QString());
}

/* Counterexample: in remote mode the remote .qsoc/skills replaced the local
 * project's, so local skills could not be used on the remote project. */
void Test::localAndRemoteSkillsAreBothListed()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString ws  = remote(QStringLiteral("skills"));
    const QString dir = project(QStringLiteral("skills"));
    QVERIFY(spillSkill(dir, QStringLiteral("local-skill"), QStringLiteral("Local only")));
    QVERIFY(spillSkill(dir, QStringLiteral("shared-skill"), QStringLiteral("Local shared")));
    QVERIFY(spillSkill(ws, QStringLiteral("remote-skill"), QStringLiteral("Remote only")));
    QVERIFY(spillSkill(ws, QStringLiteral("shared-skill"), QStringLiteral("Remote shared")));

    auto session = boundRuntime(QStringLiteral("skills"), ws);
    QVERIFY(session);
    const QString listed = skills(session.get());
    QVERIFY2(listed.contains(QStringLiteral("- local-skill [local]: Local only")), qPrintable(listed));
    QVERIFY2(
        listed.contains(QStringLiteral("- remote-skill [remote]: Remote only")), qPrintable(listed));
    QVERIFY2(
        listed.contains(QStringLiteral("- shared-skill [remote]: Remote shared")),
        qPrintable(listed));
    QVERIFY(!listed.contains(QStringLiteral("Local shared")));
    const QString listing = session->agent()->getConfig().skillListing;
    QVERIFY2(listing.contains(QStringLiteral("**local-skill** [local]")), qPrintable(listing));
    QVERIFY2(listing.contains(QStringLiteral("**remote-skill** [remote]")), qPrintable(listing));
    QVERIFY(session->availableCommands().contains(QStringLiteral("/remote-skill")));
    QVERIFY(session->availableCommands().contains(QStringLiteral("/local-skill")));
}

/* Counterexample: in remote mode skill_create reached only the remote
 * workspace and the user dir, never this machine's project. */
void Test::skillCreateScopesFollowTheBinding()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const QString ws  = remote(QStringLiteral("create"));
    const QString dir = project(QStringLiteral("create"));
    QVERIFY(QDir().mkpath(ws));
    auto session = boundRuntime(QStringLiteral("create"), ws);
    QVERIFY(session);

    json args{{"name", "here"}, {"description", "Here"}, {"instructions", "do"}, {"scope", "local"}};
    QString created = runTool(session.get(), QStringLiteral("skill_create"), args);
    QVERIFY2(created.startsWith(QStringLiteral("Successfully created")), qPrintable(created));
    QVERIFY(QFile::exists(dir + QStringLiteral("/.qsoc/skills/here/SKILL.md")));
    QVERIFY(!QFile::exists(ws + QStringLiteral("/.qsoc/skills/here/SKILL.md")));

    args["name"]  = "there";
    args["scope"] = "project";
    created       = runTool(session.get(), QStringLiteral("skill_create"), args);
    QVERIFY2(created.startsWith(QStringLiteral("Successfully created")), qPrintable(created));
    QVERIFY(QFile::exists(ws + QStringLiteral("/.qsoc/skills/there/SKILL.md")));
    QVERIFY(!QFile::exists(dir + QStringLiteral("/.qsoc/skills/there/SKILL.md")));

    const QString listed = skills(session.get());
    QVERIFY2(listed.contains(QStringLiteral("- here [local]")), qPrintable(listed));
    QVERIFY2(listed.contains(QStringLiteral("- there [remote]")), qPrintable(listed));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocagentruntimeremote.moc"
