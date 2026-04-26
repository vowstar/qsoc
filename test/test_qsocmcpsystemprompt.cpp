// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QtCore>
#include <QtTest>

namespace {

class StubMcpTool : public QSocTool
{
public:
    StubMcpTool(QString name, QObject *parent = nullptr)
        : QSocTool(parent)
        , name_(std::move(name))
    {}

    QString        getName() const override { return name_; }
    QString        getDescription() const override { return QStringLiteral("stub"); }
    nlohmann::json getParametersSchema() const override { return {{"type", "object"}}; }
    QString        execute(const nlohmann::json &) override { return {}; }

private:
    QString name_;
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc_test";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app(argc, argv.data());
    }

    void systemPromptListsMcpServers()
    {
        QSocToolRegistry registry;
        registry.registerTool(new StubMcpTool("mcp__alpha__echo"));
        registry.registerTool(new StubMcpTool("mcp__alpha__ping"));
        registry.registerTool(new StubMcpTool("mcp__beta__search"));
        registry.registerTool(new StubMcpTool("read_file"));

        QSocAgent     agent(nullptr, nullptr, &registry, QSocAgentConfig{});
        const QString prompt = agent.buildSystemPromptWithMemory();

        QVERIFY(prompt.contains(QStringLiteral("# External MCP servers")));
        QVERIFY(prompt.contains(QStringLiteral("alpha (2 tools)")));
        QVERIFY(prompt.contains(QStringLiteral("beta (1 tools)")));
        /* Built-in tools must not be listed under the MCP section. */
        const int sectionStart = prompt.indexOf(QStringLiteral("# External MCP servers"));
        QVERIFY(sectionStart >= 0);
        const int     next    = prompt.indexOf(QStringLiteral("\n# "), sectionStart + 1);
        const int     end     = (next >= 0) ? next : prompt.size();
        const QString section = prompt.mid(sectionStart, end - sectionStart);
        QVERIFY(!section.contains(QStringLiteral("read_file")));
    }

    void systemPromptOmitsSectionWhenNoMcpTools()
    {
        QSocToolRegistry registry;
        registry.registerTool(new StubMcpTool("read_file"));

        QSocAgent     agent(nullptr, nullptr, &registry, QSocAgentConfig{});
        const QString prompt = agent.buildSystemPromptWithMemory();
        QVERIFY(!prompt.contains(QStringLiteral("# External MCP servers")));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpsystemprompt.moc"
