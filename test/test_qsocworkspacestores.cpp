// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemorymanager.h"
#include "agent/qsocworkspacefs.h"
#include "agent/tool/qsoctoolskill.h"
#include "agent/tool/qsoctooltodo.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"
#include "smt/qsocsmtservice.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Point every config root at a private directory before QCoreApplication
 * exists, so nothing reads or writes the developer's own config. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsoc_wsstores-")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        QDir().mkpath(root + QStringLiteral("/config"));
        QDir().mkpath(root + QStringLiteral("/project"));
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("HOME", root.toUtf8());
        qunsetenv("QSOC_HOME");
        qunsetenv("QSOC_SKILLS_PATH");
    }
    QString root;
};

const EnvBootstrap g_env;

/* A workspace held in memory: one map from relative path to content. */
class FakeWorkspaceFs : public QSocWorkspaceFs
{
public:
    QString root() const override { return QStringLiteral("/fake/ws"); }

    Result read(const QString &path, QByteArray *bytes, QString *error) override
    {
        if (onRead) {
            const auto callback = std::move(onRead);
            onRead              = {};
            callback();
        }
        if (failing.contains(path)) {
            *error = QStringLiteral("link down");
            return Result::Failed;
        }
        if (!files.contains(path)) {
            return Result::Absent;
        }
        *bytes = files.value(path);
        return Result::Ok;
    }

    Result list(const QString &path, QList<Entry> *entries, QString *) override
    {
        QSet<QString> names;
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            if (it.key().startsWith(path + QLatin1Char('/'))) {
                names.insert(it.key().mid(path.size() + 1).section(QLatin1Char('/'), 0, 0));
            }
        }
        if (names.isEmpty()) {
            return Result::Absent;
        }
        entries->clear();
        for (const QString &name : names) {
            entries->append({name, files.contains(path + QLatin1Char('/') + name), -1});
        }
        return Result::Ok;
    }

    Result stat(const QString &path, Entry *entry, QString *) override
    {
        if (!files.contains(path)) {
            return Result::Absent;
        }
        *entry = {path, true, files.value(path).size()};
        return Result::Ok;
    }

    bool write(const QString &path, const QByteArray &bytes, QString *error) override
    {
        if (failingWrites.contains(path)) {
            *error = QStringLiteral("write refused");
            return false;
        }
        ++writes;
        files.insert(path, bytes);
        return true;
    }

    QHash<QString, QByteArray> files;
    QSet<QString>              failing;
    QSet<QString>              failingWrites;
    int                        writes = 0;
    std::function<void()>      onRead;
};

QString run(QSocTool &tool, const json &args)
{
    return tool.execute(args);
}

QByteArray skillFile(const QString &name)
{
    return "---\nname: " + name.toUtf8() + "\ndescription: " + name.toUtf8() + " probe\n---\nBODY "
           + name.toUtf8() + "\n";
}

