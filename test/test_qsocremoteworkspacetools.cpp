// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/runtime/qsocagentruntime.h"
#include "common/qsocimageattach.h"
#include "common/qsocmachine.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * A runtime bound to a loopback sshd through `/ssh`: the workspace-facing
 * pieces (read_file, /skill placeholders, the prompt's skill listing) must
 * describe and reach the remote workspace, never the local project.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing the lot.
 */

namespace {

constexpr auto kAlias = "workspacehost";

QByteArray makePng(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(Qt::darkCyan));
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

bool writeSkill(
    const QString &project, const QString &name, bool userInvocable, const QByteArray &body)
{
    const QString dir = project + QStringLiteral("/.qsoc/skills/") + name;
    return QDir().mkpath(dir)
           && writeFile(
               dir + QStringLiteral("/SKILL.md"),
               "---\nname: " + name.toUtf8() + "\ndescription: " + name.toUtf8()
                   + " probe\nuser-invocable: " + (userInvocable ? "true" : "false") + "\n---\n"
                   + body);
}

/* The value after `key=` on the line that carries it. */
QString valueOf(const QString &text, const QString &key)
{
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(key + QLatin1Char('='))) {
            return line.mid(key.size() + 1).trimmed();
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
    void readFileAttachesARemoteImage();
    void readFilePagesALargeLogWithoutReadingItAll();
    void readFileBoundsOneOversizedLine();
    void listFilesTakesTheLocalArgumentsAndSaysWhenItStops();
    void editFileReplacesAllLikeTheLocalTool();
    void tildeIsTheRemoteHome();
    void windowsSpellingsReachTheWorkspace();
    void skillPlaceholdersNameTheRemoteWorkspace();
    void skillListingFollowsTheBinding();
    void todoAddWritesTheRemoteWorkspace();
    void remoteProjectSkillIsListedAndReadable();
    void remoteSkillLinkOutsideTheWorkspaceIsRefused();
    void skillCreateWritesTheRemoteWorkspace();
    void projectMemoryStaysOnThisMachineKeyedByWorkspace();
    void switchingWorkspacesSeparatesProjectMemory();

private:
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QString readRemote(const json &args) const { return call(QStringLiteral("read_file"), args); }

    QString call(const QString &name, const json &args) const
    {
        QSocTool *tool = m_runtime->agent()->getToolRegistry()->getTool(name);
        return tool == nullptr ? QStringLiteral("(no %1 tool)").arg(name) : tool->execute(args);
    }

    bool bind(const QString &workspace)
    {
        QString    error;
        const bool ok = m_runtime->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = workspace, .remember = false},
            &error);
        if (!ok) {
            qWarning("connectRemote: %s", qPrintable(error));
        }
        return ok;
    }

    QSocTestSshd                      m_fixture;
    QTemporaryDir                     m_dir;
    std::unique_ptr<QSocAgentRuntime> m_runtime;
    bool                              m_ready = false;
    QString                           m_failure;
    QString                           m_home;
    QString                           m_project;
    QString                           m_workspace;
    QByteArray                        m_oldHome;
    QByteArray                        m_oldQsocHome;
    QByteArray                        m_oldXdgHome;
    bool                              m_hadHome     = false;
    bool                              m_hadQsocHome = false;
    bool                              m_hadXdgHome  = false;
    QByteArray                        m_oldXdgData;
    bool                              m_hadXdgData = false;
};

#define REQUIRE_WORKSPACE_FIXTURE() \
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
        return; /* the fixture's own state decides skip versus fail */
    }
    m_ready = prepare();
}

