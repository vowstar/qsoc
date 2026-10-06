// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/runtime/qsocagentruntime.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "common/qsocmachine.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QtTest>

using json = nlohmann::json;

/*
 * One path rule for the file tools: a relative path resolves against the
 * working directory bash runs in, `~` is the home directory of the machine
 * the tool acts on, and a Windows host takes every spelling its tools print.
 */

namespace Core = QSocToolFileCore;

namespace {

/* Point every config root at a private directory before QCoreApplication
 * exists, so the runtime never reads the developer's own config. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsoc_fileparity-")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        const QString qsocHome = root + QStringLiteral("/config/qsoc");
        QDir().mkpath(qsocHome);
        QDir().mkpath(root + QStringLiteral("/project/sub"));
        QDir().mkpath(root + QStringLiteral("/home"));
        qputenv("QSOC_HOME", qsocHome.toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("HOME", (root + QStringLiteral("/home")).toUtf8());
        QFile touch(QDir(qsocHome).filePath(QStringLiteral("qsoc.yml")));
        if (touch.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            touch.close();
        }
    }
    QString root;
};

const EnvBootstrap g_env;

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

QString project()
{
    return QFileInfo(g_env.root + QStringLiteral("/project")).canonicalFilePath();
}

QString call(QSocAgentRuntime &runtime, const QString &name, const json &args)
{
    QSocTool *tool = runtime.localToolRegistry()->getTool(name);
    return tool == nullptr ? QStringLiteral("(no %1 tool)").arg(name) : tool->execute(args);
}

/* A Windows host over a transport that is never dialled. */
struct WindowsBinding
{
    QObject              scratch;
    QSocRemoteConnection conn;

    WindowsBinding()
    {
        QSocMachine host;
        host.kind = QSocMachine::Kind::Windows;
        host.os   = QStringLiteral("Windows");
        conn.setHostProbe([host](QSocSshSession *, const QString &) { return host; });
        AgentRemoteState state;
        state.session            = new QSocSshSession(&scratch);
        state.sftp               = new QSocSftpClient(*state.session);
        state.targetKey          = QStringLiteral("user@host.invalid:22");
        state.endpointIdentity   = state.targetKey;
        state.workspace          = QStringLiteral("/C:/Work");
        state.canonicalWorkspace = state.workspace;
        state.workspaceTreeId    = QStringLiteral("tree-id");
        conn.adopt(std::move(state));
    }
};

/* A directory tree held in memory, listed the way a tool lists one. */
Core::ListDir fakeTree(const QHash<QString, QList<Core::ListEntry>> &tree)
{
    return [tree](const QString &dir, QList<Core::ListEntry> *entries, QString *error) {
        if (!tree.contains(dir)) {
            *error = QStringLiteral("permission denied");
            return false;
        }
        *entries = tree.value(dir);
        return true;
    };
}

Core::ListEntry file(const char *name)
{
    return {.name = QString::fromLatin1(name), .hidden = name[0] == '.'};
}

Core::ListEntry dir(const char *name)
{
    return {.name = QString::fromLatin1(name), .isDirectory = true, .hidden = name[0] == '.'};
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(writeFile(project() + QStringLiteral("/x.txt"), "top\n"));
        QVERIFY(writeFile(project() + QStringLiteral("/sub/x.txt"), "sub\n"));
        QVERIFY(writeFile(g_env.root + QStringLiteral("/home/h.txt"), "home\n"));
    }

    void cleanupTestCase() { QDir(g_env.root).removeRecursively(); }

    /* Counterexample: after a working-directory change, read_file("x.txt")
     * read the project's x.txt while bash `cat x.txt` read the new one. */
    void aRelativePathMeansWhatBashMeans()
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = project();
        QSocAgentRuntime runtime(options);
        QVERIFY(runtime.setWorkingDirectory(project() + QStringLiteral("/sub")));

        QCOMPARE(call(runtime, QStringLiteral("read_file"), {{"file_path", "x.txt"}}), "sub\n");
        const QString shell = call(runtime, QStringLiteral("bash"), {{"command", "cat x.txt"}});
        QVERIFY2(shell.contains(QStringLiteral("sub")), qPrintable(shell));
        QVERIFY2(!shell.contains(QStringLiteral("top")), qPrintable(shell));

