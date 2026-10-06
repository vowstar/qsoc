// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/runtime/qsocagentruntime.h"
#include "common/qsocmachine.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Point every config root at a private directory before QCoreApplication
 * exists, so the runtime never reads the developer's own config. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsoc_rtremote-")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        const QString qsocHome = root + QStringLiteral("/config/qsoc");
        QDir().mkpath(qsocHome);
        QDir().mkpath(root + QStringLiteral("/project"));
        qputenv("QSOC_HOME", qsocHome.toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("HOME", root.toUtf8());
        QFile touch(QDir(qsocHome).filePath(QStringLiteral("qsoc.yml")));
        touch.open(QIODevice::WriteOnly | QIODevice::Truncate);
        touch.close();
    }
    QString root;
};

const EnvBootstrap g_env;

class StubTool : public QSocTool
{
public:
    StubTool(QString name, QObject *parent = nullptr)
        : QSocTool(parent)
        , name_(std::move(name))
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("stub ") + name_; }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        if (body)
            body(currentCallContext());
        return QStringLiteral("ok");
    }
    void abort() override { ++aborts; }

    std::function<void(QSocToolCallContext *)> body;
    int                                        aborts = 0;

private:
    QString name_;
};

QStringList definitionNames(const QSocToolRegistry &registry)
{
    QStringList names;
    for (const auto &def : registry.getToolDefinitions())
        names << QString::fromStdString(def["function"]["name"].get<std::string>());
    return names;
}

/* Local workspace tools with no remote replacement yet. */
bool localOnly(const QString &name)
{
    for (const char *family : {"project_", "module_", "bus_", "generate_"}) {
        if (name.startsWith(QLatin1String(family)))
            return true;
    }
    return name == QStringLiteral("lsp");
}

/* Workspace tools a host without a usable shell does not get. */
bool execTool(const QString &name)
{
    return name == QStringLiteral("bash") || name == QStringLiteral("bash_manage")
           || name == QStringLiteral("monitor") || name == QStringLiteral("monitor_stop");
}

/* A schema with every description removed: what a call may carry, not how a
 * tool explains it. */