bool Test::prepare()
{
    if (!m_dir.isValid()) {
        return fail(QStringLiteral("temporary directory: %1").arg(m_dir.errorString()));
    }
    m_home      = m_dir.path() + QStringLiteral("/home");
    m_project   = m_dir.path() + QStringLiteral("/project");
    m_workspace = m_fixture.workDir() + QStringLiteral("/ws");
    if (!QDir().mkpath(m_home + QStringLiteral("/.ssh")) || !QDir().mkpath(m_project)
        || !QDir().mkpath(m_workspace)) {
        return fail(QStringLiteral("could not lay out the fixture tree"));
    }
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
    /* A vision model, so an image read can take the attachment path. Nothing
     * here sends a request, so the endpoint is never dialed. */
    const QByteArray yaml = "llm:\n"
                            "  model: vision\n"
                            "  models:\n"
                            "    vision:\n"
                            "      url: http://127.0.0.1:9/v1/chat/completions\n"
                            "      modalities:\n"
                            "        image: true\n"
                            "        image_max_tokens: 10000\n"
                            "        image_max_dimension: 1568\n";
    if (!writeFile(m_home + QStringLiteral("/qsoc.yml"), yaml)) {
        return fail(QStringLiteral("could not write the model config"));
    }
    if (!writeSkill(m_project, QStringLiteral("localonly"), true, "local only\n")
        || !writeSkill(m_workspace, QStringLiteral("probe"), true, "PROJECT=${PROJECT}\nCWD=${CWD}\n")
        || !writeSkill(m_workspace, QStringLiteral("modelonly"), false, "model only\n")
        || !QDir().mkpath(m_home + QStringLiteral("/qsoc/skills/mine"))
        || !writeFile(
            m_home + QStringLiteral("/qsoc/skills/mine/SKILL.md"),
            "---\nname: mine\ndescription: mine probe\n---\nmine\n")) {
        return fail(QStringLiteral("could not write the skills"));
    }
    m_hadHome     = qEnvironmentVariableIsSet("HOME");
    m_hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
    m_hadXdgHome  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_oldHome     = qgetenv("HOME");
    m_oldQsocHome = qgetenv("QSOC_HOME");
    m_oldXdgHome  = qgetenv("XDG_CONFIG_HOME");
    m_hadXdgData  = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    m_oldXdgData  = qgetenv("XDG_DATA_HOME");
    if (!qputenv("HOME", m_home.toUtf8()) || !qputenv("QSOC_HOME", m_home.toUtf8())
        || !qputenv("XDG_CONFIG_HOME", m_home.toUtf8())
        || !qputenv("XDG_DATA_HOME", (m_home + QStringLiteral("/data")).toUtf8())) {
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
        m_hadXdgData ? qputenv("XDG_DATA_HOME", m_oldXdgData) : qunsetenv("XDG_DATA_HOME");
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Every case starts local and binds with `/ssh`, the way a user does. */
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

/* Counterexample: the remote read_file returned the PNG bytes decoded as
 * UTF-8 text, so the model got mojibake instead of the picture. */
void Test::readFileAttachesARemoteImage()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(
        m_runtime->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = m_workspace, .remember = false},
            &error),
        qPrintable(error));
    QVERIFY(writeFile(m_workspace + QStringLiteral("/shot.txt"), makePng(64, 48)));

    const QString result = readRemote({{"file_path", "shot.txt"}});
    QVERIFY2(result.startsWith(QStringLiteral("[image attached:")), qPrintable(result.left(200)));
    QVERIFY(result.contains(QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen())));
    QVERIFY(!result.contains(QStringLiteral("PNG\r\n")));
}

/* A log far past one read's byte limit still pages by line: the file size is
 * never refused, and only the requested window comes back. */
void Test::readFilePagesALargeLogWithoutReadingItAll()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(
        m_runtime->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = m_workspace, .remember = false},
            &error),
        qPrintable(error));
    QByteArray log;
    const int  lines = 300000;
    log.reserve(lines * 64);
    for (int i = 0; i < lines; ++i) {
        log += QByteArray("line ").append(QByteArray::number(i)).append(QByteArray(50, 'x')) + '\n';
    }
    QVERIFY(log.size() > 16 * 1024 * 1024);
    QVERIFY(writeFile(m_workspace + QStringLiteral("/big.log"), log));

    const QString tail = readRemote({{"file_path", "big.log"}, {"offset", lines - 2}});
    QVERIFY2(tail.startsWith(QStringLiteral("line 299998x")), qPrintable(tail.left(200)));
    QCOMPARE(tail.count(QLatin1Char('\n')), 2);

    const QString window = readRemote(
        {{"file_path", "big.log"}, {"offset", 1000}, {"max_lines", 2}});
    QVERIFY2(window.startsWith(QStringLiteral("line 1000x")), qPrintable(window.left(200)));
    QVERIFY(window.contains(QStringLiteral("line 1001x")));
    QVERIFY(!window.contains(QStringLiteral("line 1002x")));
    QVERIFY2(window.contains(QStringLiteral("offset=1002")), qPrintable(window));
}

