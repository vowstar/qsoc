// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/runtime/qsocagentruntime.h"
#include "agent/tool/qsoctoolpath.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * Remote path_context add/remove/clear against a loopback sshd, so the remote
 * filesystem is this filesystem and every check reads where a write landed.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips the sshd cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing the lot.
 */

namespace {

constexpr auto kAlias = "writablehost";

QByteArray slurp(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool spill(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

json actionOf(const json &tool)
{
    return tool.at("properties").at("action").at("enum");
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void localAndRemoteOfferTheSameActions();
    void anAddedDirectoryOutsideTheWorkspaceIsWritable();
    void addRefusesAMissingDirectoryAndAFile();
    void aRetargetedAddedDirectoryRefusesWritesUntilAddedAgain();
    void removeListAndClear();
    void rewindLeavesAnAddedDirectoryAlone();
    void bindingAnotherWorkspaceDropsAddedDirectories();
    void localDropsAddedDirectories();

private:
    struct RootPair
    {
        QString work;
        QString outside;
    };

    RootPair makePaths(const QString &caseName)
    {
        const QString  base = m_fixture.root() + QLatin1Char('/') + caseName;
        const RootPair paths{base + QStringLiteral("/work"), base + QStringLiteral("/outside")};
        QDir().mkpath(paths.work);
        QDir().mkpath(paths.outside);
        return paths;
    }

    bool bindWorkspace(QSocRemoteConnection *conn, const QString &workspace, QString *error)
    {
        AgentRemoteState state;
        auto            *session = new QSocSshSession();
        if (session->connectTo(m_fixture.hostConfig(static_cast<quint16>(m_fixture.port())), error)
            != QSocSshSession::ConnectStatus::Ok) {
            delete session;
            return false;
        }
        state.session          = session;
        state.sftp             = new QSocSftpClient(*session);
        state.targetKey        = QStringLiteral("loopback");
        state.endpointIdentity = QStringLiteral("loopback:") + session->hostKeyIdentity();
        if (!prepareAgentRemoteWorkspace(workspace, &state, error)) {
            discardAgentRemoteState(&state);
            return false;
        }
        if (!conn->adopt(std::move(state))) {
            // cppcheck-suppress accessMoved
            discardAgentRemoteState(&state);
            *error = QStringLiteral("adopt refused the staging bundle");
            return false;
        }
        return true;
    }

    static QString write(QSocRemoteConnection *conn, const QString &path, const QByteArray &body)
    {
        QSocToolRemoteFileWrite tool(nullptr, conn, conn->path());
        return tool.execute(
            json{{"file_path", path.toStdString()}, {"content", body.toStdString()}});
    }

    static QString pathTool(QSocRemoteConnection *conn, const json &args)
    {
        QSocToolRemotePath tool(nullptr, conn, conn->path());
        return tool.execute(args);
    }

    bool prepareRuntimeHome();

    QSocTestSshd  m_fixture;
    QTemporaryDir m_home;
    QString       m_runtimeFailure;
    QByteArray    m_oldHome;
    QByteArray    m_oldQsocHome;
    QByteArray    m_oldXdgHome;
    bool          m_hadHome     = false;
    bool          m_hadQsocHome = false;
    bool          m_hadXdgHome  = false;
    bool          m_redirected  = false;
};

void Test::initTestCase()
{
    m_fixture.start();
    if (m_fixture.state() == QSocTestSshd::State::Ready && !prepareRuntimeHome()
        && m_runtimeFailure.isEmpty()) {
        m_runtimeFailure = QStringLiteral("could not prepare the runtime home");
    }
}

bool Test::prepareRuntimeHome()
{
    if (!m_home.isValid() || !QDir().mkpath(m_home.path() + QStringLiteral("/.ssh"))) {
        m_runtimeFailure = QStringLiteral("could not lay out the client home");
        return false;
    }
    const QString config     = QStringLiteral(
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
    const QString configPath = m_home.path() + QStringLiteral("/.ssh/config");
    if (!spill(configPath, config.toUtf8())) {
        m_runtimeFailure = QStringLiteral("could not write the client ssh config");
        return false;
    }
    QFile::setPermissions(configPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_hadHome     = qEnvironmentVariableIsSet("HOME");
    m_hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
    m_hadXdgHome  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_oldHome     = qgetenv("HOME");
    m_oldQsocHome = qgetenv("QSOC_HOME");
    m_oldXdgHome  = qgetenv("XDG_CONFIG_HOME");
    m_redirected  = qputenv("HOME", m_home.path().toUtf8())
                    && qputenv("QSOC_HOME", m_home.path().toUtf8())
                    && qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
    return m_redirected;
}

void Test::cleanupTestCase()
{
    if (m_redirected) {
        m_hadHome ? qputenv("HOME", m_oldHome) : qunsetenv("HOME");
        m_hadQsocHome ? qputenv("QSOC_HOME", m_oldQsocHome) : qunsetenv("QSOC_HOME");
        m_hadXdgHome ? qputenv("XDG_CONFIG_HOME", m_oldXdgHome) : qunsetenv("XDG_CONFIG_HOME");
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    if (m_home.isValid()) {
        QVERIFY2(m_home.remove(), qPrintable(m_home.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Counterexample: the remote tool offered only show/cwd, so a model that
 * learned path_context locally asked for actions the remote tool ignored. */
void Test::localAndRemoteOfferTheSameActions()
{
    QSocToolPathContext local(nullptr, nullptr);
    QSocToolRemotePath  remote(nullptr, nullptr, nullptr);
    QCOMPARE(
        QString::fromStdString(actionOf(remote.getParametersSchema()).dump()),
        QString::fromStdString(actionOf(local.getParametersSchema()).dump()));

    /* Both list every writable root as "<name> -> <resolved path>". */
    QTemporaryDir   added;
    QSocPathContext context;
    context.addUserDir(added.path());
    QSocToolPathContext listed(nullptr, &context);
    const QString       text = listed.execute({{"action", "list"}});
    QVERIFY2(
        text.contains(QStringLiteral("Writable:\n"))
            && text.contains(QStringLiteral("  - %1 -> %2\n")
                                 .arg(added.path(), QFileInfo(added.path()).canonicalFilePath())),
        qPrintable(text));
}

/* Counterexample: `add` fell through to a listing and the write was refused
 * as outside writable directories. */
void Test::anAddedDirectoryOutsideTheWorkspaceIsWritable()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair       paths = makePaths(QStringLiteral("add_outside"));
    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));

    const QString added
        = pathTool(&conn, {{"action", "add"}, {"path", paths.outside.toStdString()}});
    QCOMPARE(added, QStringLiteral("Added to path context: %1").arg(paths.outside));

    const QString target = paths.outside + QStringLiteral("/made.txt");
    const QString result = write(&conn, target, "added\n");
    QVERIFY2(!result.startsWith(QStringLiteral("Error")), qPrintable(result));
    QCOMPARE(slurp(target), QByteArray("added\n"));
}

void Test::addRefusesAMissingDirectoryAndAFile()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair       paths = makePaths(QStringLiteral("add_refused"));
    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));
    const QString file = paths.outside + QStringLiteral("/plain.txt");
    QVERIFY(spill(file, "x\n"));

    for (const QString &bad : {paths.outside + QStringLiteral("/absent"), file}) {
        const QString result = pathTool(&conn, {{"action", "add"}, {"path", bad.toStdString()}});
        QCOMPARE(result, QStringLiteral("Error: '%1' does not exist or is not a directory").arg(bad));
    }
    QCOMPARE(conn.addedWritableDirs(), QStringList());
}

/* An added root is bound to where the host resolved it when it was added. */
void Test::aRetargetedAddedDirectoryRefusesWritesUntilAddedAgain()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair paths = makePaths(QStringLiteral("add_retarget"));
    const QString  other = paths.outside + QStringLiteral("-other");
    const QString  link  = paths.outside + QStringLiteral("-link");
    QVERIFY(QDir().mkpath(other));
    QVERIFY(QFile::link(paths.outside, link));

    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));
    pathTool(&conn, {{"action", "add"}, {"path", link.toStdString()}});
    QVERIFY2(
        !write(&conn, link + QStringLiteral("/first.txt"), "first\n").startsWith("Error"),
        "a write under the added link was refused before it moved");
    QCOMPARE(slurp(paths.outside + QStringLiteral("/first.txt")), QByteArray("first\n"));

    QVERIFY(QFile::remove(link));
    QVERIFY(QFile::link(other, link));
    const QString refused = write(&conn, link + QStringLiteral("/second.txt"), "second\n");
    QVERIFY2(refused.contains(QStringLiteral("changed identity")), qPrintable(refused));
    QVERIFY(!QFileInfo::exists(other + QStringLiteral("/second.txt")));
    const QString inside = write(&conn, paths.work + QStringLiteral("/inside.txt"), "in\n");
    QVERIFY2(!inside.startsWith(QStringLiteral("Error")), qPrintable(inside));
    const QString listed = pathTool(&conn, {{"action", "list"}});
    QVERIFY2(listed.contains(link + QStringLiteral(" -> ")), qPrintable(listed));
    QVERIFY2(listed.contains(QStringLiteral("[changed]")), qPrintable(listed));

    pathTool(&conn, {{"action", "add"}, {"path", link.toStdString()}});
    const QString again = write(&conn, link + QStringLiteral("/second.txt"), "second\n");
    QVERIFY2(!again.startsWith(QStringLiteral("Error")), qPrintable(again));
    QCOMPARE(slurp(other + QStringLiteral("/second.txt")), QByteArray("second\n"));
}

void Test::removeListAndClear()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair paths  = makePaths(QStringLiteral("remove_clear"));
    const QString  second = paths.outside + QStringLiteral("-second");
    QVERIFY(QDir().mkpath(second));
    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));
    pathTool(&conn, {{"action", "add"}, {"path", paths.outside.toStdString()}});
    pathTool(&conn, {{"action", "add"}, {"path", second.toStdString()}});

    const QString listed    = pathTool(&conn, {{"action", "list"}});
    const QString canonical = QFileInfo(paths.outside).canonicalFilePath();
    QVERIFY2(listed.startsWith(QStringLiteral("Project: ") + paths.work), qPrintable(listed));
    QVERIFY2(listed.contains(QStringLiteral("Working: ") + paths.work), qPrintable(listed));
    QVERIFY2(listed.contains(QStringLiteral("Recent:\n  - ") + paths.outside), qPrintable(listed));
    QVERIFY2(
        listed.contains(QStringLiteral("  - %1 -> %2").arg(paths.outside, canonical)),
        qPrintable(listed));
    QVERIFY2(
        listed.contains(QStringLiteral("  - %1 -> %2")
                            .arg(paths.work, QFileInfo(paths.work).canonicalFilePath())),
        qPrintable(listed));

    QCOMPARE(
        pathTool(&conn, {{"action", "remove"}, {"path", paths.outside.toStdString()}}),
        QStringLiteral("Removed from path context: %1").arg(paths.outside));
    QVERIFY(write(&conn, paths.outside + QStringLiteral("/gone.txt"), "x").startsWith("Error"));
    QVERIFY(!write(&conn, second + QStringLiteral("/kept.txt"), "x").startsWith("Error"));

    QCOMPARE(pathTool(&conn, {{"action", "clear"}}), QStringLiteral("User directories cleared."));
    QCOMPARE(conn.addedWritableDirs(), QStringList());
    QVERIFY(write(&conn, second + QStringLiteral("/after.txt"), "x").startsWith("Error"));
    QVERIFY(!write(&conn, paths.work + QStringLiteral("/root.txt"), "x").startsWith("Error"));

    /* The workspace root is never removable. */
    pathTool(&conn, {{"action", "remove"}, {"path", paths.work.toStdString()}});
    QVERIFY(!write(&conn, paths.work + QStringLiteral("/still.txt"), "x").startsWith("Error"));
}

