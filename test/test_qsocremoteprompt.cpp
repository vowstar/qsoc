// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QRegularExpression>
#include <QtTest>

#include <memory>

using json = nlohmann::json;

/*
 * What the remote-mode system prompt claims about the workspace. No host is
 * dialled: the remote registry is an overlay of stub tools over a stub local
 * registry, the shape a bound workspace has.
 */

namespace {

class StubTool : public QSocTool
{
public:
    explicit StubTool(QString name, QObject *parent = nullptr)
        : QSocTool(parent)
        , name_(std::move(name))
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("stub ") + name_; }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return QStringLiteral("ok"); }
    void    abort() override {}

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

/* The local registry a remote overlay falls back to. */
struct Workspace
{
    QSocToolRegistry                       local;
    QSocToolRegistry                       remote;
    std::vector<std::unique_ptr<StubTool>> tools;

    Workspace(const QStringList &localNames, const QStringList &remoteNames)
    {
        for (const QString &name : localNames) {
            tools.push_back(std::make_unique<StubTool>(name));
            local.registerTool(tools.back().get());
        }
        for (const QString &name : remoteNames) {
            tools.push_back(std::make_unique<StubTool>(name));
            remote.registerTool(tools.back().get());
        }
        remote.setFallback(&local);
    }
};

const QStringList kLocal = {
    QStringLiteral("read_file"),
    QStringLiteral("bash"),
    QStringLiteral("path_context"),
    QStringLiteral("todo_add"),
    QStringLiteral("todo_list"),
    QStringLiteral("skill_find"),
    QStringLiteral("skill_create"),
    QStringLiteral("project_list"),
    QStringLiteral("module_add"),
    QStringLiteral("memory_read"),
    QStringLiteral("web_fetch"),
    QStringLiteral("mcp__srv__lookup"),
};

/* A bound workspace with no todo, skill or path tool of its own. */
const QStringList kRemote = {
    QStringLiteral("read_file"),
    QStringLiteral("list_files"),
    QStringLiteral("write_file"),
    QStringLiteral("edit_file"),
    QStringLiteral("bash"),
    QStringLiteral("bash_manage"),
};

QString remoteSection(const QString &prompt)
{
    const qsizetype start = prompt.indexOf(QStringLiteral("# Remote Workspace"));
    const qsizetype end   = prompt.indexOf(QStringLiteral("# SSH Secret Handling"));
    return start < 0 || end < start ? QString() : prompt.mid(start, end - start);
}

QString promptFor(QSocToolRegistry *registry)
{
    QSocAgentConfig cfg;
    cfg.remoteMode      = true;
    cfg.remoteName      = QStringLiteral("box");
    cfg.remoteWorkspace = QStringLiteral("/srv/ws");
    cfg.autoLoadMemory  = false;
    QSocAgent agent(nullptr, nullptr, registry, cfg);
    return agent.buildSystemPromptWithMemory();
}