/* Counterexample: one line with no newline came back whole, so a single read
 * could hand the model any number of megabytes. */
void Test::readFileBoundsOneOversizedLine()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(
        m_runtime->connectRemote(
            {.target = QString::fromLatin1(kAlias), .workspace = m_workspace, .remember = false},
            &error),
        qPrintable(error));
    QVERIFY(writeFile(m_workspace + QStringLiteral("/one.line"), QByteArray(17 * 1024 * 1024, 'y')));

    const QString result = readRemote({{"file_path", "one.line"}});
    QVERIFY2(result.size() < 4096, qPrintable(QString::number(result.size())));
    QVERIFY2(result.startsWith(QStringLiteral("Error: line 0 ")), qPrintable(result));
}

/* Counterexample: remote list_files required `directory_path`, ignored
 * `pattern`, and cut the listing at `limit` without saying so. */
void Test::listFilesTakesTheLocalArgumentsAndSaysWhenItStops()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString dir = m_workspace + QStringLiteral("/listing");
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/sub")));
    for (const char *name : {"a.v", "b.v", "c.txt", "sub/d.v", ".hidden.v"}) {
        QVERIFY(writeFile(dir + QLatin1Char('/') + QString::fromLatin1(name), "x\n"));
    }

    const QString all = call(QStringLiteral("list_files"), {{"directory", "listing"}});
    QCOMPARE(
        all,
        QStringLiteral("Files in %1:\na.v\nb.v\nc.txt\nsub/")
            .arg(QFileInfo(dir).canonicalFilePath()));

    const QString cut = call(QStringLiteral("list_files"), {{"directory", "listing"}, {"limit", 2}});
    QVERIFY2(cut.contains(QStringLiteral("a.v\nb.v\n[truncated:")), qPrintable(cut));
    QVERIFY2(!cut.contains(QStringLiteral("c.txt")), qPrintable(cut));

    const QString deep = call(
        QStringLiteral("list_files"),
        {{"directory", "listing"}, {"pattern", "*.v"}, {"recursive", true}});
    QVERIFY2(deep.endsWith(QStringLiteral("a.v\nb.v\nsub/d.v")), qPrintable(deep));
    QVERIFY(!deep.contains(QStringLiteral(".hidden.v")));

    const QString legacy = call(QStringLiteral("list_files"), {{"directory_path", "listing"}});
    QCOMPARE(legacy, all);
}

/* Counterexample: remote edit_file refused every repeated string, so the
 * replace_all call the local tool takes had no remote equivalent. */
void Test::editFileReplacesAllLikeTheLocalTool()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString path = m_workspace + QStringLiteral("/many.txt");
    QVERIFY(writeFile(path, "x x x\n"));
    QVERIFY(!readRemote({{"file_path", "many.txt"}}).startsWith(QStringLiteral("Error")));

    const QString once = call(
        QStringLiteral("edit_file"),
        {{"file_path", "many.txt"}, {"old_string", "x"}, {"new_string", "y"}});
    QVERIFY2(once.startsWith(QStringLiteral("Error: old_string found 3 times")), qPrintable(once));

    const QString all = call(
        QStringLiteral("edit_file"),
        {{"file_path", "many.txt"}, {"old_string", "x"}, {"new_string", "y"}, {"replace_all", true}});
    QCOMPARE(
        all,
        QStringLiteral("Successfully edited file: %1 (3 replacement(s))")
            .arg(QFileInfo(path).canonicalFilePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("y y y\n"));
}