/* Counterexample: an added root was checkpointed like the workspace, so a
 * rewind deleted the file the agent had written there. */
void Test::rewindLeavesAnAddedDirectoryAlone()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair       paths = makePaths(QStringLiteral("rewind_added"));
    QSocRemoteConnection conn;
    QString              err;
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));
    pathTool(&conn, {{"action", "add"}, {"path", paths.outside.toStdString()}});

    QTemporaryDir historyRoot;
    QVERIFY(historyRoot.isValid());
    QSocFileHistory history(historyRoot.path(), QStringLiteral("rewind-added"));
    history.setLiveAccessor(remoteLiveFileAccessor(&conn));
    QSocToolRemoteFileWrite tool(nullptr, &conn, conn.path());
    tool.setFileHistory(&history);

    const QString inside   = paths.work + QStringLiteral("/inside.txt");
    const QString outside  = paths.outside + QStringLiteral("/outside.txt");
    const QString restored = QFileInfo(paths.work).canonicalFilePath()
                             + QStringLiteral("/inside.txt");
    for (const QString &path : {inside, outside}) {
        const QString result = tool.execute(
            json{{"file_path", path.toStdString()}, {"content", "written\n"}});
        QVERIFY2(!result.startsWith(QStringLiteral("Error")), qPrintable(result));
    }

    const auto report = history.applySnapshot(0);
    QCOMPARE(report.restored, QStringList{restored});
    QVERIFY(!QFileInfo::exists(inside));
    QCOMPARE(slurp(outside), QByteArray("written\n"));
}