/* The line of @p section that starts with @p lead, without the lead. */
QStringList listedAfter(const QString &section, const QString &lead)
{
    for (const QString &line : section.split(QLatin1Char('\n'))) {
        if (line.startsWith(lead)) {
            QString body = line.mid(lead.size());
            body.chop(1);
            return body.split(QStringLiteral(", "));
        }
    }
    return {};
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Counterexample: the remote section claimed path_context, todo tools,
     * project skills and skill creation in a workspace that had none. */
    void theRemoteSectionNamesOnlyToolsThatExist()
    {
        Workspace     ws(kLocal, kRemote);
        const QString section = remoteSection(promptFor(&ws.remote));
        QVERIFY2(!section.isEmpty(), "no remote section");

        /* A name the section offers is registered; a name it lists as not
         * available is not. */
        static const QRegularExpression toolName(
            QStringLiteral("\\b[a-z][a-z0-9]*(?:_[a-z0-9]+)+\\b"));
        const QString absentLead = QStringLiteral("Not available in this workspace: ");
        for (const QString &line : section.split(QLatin1Char('\n'))) {
            const bool absent  = line.startsWith(absentLead);
            auto       matches = toolName.globalMatch(line);
            while (matches.hasNext()) {
                const QString name = matches.next().captured(0);
                QVERIFY2(ws.remote.hasTool(name) != absent, qPrintable(line));
            }
        }
        static const QRegularExpression family(QStringLiteral("\\b([a-z]+_)\\*"));
        auto                            families = family.globalMatch(section);
        while (families.hasNext()) {
            const QString prefix = families.next().captured(1);
            for (const QString &name : ws.remote.toolNames())
                QVERIFY2(!name.startsWith(prefix), qPrintable(prefix + QStringLiteral("* exists")));
        }
        QVERIFY2(!section.contains(QStringLiteral("todos.md")), qPrintable(section));
        QVERIFY2(!section.contains(QStringLiteral("skill creation")), qPrintable(section));
    }

    void theRemoteSectionSaysWhereWorkspaceStoresLive()
    {
        Workspace     bare(kLocal, kRemote);
        const QString without = remoteSection(promptFor(&bare.remote));
        QVERIFY2(!without.contains(QStringLiteral("[remote]")), qPrintable(without));
        QVERIFY2(without.contains(QStringLiteral("Project memory")), qPrintable(without));

        Workspace
                      ws(kLocal,
                         kRemote
                             + QStringList{
                                 QStringLiteral("todo_list"),
                                 QStringLiteral("todo_add"),
                                 QStringLiteral("skill_find")});
        const QString section = remoteSection(promptFor(&ws.remote));
        QVERIFY2(section.contains(QStringLiteral(".qsoc/todos.md")), qPrintable(section));
        QVERIFY2(
            section.contains(QStringLiteral("A remote skill wins over a local one")),
            qPrintable(section));
        QVERIFY2(
            section.contains(QStringLiteral("separate for each remote workspace")),
            qPrintable(section));
    }

    void theRemoteSectionSaysWhereEachToolRuns()
    {
        Workspace     ws(kLocal, kRemote);
        const QString section = remoteSection(promptFor(&ws.remote));
        const auto    onHost
            = listedAfter(section, QStringLiteral("Tools that act on the remote host: "));
        const auto local = listedAfter(section, QStringLiteral("Tools that run on this machine: "));
        const auto absent
            = listedAfter(section, QStringLiteral("Not available in this workspace: "));

        QCOMPARE(
            onHost,
            (QStringList{
                "bash", "bash_manage", "edit_file", "list_files", "read_file", "write_file"}));
        QVERIFY2(local.contains(QStringLiteral("web_fetch")), qPrintable(section));
        QVERIFY2(local.contains(QStringLiteral("memory_read")), qPrintable(section));
        QVERIFY2(!local.contains(QStringLiteral("read_file")), qPrintable(section));
        QVERIFY2(absent.contains(QStringLiteral("todo_*")), qPrintable(section));
        QVERIFY2(absent.contains(QStringLiteral("skill_*")), qPrintable(section));
        QVERIFY2(absent.contains(QStringLiteral("path_context")), qPrintable(section));
        QVERIFY2(absent.contains(QStringLiteral("module_*")), qPrintable(section));
    }

    /* Counterexample: a host without a shell still read as one bash ran on. */
    void aHostWithoutAShellSaysSo()
    {
        Workspace     ws(kLocal, {QStringLiteral("read_file")});
        const QString section = remoteSection(promptFor(&ws.remote));
        QVERIFY2(section.contains(QStringLiteral("no usable shell")), qPrintable(section));
        QVERIFY2(
            listedAfter(section, QStringLiteral("Not available in this workspace: "))
                .contains(QStringLiteral("bash")),
            qPrintable(section));
    }

    /* Local MCP tools stay callable from a remote workspace and are listed as
     * running on this machine. */
    void localMcpToolsRunOnThisMachine()
    {
        Workspace     ws(kLocal, kRemote);
        const QString mcp = QStringLiteral("mcp__srv__lookup");
        QCOMPARE(ws.remote.getTool(mcp), ws.local.getTool(mcp));
        QVERIFY(definitionNames(ws.remote).contains(mcp));
        const QString section = remoteSection(promptFor(&ws.remote));
        QVERIFY2(
            listedAfter(section, QStringLiteral("Tools that run on this machine: "))
                .contains(QStringLiteral("the tools under External MCP servers")),
            qPrintable(section));
        QVERIFY2(!section.contains(mcp), qPrintable(section));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteprompt.moc"