/* Counterexample: `~/x` resolved to `<cwd>/~/x`, a directory nobody has. */
void Test::tildeIsTheRemoteHome()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    QString home;
    QString err;
    QCOMPARE(
        m_runtime->remoteConnection()->sftp()->realPath(QStringLiteral("."), &home, &err),
        QSocSftpClient::Presence::Present);

    const QString listed = call(QStringLiteral("list_files"), {{"directory", "~"}});
    QVERIFY2(listed.startsWith(QStringLiteral("Files in %1:").arg(home)), qPrintable(listed));
    const QString missing = readRemote({{"file_path", "~/qsoc-no-such-file"}});
    QCOMPARE(missing, QStringLiteral("Error: File not found: %1/qsoc-no-such-file").arg(home));
}

/* Counterexample: on a Windows host the Git Bash spelling of a workspace
 * path, which is what bash prints, read as a path relative to the cwd. */
void Test::windowsSpellingsReachTheWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    const QString root = QFileInfo(m_workspace).canonicalFilePath();
    QSocMachine   host;
    host.kind     = QSocMachine::Kind::Windows;
    host.os       = QStringLiteral("Windows");
    host.rootFrom = root;
    host.rootTo   = QStringLiteral("/c/ws");
    m_runtime->remoteConnection()->setHostProbe(
        [host](QSocSshSession *, const QString &) { return host; });
    QVERIFY(bind(m_workspace));
    QVERIFY(writeFile(m_workspace + QStringLiteral("/win.txt"), "windows\n"));

    for (const char *spelling : {"/c/ws/win.txt", "/C/WS/win.txt"}) {
        const QString read = readRemote({{"file_path", spelling}});
        QCOMPARE(read, QStringLiteral("windows\n"));
    }
    const QString wrote
        = call(QStringLiteral("write_file"), {{"file_path", "/c/ws/fresh.txt"}, {"content", "new"}});
    QCOMPARE(wrote, QStringLiteral("Successfully wrote 3 bytes to: %1/fresh.txt").arg(root));
}

/* Counterexample: ${PROJECT} named the local project while ${CWD} named the
 * remote directory, so one skill body pointed at two different trees. */
void Test::skillPlaceholdersNameTheRemoteWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));

    QVERIFY(m_runtime->executeCommand(QStringLiteral("/probe")));
    const QStringList queued = m_runtime->takePendingAutoInputs();
    QCOMPARE(queued.size(), 1);
    const QString project = valueOf(queued.first(), QStringLiteral("PROJECT"));
    const QString cwd     = valueOf(queued.first(), QStringLiteral("CWD"));
    QVERIFY2(project.endsWith(QStringLiteral("/ws")), qPrintable(queued.first()));
    QCOMPARE(project, cwd);
    QVERIFY(!queued.first().contains(m_project));
}

/* Counterexample: the listing was built once at startup, so after `/ssh` the
 * prompt never listed the remote workspace's skills. */
void Test::skillListingFollowsTheBinding()
{
    REQUIRE_WORKSPACE_FIXTURE();
    const auto listing = [this] { return m_runtime->agent()->getConfig().skillListing; };
    QVERIFY2(listing().contains(QStringLiteral("**localonly**")), qPrintable(listing()));
    QVERIFY(!listing().contains(QStringLiteral("**probe**")));
    QVERIFY(listing().contains(QStringLiteral("**mine**")));

    QVERIFY(bind(m_workspace));
    QVERIFY(m_runtime->agent()->getToolRegistry()->getTool(QStringLiteral("skill_find")) != nullptr);
    QVERIFY2(listing().contains(QStringLiteral("skill_find(action")), qPrintable(listing()));
    QVERIFY(listing().contains(QStringLiteral("**probe** [remote]")));
    QVERIFY(listing().contains(QStringLiteral("**mine** [user]")));
    QVERIFY(listing().contains(QStringLiteral("**modelonly** [remote]")));
    QVERIFY(listing().contains(QStringLiteral("**localonly** [local]")));

    m_runtime->disconnectRemote();
    QVERIFY2(listing().contains(QStringLiteral("**localonly**")), qPrintable(listing()));
    QVERIFY(!listing().contains(QStringLiteral("**probe**")));
}