json callShape(json schema)
{
    if (schema.is_object()) {
        schema.erase("description");
        for (auto &item : schema.items())
            item.value() = callShape(item.value());
    } else if (schema.is_array()) {
        for (auto &item : schema)
            item = callShape(item);
    }
    return schema;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanupTestCase() { QDir(g_env.root).removeRecursively(); }

    void overlayResolvesBaseToolsButNeverWorkspaceBoundOnes()
    {
        QSocToolRegistry base;
        StubTool         list(QStringLiteral("agent_list"));
        StubTool         localRead(QStringLiteral("read_file"));
        StubTool         project(QStringLiteral("project_show"));
        base.registerTool(&list);
        base.registerTool(&localRead);
        base.registerTool(&project);

        QSocToolRegistry overlay;
        StubTool         bash(QStringLiteral("bash"));
        overlay.registerTool(&bash);
        overlay.setFallback(&base);

        QCOMPARE(overlay.getTool(QStringLiteral("agent_list")), &list);
        QCOMPARE(overlay.getTool(QStringLiteral("bash")), &bash);
        QVERIFY(overlay.getTool(QStringLiteral("read_file")) == nullptr);
        QVERIFY(overlay.getTool(QStringLiteral("project_show")) == nullptr);
        QCOMPARE(overlay.toolNames(), (QStringList{"agent_list", "bash"}));
        QCOMPARE(definitionNames(overlay), overlay.toolNames());
        QCOMPARE(overlay.count(), 2);
        QVERIFY(overlay.executeTool(QStringLiteral("read_file"), json::object())
                    .startsWith(QStringLiteral("Error:")));
    }

    void ownToolsShadowTheBase()
    {
        QSocToolRegistry base;
        StubTool         localWeb(QStringLiteral("web_fetch"));
        base.registerTool(&localWeb);
        QSocToolRegistry overlay;
        StubTool         ownWeb(QStringLiteral("web_fetch"));
        overlay.registerTool(&ownWeb);
        overlay.setFallback(&base);
        QCOMPARE(overlay.getTool(QStringLiteral("web_fetch")), &ownWeb);
        QCOMPARE(overlay.count(), 1);
    }

    void revisionFollowsTheBase()
    {
        QSocToolRegistry base;
        QSocToolRegistry overlay;
        overlay.setFallback(&base);
        const quint64 before = overlay.revision();
        StubTool      mcp(QStringLiteral("mcp__stub__echo"));
        base.registerTool(&mcp);
        /* The base registry changed what revision() reads. */
        // cppcheck-suppress knownConditionTrueFalse
        QVERIFY(overlay.revision() != before);
        QCOMPARE(overlay.getTool(QStringLiteral("mcp__stub__echo")), &mcp);
        QVERIFY(definitionNames(overlay).contains(QStringLiteral("mcp__stub__echo")));
        const quint64 registered = overlay.revision();
        base.unregisterTool(&mcp);
        /* The base registry changed what revision() reads. */
        // cppcheck-suppress knownConditionTrueFalse
        QVERIFY(overlay.revision() != registered);
        QVERIFY(!overlay.hasTool(QStringLiteral("mcp__stub__echo")));
    }

    void abortAllLeavesBaseToolsToSiblings()
    {
        QSocToolRegistry base;
        StubTool         shared(QStringLiteral("agent_list"));
        base.registerTool(&shared);
        QSocToolRegistry overlay;
        StubTool         own(QStringLiteral("bash"));
        overlay.registerTool(&own);
        overlay.setFallback(&base);

        bool cancelled = false;
        shared.body    = [&](QSocToolCallContext *context) {
            overlay.abortAll();
            cancelled = context->isCancellationRequested();
        };
        overlay.executeTool(QStringLiteral("agent_list"), json::object());
        QVERIFY(cancelled);
        QCOMPARE(shared.aborts, 0);
        QCOMPARE(own.aborts, 1);

        QObject sibling;
        shared.body = [&](QSocToolCallContext *context) {
            overlay.abortAll();
            cancelled = context->isCancellationRequested();
        };
        base.executeTool(QStringLiteral("agent_list"), json::object(), &sibling);
        QVERIFY(!cancelled);
        QCOMPARE(shared.aborts, 0);
    }

    void abortCallsReachesTheBase()
    {
        QSocToolRegistry base;
        StubTool         shared(QStringLiteral("wait_agent"));
        base.registerTool(&shared);
        QSocToolRegistry overlay;
        overlay.setFallback(&base);

        QObject owner;
        QObject other;
        bool    cancelled = false;
        shared.body       = [&](QSocToolCallContext *context) {
            overlay.abortCalls(&other);
            const bool untouched = !context->isCancellationRequested();
            overlay.abortCalls(&owner);
            cancelled = untouched && context->isCancellationRequested();
        };
        base.executeTool(QStringLiteral("wait_agent"), json::object(), &owner);
        QVERIFY(cancelled);
    }

    void remoteModeResolvesLocalControlTools()
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = g_env.root + QStringLiteral("/project");
        QSocAgentRuntime  runtime(options);
        QSocToolRegistry *local = runtime.localToolRegistry();
        QVERIFY(local != nullptr);
        StubTool mcp(QStringLiteral("mcp__stub__echo"));

        QObject     owner;
        auto *const remote = buildAgentRemoteRegistry(
            &owner, runtime.remoteConnection(), local, runtime.monitorTaskSource());
        local->registerTool(&mcp);

        for (const char *name :
             {"agent",
              "agent_status",
              "send_message",
              "agent_list",
              "agent_inbox",
              "wait_agent",
              "followup_task",
              "interrupt_agent",
              "schedule_create",
              "schedule_list",
              "host_register",
              "memory_read",
              "ask_user",
              "goal_complete",
              "mcp__stub__echo"}) {
            const QString tool = QString::fromLatin1(name);
            QVERIFY2(remote->getTool(tool) != nullptr, name);
            QCOMPARE(remote->getTool(tool), local->getTool(tool));
        }
        for (const char *name : {"project_list", "module_list", "lsp"}) {
            QVERIFY2(local->hasTool(QString::fromLatin1(name)), name);
            QVERIFY2(!remote->hasTool(QString::fromLatin1(name)), name);
        }
        for (const char *name :
             {"todo_list", "todo_add", "todo_update", "todo_delete", "skill_find", "skill_create"}) {
            const QString tool = QString::fromLatin1(name);
            QVERIFY2(remote->getTool(tool) != nullptr, name);
            QVERIFY2(remote->getTool(tool) != local->getTool(tool), name);
        }
    }

    void workspaceBoundToolsNeverFallThrough()
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = g_env.root + QStringLiteral("/project");
        QSocAgentRuntime  runtime(options);
        QSocToolRegistry *local = runtime.localToolRegistry();
        QObject           owner;
        auto *const       remote = buildAgentRemoteRegistry(
            &owner, runtime.remoteConnection(), local, runtime.monitorTaskSource());
        /* Never probed, so the shell is unknown and the exec tools are absent. */
        QVERIFY(!machineOffersExecTools(runtime.remoteConnection()->host()));

        for (const QString &name : local->toolNames()) {
            if (!QSocToolRegistry::isWorkspaceBound(name))
                continue;
            QSocTool *resolved = remote->getTool(name);
            QVERIFY2(resolved != local->getTool(name), qPrintable(name));
            if (execTool(name)) {
                QVERIFY2(resolved == nullptr, qPrintable(name));
                continue;
            }
            QVERIFY2(localOnly(name) || resolved != nullptr, qPrintable(name));
        }
        for (const QString &name : remote->toolNames()) {
            if (remote->getTool(name) != local->getTool(name))
                QVERIFY2(QSocToolRegistry::isWorkspaceBound(name), qPrintable(name));
        }
    }

    /* Counterexample: remote list_files wanted `directory_path` and remote
     * edit_file had no `replace_all`, so a call that worked locally failed
     * after `/ssh`. */
    void sameNamedToolsTakeTheSameArguments()
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = g_env.root + QStringLiteral("/project");
        QSocAgentRuntime  runtime(options);
        QSocToolRegistry *local = runtime.localToolRegistry();
        QObject           owner;
        auto *const       remote = buildAgentRemoteRegistry(
            &owner, runtime.remoteConnection(), local, runtime.monitorTaskSource());

        int compared = 0;
        for (const QString &name : remote->toolNames()) {
            QSocTool *mine = local->getTool(name);
            QSocTool *host = remote->getTool(name);
            if (mine == nullptr || mine == host)
                continue;
            ++compared;
            QVERIFY2(
                callShape(mine->getParametersSchema()) == callShape(host->getParametersSchema()),
                qPrintable(
                    name + QStringLiteral(": local ")
                    + QString::fromStdString(callShape(mine->getParametersSchema()).dump())
                    + QStringLiteral(" remote ")
                    + QString::fromStdString(callShape(host->getParametersSchema()).dump())));
        }
        QVERIFY(compared >= 4);
    }

    void remotePromptSaysMcpToolsRunHere()
    {
        QSocToolRegistry registry;
        StubTool         mcp(QStringLiteral("mcp__stub__echo"));
        registry.registerTool(&mcp);
        QSocAgentConfig config;
        config.remoteMode = true;
        config.remoteName = QStringLiteral("example");
        QSocAgent agent(nullptr, nullptr, &registry, config);
        QVERIFY(agent.buildSystemPromptWithMemory(false).contains(
            QStringLiteral("These MCP tools run on this machine")));
        config.remoteMode = false;
        agent.setConfig(config);
        QVERIFY(!agent.buildSystemPromptWithMemory(false).contains(
            QStringLiteral("These MCP tools run on this machine")));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentruntimeremotetools.moc"