void Test::bindingAnotherWorkspaceDropsAddedDirectories()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    const RootPair       paths = makePaths(QStringLiteral("rebind_other"));
    const QString        next  = paths.work + QStringLiteral("-next");
    QSocRemoteConnection conn;
    QString              err;
    QVERIFY(QDir().mkpath(next));
    QVERIFY2(bindWorkspace(&conn, paths.work, &err), qPrintable(err));
    pathTool(&conn, {{"action", "add"}, {"path", paths.outside.toStdString()}});
    QCOMPARE(conn.addedWritableDirs(), QStringList{paths.outside});

    QVERIFY2(bindWorkspace(&conn, next, &err), qPrintable(err));
    QCOMPARE(conn.addedWritableDirs(), QStringList());
    QVERIFY(write(&conn, paths.outside + QStringLiteral("/stale.txt"), "x").startsWith("Error"));
}

/* `/local` unbinds the workspace, and the added directories go with it. */
void Test::localDropsAddedDirectories()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    if (!m_runtimeFailure.isEmpty()) {
        QSOC_TEST_FIXTURE_FAILED(m_runtimeFailure);
    }
    const RootPair paths   = makePaths(QStringLiteral("local_clears"));
    const QString  project = paths.work + QStringLiteral("-project");
    QVERIFY(QDir().mkpath(project));
    QSocAgentRuntimeOptions options;
    options.projectDirectory = project;
    options.workspace        = paths.work;
    QSocAgentRuntime runtime(options);
    QVERIFY(runtime.openSession());
    const auto connect = [&runtime, &paths] {
        QString    error;
        const bool ok = runtime.connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = paths.work, .remember = false},
            &error);
        return ok ? QString() : error;
    };
    const auto callPath = [&runtime](const json &args) {
        QSocTool *tool = runtime.agent()->getToolRegistry()->getTool(QStringLiteral("path_context"));
        return tool == nullptr ? QStringLiteral("(no path_context)") : tool->execute(args);
    };

    QString why = connect();
    QVERIFY2(why.isEmpty(), qPrintable(why));
    callPath({{"action", "add"}, {"path", paths.outside.toStdString()}});
    QVERIFY(callPath({{"action", "list"}}).contains(QStringLiteral("Recent:")));

    QVERIFY(runtime.executeCommand(QStringLiteral("/local")));
    why = connect();
    QVERIFY2(why.isEmpty(), qPrintable(why));
    const QString listed = callPath({{"action", "list"}});
    QVERIFY2(!listed.contains(paths.outside), qPrintable(listed));
    QSocTool *writer = runtime.agent()->getToolRegistry()->getTool(QStringLiteral("write_file"));
    const QString result = writer->execute(
        json{
            {"file_path", (paths.outside + QStringLiteral("/after.txt")).toStdString()},
            {"content", "x"}});
    QVERIFY2(result.startsWith(QStringLiteral("Error")), qPrintable(result));
    runtime.disconnectRemote();
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotewritabledirs.moc"