/* Counterexample: todo_* were missing in remote mode, although the prompt
 * said they wrote the remote workspace's .qsoc/todos.md. */
void Test::todoAddWritesTheRemoteWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString added = call(QStringLiteral("todo_add"), {{"title", "remote task"}});
    QVERIFY2(added.startsWith(QStringLiteral("Added todo #1")), qPrintable(added));
    QFile remote(m_workspace + QStringLiteral("/.qsoc/todos.md"));
    QVERIFY(remote.open(QIODevice::ReadOnly));
    QVERIFY(remote.readAll().contains("#1 remote task"));
    QVERIFY(!QFile::exists(m_project + QStringLiteral("/.qsoc/todos.md")));

    const QString updated = call(QStringLiteral("todo_update"), {{"id", 1}, {"status", "done"}});
    QVERIFY2(updated.startsWith(QStringLiteral("Updated todo #1")), qPrintable(updated));
    const QString listed = call(QStringLiteral("todo_list"), json::object());
    QVERIFY2(listed.contains(QStringLiteral("[x] 1. remote task")), qPrintable(listed));
    QVERIFY(call(QStringLiteral("todo_delete"), {{"id", 1}}).startsWith(QStringLiteral("Deleted")));

    m_runtime->disconnectRemote();
    QVERIFY(
        call(QStringLiteral("todo_list"), json::object()).startsWith(QStringLiteral("No todos")));
}

void Test::remoteProjectSkillIsListedAndReadable()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString listed = call(QStringLiteral("skill_find"), {{"action", "list"}});
    QVERIFY2(listed.contains(QStringLiteral("- probe [remote]")), qPrintable(listed));
    QVERIFY(listed.contains(QStringLiteral("- localonly [local]")));
    const QString read
        = call(QStringLiteral("skill_find"), {{"action", "read"}, {"query", "probe"}});
    QVERIFY2(read.contains(QStringLiteral("PROJECT=${PROJECT}")), qPrintable(read));
    QVERIFY(read.contains(m_workspace + QStringLiteral("/.qsoc/skills/probe/SKILL.md")));
}

/* A SKILL.md that links out of the workspace is not read, so a writer of the
 * remote tree cannot pull another file into the prompt through it. */
void Test::remoteSkillLinkOutsideTheWorkspaceIsRefused()
{
    REQUIRE_WORKSPACE_FIXTURE();
    const QString outside = m_fixture.workDir() + QStringLiteral("/secret.md");
    QVERIFY(writeFile(outside, "---\nname: leak\ndescription: leak probe\n---\nSECRET\n"));
    const QString dir = m_workspace + QStringLiteral("/.qsoc/skills/leak");
    QVERIFY(QDir().mkpath(dir));
    QFile::remove(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(QFile::link(outside, dir + QStringLiteral("/SKILL.md")));

    QVERIFY(bind(m_workspace));
    const QString listed = call(QStringLiteral("skill_find"), {{"action", "list"}});
    QVERIFY2(!listed.contains(QStringLiteral("leak")), qPrintable(listed));
    const QString read = call(QStringLiteral("skill_find"), {{"action", "read"}, {"query", "leak"}});
    QVERIFY2(!read.contains(QStringLiteral("SECRET")), qPrintable(read));
    QVERIFY(!m_runtime->agent()->getConfig().skillListing.contains(QStringLiteral("leak")));
    QVERIFY(QDir(dir).removeRecursively());
}

void Test::skillCreateWritesTheRemoteWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString made = call(
        QStringLiteral("skill_create"),
        {{"name", "made-remote"},
         {"description", "made remotely"},
         {"instructions", "body"},
         {"scope", "project"}});
    QVERIFY2(made.startsWith(QStringLiteral("Successfully created")), qPrintable(made));
    const QString file = m_workspace + QStringLiteral("/.qsoc/skills/made-remote/SKILL.md");
    QVERIFY(QFile::exists(file));
    QVERIFY(!QFile::exists(m_project + QStringLiteral("/.qsoc/skills/made-remote")));
    const QString again = call(
        QStringLiteral("skill_create"),
        {{"name", "made-remote"},
         {"description", "made remotely"},
         {"instructions", "body"},
         {"scope", "project"}});
    QVERIFY2(again.contains(QStringLiteral("already exists")), qPrintable(again));
    QVERIFY(call(QStringLiteral("skill_find"), {{"action", "list"}})
                .contains(QStringLiteral("made-remote")));
    QVERIFY(QDir(m_workspace + QStringLiteral("/.qsoc/skills/made-remote")).removeRecursively());
}