        const QString listed = call(runtime, QStringLiteral("list_files"), json::object());
        QVERIFY2(
            listed.startsWith(QStringLiteral("Files in %1/sub:").arg(project())),
            qPrintable(listed));
        const QString wrote = call(
            runtime, QStringLiteral("write_file"), {{"file_path", "new.txt"}, {"content", "n"}});
        QVERIFY2(wrote.startsWith(QStringLiteral("Successfully wrote")), qPrintable(wrote));
        QVERIFY(QFile::exists(project() + QStringLiteral("/sub/new.txt")));
        QVERIFY(!QFile::exists(project() + QStringLiteral("/new.txt")));
    }

    /* Counterexample: `~/h.txt` resolved to `<project>/~/h.txt`. */
    void tildeIsTheLocalHome()
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = project();
        QSocAgentRuntime runtime(options);
        QCOMPARE(call(runtime, QStringLiteral("read_file"), {{"file_path", "~/h.txt"}}), "home\n");
        const QString listed = call(runtime, QStringLiteral("list_files"), {{"directory", "~"}});
        QVERIFY2(
            listed.startsWith(
                QStringLiteral("Files in %1:").arg(QDir::cleanPath(g_env.root + "/home"))),
            qPrintable(listed));
    }

    /* Counterexample: `C:\Work\a` and `/c/Work/a` on a Windows host were
     * taken as names relative to the working directory. */
    void aWindowsHostTakesEverySpelling()
    {
        WindowsBinding binding;
        const auto    *path = binding.conn.path();
        for (const char *spelling : {"C:\\Work\\a", "C:/Work/a", "/c/Work/a", "/C:/Work/a"}) {
            QCOMPARE(path->normalize(QString::fromLatin1(spelling)), QStringLiteral("/C:/Work/a"));
        }
        QCOMPARE(path->normalize(QStringLiteral("sub\\b")), QStringLiteral("/C:/Work/sub/b"));
        QCOMPARE(path->resolveCwdRequest(QStringLiteral("c:\\work\\sub")), "/C:/work/sub");
    }

    void tildeExpandsOnlyAsAHomePrefix()
    {
        QCOMPARE(Core::expandHome("~", "/h"), QStringLiteral("/h"));
        QCOMPARE(Core::expandHome("~/a", "/h"), QStringLiteral("/h/a"));
        QCOMPARE(Core::expandHome("~\\a", "/C:/Users/u"), QStringLiteral("/C:/Users/u/a"));
        QCOMPARE(Core::expandHome("~/a", "/"), QStringLiteral("/a"));
        QCOMPARE(Core::expandHome("~user/a", "/h"), QStringLiteral("~user/a"));
        QCOMPARE(Core::expandHome("a/~", "/h"), QStringLiteral("a/~"));
        QCOMPARE(Core::expandHome("~/a", QString()), QStringLiteral("~/a"));
    }

    void aLocalPathFollowsOneRule()
    {
        QCOMPARE(Core::localPath("x", "/w/sub", "/h", false), QStringLiteral("/w/sub/x"));
        QCOMPARE(Core::localPath("../x", "/w/sub", "/h", false), QStringLiteral("/w/x"));
        QCOMPARE(Core::localPath("", "/w", "/h", false), QStringLiteral("/w"));
        QCOMPARE(Core::localPath("~/x", "/w", "/h", false), QStringLiteral("/h/x"));
        QCOMPARE(Core::localPath("/abs/x", "/w", "/h", false), QStringLiteral("/abs/x"));
        for (const char *spelling : {"C:\\w\\a", "C:/w/a", "/c/w/a", "/C:/w/a"}) {
            QCOMPARE(
                Core::localPath(QString::fromLatin1(spelling), "D:/base", "C:/Users/u", true),
                QStringLiteral("C:/w/a"));
        }
        QCOMPARE(Core::localPath("sub\\a", "D:/base", "C:/Users/u", true), "D:/base/sub/a");
        QCOMPARE(Core::localPath("~\\a", "D:/base", "C:/Users/u", true), "C:/Users/u/a");
    }

    void theSftpSpellingOfAWindowsPath()
    {
        using Ctx = QSocRemotePathContext;
        for (const char *spelling : {"C:\\w\\a", "C:/w/a", "/c/w/a", "/C:/w/a", "c:/w/a"}) {
            QCOMPARE(Ctx::windowsSftpPath(QString::fromLatin1(spelling), {}, {}), "/C:/w/a");
        }
        QCOMPARE(Ctx::windowsSftpPath("C:", {}, {}), QStringLiteral("/C:/"));
        QCOMPARE(Ctx::windowsSftpPath("sub\\a", {}, {}), QStringLiteral("sub/a"));
        QCOMPARE(Ctx::windowsSftpPath("/tmp/a", {}, {}), QStringLiteral("/tmp/a"));
        QCOMPARE(
            Ctx::windowsSftpPath("/HOME/u/w/a", "/C:/msys64/home/u/w", "/home/u/w"),
            QStringLiteral("/C:/msys64/home/u/w/a"));
        QVERIFY(Ctx::isWithinAny("/C:/WORK/a", {"/C:/Work"}, Qt::CaseInsensitive));
        QVERIFY(!Ctx::isWithinAny("/C:/WORK/a", {"/C:/Work"}, Qt::CaseSensitive));
        QVERIFY(!Ctx::isWithinAny("/C:/Workshop", {"/C:/Work"}, Qt::CaseInsensitive));
    }

    void aWindowsHostComparesWithoutCase()
    {
        WindowsBinding binding;
        QCOMPARE(binding.conn.path()->pathCase(), Qt::CaseInsensitive);
        QVERIFY(binding.conn.path()->isWritable(QStringLiteral("/C:/WORK/a")));
    }

    void aListingIsSortedAndSaysWhereItStopped()
    {
        const auto tree = fakeTree(
            {{"/r", {file("b.v"), dir("sub"), file("a.v"), file(".h.v"), dir(".git"), dir("no")}},
             {"/r/sub", {file("c.v"), file("d.txt")}},
             {"/r/.git", {file("e.v")}}});
        Core::ListCall call;
        QCOMPARE(
            Core::listFiles("/r", call, true, tree),
            QStringLiteral("Files in /r:\na.v\nb.v\nno/\nsub/"));

        call.recursive = true;
        call.pattern   = QStringLiteral("*.V");
        QCOMPARE(
            Core::listFiles("/r", call, false, tree),
            QStringLiteral("Files in /r:\na.v\nb.v\nsub/c.v\n[unreadable: no/: permission denied]"));
        QCOMPARE(
            Core::listFiles("/r", call, true, tree),
            QStringLiteral("Files in /r:\n[unreadable: no/: permission denied]"));

        call.pattern       = QStringLiteral("*.v");
        call.includeHidden = true;
        call.limit         = 3;
        const QString cut  = Core::listFiles("/r", call, true, tree);
        QVERIFY2(
            cut.startsWith(QStringLiteral(
                "Files in /r:\n.git/e.v\n.h.v\na.v\n[truncated: listing stopped at 3 entries")),
            qPrintable(cut));

        call = Core::ListCall{};
        QCOMPARE(
            Core::listFiles("/r/sub", call, true, fakeTree({{"/r/sub", {}}})),
            "No files found in: /r/sub");
        QCOMPARE(Core::listFiles("/x", call, true, tree), QStringLiteral("Error: permission denied"));
    }

    void anEditCallIsCheckedOnceForBothSides()
    {
        QCOMPARE(
            Core::editCall({{"file_path", "f"}, {"old_string", "a"}, {"new_string", "a"}}).error,
            QStringLiteral("Error: old_string and new_string are identical"));
        QCOMPARE(
            Core::editCall({{"file_path", "f"}, {"old_string", ""}, {"new_string", "a"}}).error,
            QStringLiteral("Error: old_string must not be empty"));
        QCOMPARE(
            Core::editCall({{"file_path", "f"}, {"old_string", "a"}}).error,
            QStringLiteral("Error: new_string is required"));

        Core::EditCall call = Core::editCall(
            {{"file_path", "f"}, {"old_string", "a"}, {"new_string", "b"}});
        QCOMPARE(Core::applyEdit("a a", call).error.left(29), "Error: old_string found 2 tim");
        QCOMPARE(Core::applyEdit("xa", call).content, QStringLiteral("xb"));
        call.replaceAll                = true;
        const Core::EditOutcome edited = Core::applyEdit("a a", call);
        QCOMPARE(edited.content, QStringLiteral("b b"));
        QCOMPARE(edited.count, 2);
        QCOMPARE(
            Core::applyEdit("x", call).error,
            QStringLiteral("Error: old_string not found in file: f"));
    }

    void aReadWindowEndsWithOneMarker()
    {
        Core::Reader reader({.offset = 1, .maxLines = 2});
        QVERIFY(!reader.feed("l0\r\nl1\r\nl2\r\nl3\r\n"));
        QCOMPARE(
            reader.result("/f", nullptr),
            QStringLiteral(
                "l1\nl2\n[truncated: more lines follow; rerun with offset=3 to continue]\n"));
        QVERIFY(!reader.wholeFile());

        Core::Reader whole({});
        QVERIFY(whole.feed("a\nb"));
        whole.finish();
        QCOMPARE(whole.result("/f", nullptr), QStringLiteral("a\nb\n"));
        QCOMPARE(*whole.wholeFile(), QByteArray("a\nb"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocfiletoolparity.moc"