bool writeLocal(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanupTestCase() { QDir(g_env.root).removeRecursively(); }

    void relativePathsStayInsideTheWorkspace()
    {
        QVERIFY(QSocWorkspaceFs::isContainedRelative(QStringLiteral(".qsoc/todos.md")));
        QVERIFY(QSocWorkspaceFs::isContainedRelative(QStringLiteral("a/b.md")));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QString()));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QStringLiteral("/etc/passwd")));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QStringLiteral("../x")));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QStringLiteral(".qsoc/../../x")));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QStringLiteral("a//b")));
        QVERIFY(!QSocWorkspaceFs::isContainedRelative(QStringLiteral("a\\b")));
    }

    void todoToolsKeepTheListInTheWorkspace()
    {
        FakeWorkspaceFs    fs;
        QSocToolTodoAdd    add(nullptr, &fs);
        QSocToolTodoList   list(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QSocToolTodoDelete del(nullptr, &fs);

        QVERIFY(run(list, json::object()).startsWith(QStringLiteral("No todos")));
        QVERIFY(run(add, {{"title", "one"}}).startsWith(QStringLiteral("Added todo #1")));
        QVERIFY(run(add, {{"title", "two"}, {"priority", "high"}})
                    .startsWith(QStringLiteral("Added todo #2")));
        QVERIFY(fs.files.value(QStringLiteral(".qsoc/todos.md")).contains("#2 two"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.hwm")), QByteArray("2"));

        QVERIFY(run(update, {{"id", 1}, {"status", "done"}}).startsWith(QStringLiteral("Updated")));
        QVERIFY(run(del, {{"id", 2}}).startsWith(QStringLiteral("Deleted")));
        /* A deleted id is never handed out again. */
        QVERIFY(run(add, {{"title", "three"}}).startsWith(QStringLiteral("Added todo #3")));
        const QString listed = run(list, {{"filter", "done"}});
        QVERIFY2(listed.contains(QStringLiteral("[x] 1. one")), qPrintable(listed));
        QVERIFY(!listed.contains(QStringLiteral("three")));
    }

    /* A read that failed must not look like an empty list, or the next save
     * replaces todos nobody read. */
    void failedTodoReadNeverOverwrites()
    {
        FakeWorkspaceFs fs;
        fs.files.insert(QStringLiteral(".qsoc/todos.md"), "- [ ] #7 kept\n");
        fs.failing.insert(QStringLiteral(".qsoc/todos.md"));
        QSocToolTodoAdd    add(nullptr, &fs);
        QSocToolTodoDelete del(nullptr, &fs);
        QSocToolTodoList   list(nullptr, &fs);

        QVERIFY(run(add, {{"title", "new"}}).startsWith(QStringLiteral("Error: link down")));
        QVERIFY(run(del, {{"id", 7}}).startsWith(QStringLiteral("Error: link down")));
        QVERIFY(run(list, json::object()).startsWith(QStringLiteral("Error: link down")));
        QCOMPARE(fs.writes, 0);
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), QByteArray("- [ ] #7 kept\n"));
    }

    void smtChecksTheSameDependencyGraph_data()
    {
        QTest::addColumn<bool>("cycle");
        QTest::newRow("diamond") << false;
        QTest::newRow("cycle") << true;
    }

    void smtChecksTheSameDependencyGraph()
    {
        QFETCH(bool, cycle);
        if (!QSocSmtService::supported())
            QSKIP("SMT worker is unavailable");
        FakeWorkspaceFs     fs;
        QByteArray          content;
        QList<QSocTodoItem> items;
        for (int id = 1; id <= 4; ++id) {
            QSocTodoItem item;
            item.id    = id;
            item.title = "task";
            if (id == 2 || id == 3)
                item.dependsOn = {1};
            if (id == 4)
                item.dependsOn = {2, 3};
            if (id == 1 && cycle)
                item.dependsOn = {4};
            items.append(item);
            json metadata = {{"depends_on", json::array()}};
            for (int dependency : item.dependsOn)
                metadata["depends_on"].push_back(dependency);
            content += "- [ ] #" + QByteArray::number(id) + " task\n  <!-- qsoc-todo: "
                       + QByteArray::fromStdString(metadata.dump()) + " -->\n";
        }
        fs.files.insert(QStringLiteral(".qsoc/todos.md"), content);
        QString error;
        QVERIFY(fs.todos()->refresh(&error));
        QCOMPARE(QSocTodoModel::dependencyError(items).isEmpty(), !cycle);
        const auto answer = QSocSmtService::solve(
            {{"smtlib", fs.todos()->dependencySmt()}, {"timeout_ms", 1000}},
            {},
            QStringLiteral(QSOC_SMT_WORKER_PATH));
        QCOMPARE(answer.value("execution").toString(), QStringLiteral("completed"));
        QCOMPARE(
            answer.value("solver_status").toString(),
            cycle ? QStringLiteral("unsat") : QStringLiteral("sat"));
    }

    void prerequisitesMustFinishBeforeStartingOrCompleting()
    {
        FakeWorkspaceFs fs;
        auto           *model = fs.todos();
        QVERIFY(model->execute("todo_add", {{"title", "first"}}).startsWith("Added"));
        QVERIFY(model->execute("todo_add", {{"title", "second"}, {"depends_on", {1}}})
                    .startsWith("Added"));
        for (const auto *status : {"in_progress", "done"})
            QVERIFY(model->execute("todo_update", {{"id", 2}, {"status", status}})
                        .contains("Finish prerequisite #1"));
        QVERIFY(
            model->execute("todo_update", {{"id", 1}, {"status", "done"}}).startsWith("Updated"));
        QVERIFY(model->execute("todo_update", {{"id", 2}, {"status", "in_progress"}})
                    .startsWith("Updated"));
    }

    void todoStateAndCriteriaSurviveReload()
    {
        FakeWorkspaceFs    fs;
        QSocToolTodoAdd    add(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QSocToolTodoList   list(nullptr, &fs);
        QVERIFY(run(add, {{"title", "first"}, {"description", "check <input>\nthen verify"}})
                    .startsWith("Added"));
        QVERIFY(run(update, {{"id", 1}, {"status", "in_progress"}}).startsWith("Updated"));
        QVERIFY(run(add, {{"title", "second"}, {"depends_on", {1}}}).startsWith("Added"));
        const auto listed = run(list, json::object());
        QVERIFY(listed.contains("[in_progress]"));
        QCOMPARE(
            fs.todos()->items().first().description, QStringLiteral("check <input>\nthen verify"));
        QCOMPARE(fs.todos()->items().last().dependsOn, QList<int>{1});
        fs.todos()->setScope({});
        const auto rows = fs.todos()->listTasks();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.last().summary, QStringLiteral("Waiting for #1"));
        QVERIFY(run(update, {{"id", 1}, {"status", "done"}}).startsWith("Updated"));
        QCOMPARE(fs.todos()->listTasks().last().summary, QStringLiteral("Ready"));
        QCOMPARE(fs.todos()->listTasks().last().status, QSocTask::Status::Pending);
    }

    void todoCyclesAndMissingDependenciesDoNotOverwrite()
    {
        FakeWorkspaceFs    fs;
        QSocToolTodoAdd    add(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QSocToolTodoDelete remove(nullptr, &fs);
        QVERIFY(run(add, {{"title", "first"}}).startsWith("Added"));
        QVERIFY(run(add, {{"title", "second"}, {"depends_on", {1}}}).startsWith("Added"));
        const auto original = fs.files.value(QStringLiteral(".qsoc/todos.md"));
        QVERIFY(run(update, {{"id", 1}, {"depends_on", {2}}}).contains("Dependency cycle:"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), original);
        QVERIFY(run(update, {{"id", 1}, {"depends_on", {1}}}).contains("Dependency cycle:"));
        QVERIFY(run(update, {{"id", 2}, {"depends_on", {99}}}).contains("missing todo #99"));
        QVERIFY(run(update, {{"id", 2}, {"depends_on", {1, 1}}}).contains("repeats dependency"));
        QVERIFY(run(remove, {{"id", 1}}).contains("missing todo #1"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), original);
        QVERIFY(run(update, {{"id", 2}, {"depends_on", json::array()}}).startsWith("Updated"));
        QVERIFY(run(remove, {{"id", 1}}).startsWith("Deleted"));
    }

    void importedCyclesRemainReadableAndCanBeRepaired()
    {
        FakeWorkspaceFs fs;
        QByteArray      content;
        for (int id = 1; id <= 4; ++id) {
            const int dependency = id % 2 == 0 ? id - 1 : id + 1;
            json      metadata   = {{"depends_on", {dependency}}};
            if (id == 1)
                metadata["depends_on"].push_back(3);
            content += "- [ ] #" + QByteArray::number(id) + " task\n  <!-- qsoc-todo: "
                       + QByteArray::fromStdString(metadata.dump()) + " -->\n";
        }
        fs.files.insert(QStringLiteral(".qsoc/todos.md"), content);
        QSocToolTodoList   list(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QVERIFY(run(list, json::object()).contains("task"));
        fs.todos()->setScope({});
        QCOMPARE(fs.todos()->listTasks().first().id, QStringLiteral("error"));
        QVERIFY(fs.todos()->tailFor("error", 4096).contains("Dependency cycle:"));
        QVERIFY(run(update, {{"id", 1}, {"depends_on", {3}}, {"status", "done"}})
                    .contains("Finish prerequisite #3"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), content);
        QVERIFY(run(update, {{"id", 1}, {"depends_on", json::array()}}).startsWith("Updated"));
        QVERIFY(fs.todos()->listTasks().first().summary.contains("Dependency cycle:"));
        QVERIFY(run(update, {{"id", 3}, {"depends_on", json::array()}}).startsWith("Updated"));
        QCOMPARE(fs.todos()->listTasks().size(), 4);
        QVERIFY(!fs.todos()->listTasks().first().summary.contains("cycle"));
    }

    void plansStaySeparateAndChildrenCannotEditTheirParent()
    {
        FakeWorkspaceFs fs;
        auto           *model = fs.todos();
        QVERIFY(model->execute("todo_add", {{"title", "parent"}}, "session-a").startsWith("Added"));
        QVERIFY(model->execute("todo_add", {{"title", "child"}}, "session-b").startsWith("Added"));
        model->setScope("session-a");
        QCOMPARE(model->listTasks().size(), 1);
        QCOMPARE(model->listTasks().first().label, QStringLiteral("parent"));
        QVERIFY(model->execute("todo_update", {{"id", 1}, {"status", "done"}}, "session-b", true)
                    .contains("owning agent"));
        QVERIFY(model->execute("todo_update", {{"id", 2}, {"depends_on", {1}}}, "session-b", true)
                    .contains("missing todo"));
        QCOMPARE(model->listTasks().first().status, QSocTask::Status::Pending);
    }

    void legacyTodosRemainReadableAndUnknownMetadataIsPreserved()
    {
        FakeWorkspaceFs fs;
        fs.files.insert(QStringLiteral(".qsoc/todos.md"), "- [ ] legacy\n- [x] #1 stable\n");
        QSocToolTodoList   list(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QVERIFY(run(list, json::object()).contains("[x] 1. stable"));
        QCOMPARE(fs.todos()->items().first().id, 2);
        const json metadata = {{"future_field", true}};
        const auto original = QByteArray("- [ ] #1 stable\n  <!-- qsoc-todo: ")
                              + QByteArray::fromStdString(metadata.dump()) + " -->\n";
        fs.files[QStringLiteral(".qsoc/todos.md")] = original;
        QVERIFY(run(update, {{"id", 1}, {"status", "done"}}).contains("Unsupported todo metadata"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), original);
    }

    void aliasesOfOneWorkspaceCannotOverwriteAConcurrentUpdate()
    {
        FakeWorkspaceFs first;
        FakeWorkspaceFs second;
        QSocToolTodoAdd addFirst(nullptr, &first);
        QSocToolTodoAdd addSecond(nullptr, &second);
        QString         nested;
        first.onRead = [&] { nested = run(addSecond, {{"title", "nested"}}); };
        QVERIFY(run(addFirst, {{"title", "first"}}).startsWith("Added"));
        QVERIFY(nested.contains("storage is busy"));
        QCOMPARE(second.writes, 0);
        QVERIFY(run(addSecond, {{"title", "later"}}).startsWith("Added"));
    }

    void failedTodoWriteKeepsThePreviousPlan()
    {
        FakeWorkspaceFs    fs;
        QSocToolTodoAdd    add(nullptr, &fs);
        QSocToolTodoUpdate update(nullptr, &fs);
        QVERIFY(run(add, {{"title", "kept"}}).startsWith("Added"));
        const auto original = fs.files.value(QStringLiteral(".qsoc/todos.md"));
        fs.failingWrites.insert(QStringLiteral(".qsoc/todos.md"));
        QVERIFY(run(update, {{"id", 1}, {"status", "done"}}).contains("write refused"));
        QCOMPARE(fs.todos()->items().first().status, QStringLiteral("pending"));
        QCOMPARE(fs.files.value(QStringLiteral(".qsoc/todos.md")), original);
    }

    void workspaceSkillsLayerOverTheLocalProject()
    {
        const QString project = g_env.root + QStringLiteral("/project");
        QVERIFY(writeLocal(
            project + QStringLiteral("/.qsoc/skills/localonly/SKILL.md"), skillFile("localonly")));
        QVERIFY(writeLocal(
            project + QStringLiteral("/.qsoc/skills/shared/SKILL.md"), skillFile("shared")));
        QVERIFY(writeLocal(
            g_env.root + QStringLiteral("/config/qsoc/skills/mine/SKILL.md"), skillFile("mine")));
        QSocProjectManager projectManager;
        projectManager.setProjectPath(project);

        FakeWorkspaceFs fs;
        fs.files.insert(QStringLiteral(".qsoc/skills/alpha/SKILL.md"), skillFile("alpha"));
        fs.files.insert(QStringLiteral(".qsoc/skills/mine/SKILL.md"), skillFile("mine"));
        fs.files.insert(QStringLiteral(".qsoc/skills/shared/SKILL.md"), skillFile("shared"));
        fs.files.insert(QStringLiteral(".qsoc/skills/broken/SKILL.md"), "no frontmatter\n");

        const auto scopes = [](const QSocToolSkillFind &finder) {
            QStringList seen;
            for (const auto &skill : finder.scanAllSkills()) {
                seen << skill.name + QLatin1Char(':') + skill.scope;
            }
            return seen;
        };

        /* Counterexample: the workspace layer replaced the local project's
         * skills instead of sitting above them. */
        QSocToolSkillFind remote(nullptr, &projectManager, &fs);
        const QStringList seen = scopes(remote);
        QVERIFY2(seen.contains(QStringLiteral("alpha:remote")), qPrintable(seen.join(',')));
        QVERIFY2(seen.contains(QStringLiteral("localonly:local")), qPrintable(seen.join(',')));
        QVERIFY(seen.contains(QStringLiteral("shared:remote")));
        QVERIFY(!seen.contains(QStringLiteral("shared:local")));
        QVERIFY(seen.contains(QStringLiteral("mine:remote")));
        QVERIFY(!seen.contains(QStringLiteral("mine:user")));

        bool sawBroken = false;
        for (const auto &skill : remote.scanAllSkillFiles()) {
            sawBroken = sawBroken || (skill.name.isEmpty() && !skill.parseError.isEmpty());
        }
        QVERIFY(sawBroken);

        const QString read = run(remote, {{"action", "read"}, {"query", "alpha"}});
        QVERIFY2(read.contains(QStringLiteral("BODY alpha")), qPrintable(read));
        QVERIFY(read.contains(QStringLiteral("/fake/ws/.qsoc/skills/alpha/SKILL.md")));
        const QString listed = run(remote, {{"action", "list"}, {"scope", "remote"}});
        QVERIFY2(listed.contains(QStringLiteral("- alpha [remote]")), qPrintable(listed));
        QVERIFY(!listed.contains(QStringLiteral("localonly")));
        const QString all = run(remote, {{"action", "list"}});
        QVERIFY2(all.contains(QStringLiteral("- localonly [local]")), qPrintable(all));
        /* `project` names the bound workspace's layer in either mode. */
        const QString bound = run(remote, {{"action", "list"}, {"scope", "project"}});
        QVERIFY2(bound.contains(QStringLiteral("- alpha [remote]")), qPrintable(bound));
        QVERIFY2(!bound.contains(QStringLiteral("localonly")), qPrintable(bound));

        const QSocToolSkillFind local(nullptr, &projectManager);
        const QStringList       localSeen = scopes(local);
        QVERIFY(localSeen.contains(QStringLiteral("localonly:local")));
        QVERIFY(localSeen.contains(QStringLiteral("mine:user")));
        QVERIFY(!localSeen.join(',').contains(QStringLiteral("alpha")));
        QSocToolSkillFind localFinder(nullptr, &projectManager);
        const QString     localBound = run(localFinder, {{"action", "list"}, {"scope", "project"}});
        QVERIFY2(localBound.contains(QStringLiteral("- localonly [local]")), qPrintable(localBound));
        QVERIFY2(!localBound.contains(QStringLiteral("mine")), qPrintable(localBound));
    }

    void skillCreateWritesProjectSkillsToTheWorkspace()
    {
        FakeWorkspaceFs     fs;
        QSocToolSkillCreate create(nullptr, nullptr, &fs);
        const json          args{
            {"name", "made"},
            {"description", "made here"},
            {"instructions", "do"},
            {"scope", "project"}};
        QVERIFY(run(create, args).startsWith(QStringLiteral("Successfully created")));
        const QByteArray body = fs.files.value(QStringLiteral(".qsoc/skills/made/SKILL.md"));
        QVERIFY2(body.startsWith("---\nname: made\ndescription: made here\n"), body.constData());
        QVERIFY(body.endsWith("do\n"));
        QVERIFY(run(create, args).contains(QStringLiteral("already exists")));

        json user     = args;
        user["scope"] = "user";
        QVERIFY(run(create, user).startsWith(QStringLiteral("Successfully created")));
        QVERIFY(QFile::exists(g_env.root + QStringLiteral("/config/qsoc/skills/made/SKILL.md")));
        QCOMPARE(fs.writes, 1);
    }

    /* Counterexample: in remote mode skill_create could reach only the remote
     * workspace and the user dir, never this machine's project. */
    void skillCreateLocalScopeWritesThisMachinesProject()
    {
        const QString      project = g_env.root + QStringLiteral("/create-local");
        QSocProjectManager projectManager;
        projectManager.setProjectPath(project);
        FakeWorkspaceFs     fs;
        QSocToolSkillCreate create(nullptr, &projectManager, &fs);
        QSocToolSkillFind   find(nullptr, &projectManager, &fs);
        json                args{
            {"name", "here"},
            {"description", "made locally"},
            {"instructions", "do"},
            {"scope", "local"}};
        QVERIFY(run(create, args).startsWith(QStringLiteral("Successfully created")));
        QVERIFY(QFile::exists(project + QStringLiteral("/.qsoc/skills/here/SKILL.md")));
        QCOMPARE(fs.writes, 0);
        QVERIFY2(
            run(find, {{"action", "list"}}).contains(QStringLiteral("- here [local]")),
            qPrintable(run(find, {{"action", "list"}})));

        args["name"]  = "there";
        args["scope"] = "project";
        QVERIFY(run(create, args).startsWith(QStringLiteral("Successfully created")));
        QVERIFY(fs.files.contains(QStringLiteral(".qsoc/skills/there/SKILL.md")));
        QVERIFY(!QFile::exists(project + QStringLiteral("/.qsoc/skills/there/SKILL.md")));
        QVERIFY(run(find, {{"action", "list"}}).contains(QStringLiteral("- there [remote]")));

        /* In local mode `project` and `local` are the same place. */
        QSocToolSkillCreate localCreate(nullptr, &projectManager);
        args["name"] = "both";
        QVERIFY(run(localCreate, args).startsWith(QStringLiteral("Successfully created")));
        QVERIFY(QFile::exists(project + QStringLiteral("/.qsoc/skills/both/SKILL.md")));
    }

    void remoteProjectMemoryIsKeyedAndPrivate()
    {
        const QString      project = g_env.root + QStringLiteral("/project");
        QSocProjectManager projectManager;
        projectManager.setProjectPath(project);
        QSocMemoryManager memory(nullptr, &projectManager);
        const QString     local = project + QStringLiteral("/.qsoc/memory");
        QCOMPARE(memory.projectMemoryDir(), local);

        memory.setRemoteWorkspace(QStringLiteral("user@host-a key-a"), QStringLiteral("/srv/ws"));
        const QString first = memory.projectMemoryDir();
        QVERIFY(first.startsWith(g_env.root + QStringLiteral("/config/qsoc/remote-memory/")));
        QVERIFY(memory.writeTopicFile("project", "fact-a", "project", "a", "in a"));
        QVERIFY(QFile::exists(first + QStringLiteral("/fact-a.md")));
        QVERIFY(!QFile::exists(local + QStringLiteral("/fact-a.md")));
#ifdef Q_OS_UNIX
        const auto groupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                  | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                  | QFileDevice::WriteOther | QFileDevice::ExeOther;
        for (const QString &path :
             {QFileInfo(first).absolutePath(),
              first,
              first + QStringLiteral("/fact-a.md"),
              first + QStringLiteral("/MEMORY.md")}) {
            QVERIFY2(!(QFileInfo(path).permissions() & groupOrOther), qPrintable(path));
        }
#endif

        /* Another host with the same path, and another path on the same host. */
        memory.setRemoteWorkspace(QStringLiteral("user@host-b key-b"), QStringLiteral("/srv/ws"));
        QVERIFY(memory.projectMemoryDir() != first);
        QVERIFY(memory.scanHeaders("project").isEmpty());
        memory.setRemoteWorkspace(QStringLiteral("user@host-a key-a"), QStringLiteral("/srv/other"));
        QVERIFY(memory.projectMemoryDir() != first);
        QVERIFY(memory.readTopicFile("project", "fact-a").isEmpty());

        memory.setRemoteWorkspace(QStringLiteral("user@host-a key-a"), QStringLiteral("/srv/ws"));
        QVERIFY(memory.readTopicFile("project", "fact-a").contains(QStringLiteral("in a")));

        memory.setRemoteWorkspace({}, {});
        QCOMPARE(memory.projectMemoryDir(), local);
        QVERIFY(memory.readTopicFile("project", "fact-a").isEmpty());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocworkspacestores.moc"