/* Counterexample: project memory in remote mode was written into the local
 * project's .qsoc/memory, mixing every remote workspace with the local one. */
void Test::projectMemoryStaysOnThisMachineKeyedByWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QVERIFY(bind(m_workspace));
    const QString wrote = call(
        QStringLiteral("memory_write"),
        {{"scope", "project"},
         {"name", "remote-fact"},
         {"type", "project"},
         {"description", "remote fact"},
         {"content", "keyed"}});
    QVERIFY2(!wrote.startsWith(QStringLiteral("Error")), qPrintable(wrote));

    const QString dir = m_runtime->memoryManager()->projectMemoryDir();
    QVERIFY2(!dir.startsWith(m_project), qPrintable(dir));
    QVERIFY2(!dir.startsWith(m_fixture.workDir()), qPrintable(dir));
    QVERIFY2(dir.startsWith(m_home), qPrintable(dir));
    QVERIFY(QFile::exists(dir + QStringLiteral("/remote-fact.md")));
    QVERIFY(!QFile::exists(m_project + QStringLiteral("/.qsoc/memory/remote-fact.md")));
    QVERIFY(!QDir(m_workspace + QStringLiteral("/.qsoc/memory")).exists());
#ifndef Q_OS_WIN
    QCOMPARE(
        QFileInfo(dir).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
        QFileDevice::Permissions());
    QCOMPARE(
        QFileInfo(dir + QStringLiteral("/remote-fact.md")).permissions()
            & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
        QFileDevice::Permissions());
#endif

    m_runtime->disconnectRemote();
    QCOMPARE(
        m_runtime->memoryManager()->projectMemoryDir(), m_project + QStringLiteral("/.qsoc/memory"));
}

void Test::switchingWorkspacesSeparatesProjectMemory()
{
    REQUIRE_WORKSPACE_FIXTURE();
    const QString other = m_fixture.workDir() + QStringLiteral("/ws2");
    QVERIFY(QDir().mkpath(other));
    const auto names = [this] {
        QStringList out;
        for (const auto &header : m_runtime->memoryManager()->scanHeaders("project")) {
            out << header.name;
        }
        return out;
    };

    QVERIFY(bind(m_workspace));
    QVERIFY(m_runtime->memoryManager()
                ->writeTopicFile("project", "first-ws", "project", "first", "one"));
    QVERIFY(names().contains(QStringLiteral("first-ws")));

    QVERIFY(bind(other));
    QVERIFY2(!names().contains(QStringLiteral("first-ws")), qPrintable(names().join(',')));
    QVERIFY(m_runtime->memoryManager()
                ->writeTopicFile("project", "second-ws", "project", "second", "two"));

    QVERIFY(bind(m_workspace));
    QVERIFY(names().contains(QStringLiteral("first-ws")));
    QVERIFY(!names().contains(QStringLiteral("second-ws")));

    m_runtime->disconnectRemote();
    QVERIFY(!names().contains(QStringLiteral("first-ws")));
    QVERIFY(!names().contains(QStringLiteral("second-ws")));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteworkspacetools.moc"
