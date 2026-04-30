// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class FakeTool : public QSocTool
{
    Q_OBJECT
public:
    FakeTool(QString name, QObject *parent = nullptr)
        : QSocTool(parent)
        , name_(std::move(name))
    {}
    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("fake"); }
    json    getParametersSchema() const override
    {
        return json{{"type", "object"}, {"properties", json::object()}};
    }
    QString execute(const json &) override { return QStringLiteral("ok"); }

private:
    QString name_;
};

class Test : public QObject
{
    Q_OBJECT

private:
    QSocToolRegistry *makeRegistry()
    {
        auto *reg = new QSocToolRegistry(this);
        reg->registerTool(new FakeTool(QStringLiteral("file_read"), reg));
        reg->registerTool(new FakeTool(QStringLiteral("bash_run"), reg));
        reg->registerTool(new FakeTool(QStringLiteral("agent"), reg));
        return reg;
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    /* Default config: every tool allowed; "agent" allowed for parent. */
    void testAllowlistEmptyMeansAll()
    {
        QSocAgentConfig cfg;
        auto           *reg   = makeRegistry();
        auto           *agent = new QSocAgent(this, nullptr, reg, cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(agent->isToolAllowed(QStringLiteral("agent")));
        QCOMPARE(agent->getEffectiveToolDefinitions().size(), size_t{3});
    }

    /* Sub-agent without explicit allowlist still inherits everything
     * EXCEPT the spawn-agent tool (recursion guard). */
    void testSubAgentBlocksAgentToolEvenWithEmptyAllowlist()
    {
        QSocAgentConfig cfg;
        cfg.isSubAgent = true;
        auto *reg      = makeRegistry();
        auto *agent    = new QSocAgent(this, nullptr, reg, cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(agent->isToolAllowed(QStringLiteral("bash_run")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("agent")));
        json defs = agent->getEffectiveToolDefinitions();
        QCOMPARE(defs.size(), size_t{2});
        for (const auto &def : defs) {
            QString name = QString::fromStdString(def["function"]["name"].get<std::string>());
            QVERIFY(name != QStringLiteral("agent"));
        }
    }

    /* Sub-agent with explicit allowlist: only listed tools, never "agent". */
    void testSubAgentAllowlistFilters()
    {
        QSocAgentConfig cfg;
        cfg.isSubAgent = true;
        cfg.toolsAllow = {QStringLiteral("file_read"), QStringLiteral("agent")};
        auto *reg      = makeRegistry();
        auto *agent    = new QSocAgent(this, nullptr, reg, cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("bash_run")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("agent")));
        QCOMPARE(agent->getEffectiveToolDefinitions().size(), size_t{1});
    }

    /* Denylist subtracts from inherit-everything: empty allow + deny
     * one tool means everything else stays allowed. */
    void testDenylistSubtractsFromInheritEverything()
    {
        QSocAgentConfig cfg;
        cfg.toolsAllow.clear();
        cfg.toolsDeny = {QStringLiteral("bash_run")};
        auto *agent   = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("bash_run")));
        QVERIFY(agent->isToolAllowed(QStringLiteral("agent")));
    }

    /* Denylist takes precedence over allowlist when they intersect:
     * "allow X and Y, but deny Y" leaves only X. */
    void testDenylistWinsAgainstAllowlist()
    {
        QSocAgentConfig cfg;
        cfg.toolsAllow = {QStringLiteral("file_read"), QStringLiteral("bash_run")};
        cfg.toolsDeny  = {QStringLiteral("bash_run")};
        auto *agent    = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("bash_run")));
    }

    /* Denylist works in sub-agent mode too; recursion guard still
     * blocks the agent tool independently. */
    void testDenylistInSubAgentRespectsRecursionGuard()
    {
        QSocAgentConfig cfg;
        cfg.isSubAgent = true;
        cfg.toolsDeny  = {QStringLiteral("file_read")};
        auto *agent    = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QVERIFY(!agent->isToolAllowed(QStringLiteral("file_read")));
        QVERIFY(agent->isToolAllowed(QStringLiteral("bash_run")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("agent")));
    }

    /* Non-sub parent with allowlist behaves as plain whitelist (no
     * automatic "agent" rejection). */
    void testParentAllowlistAllowsAgentTool()
    {
        QSocAgentConfig cfg;
        cfg.toolsAllow = {QStringLiteral("agent")};
        auto *reg      = makeRegistry();
        auto *agent    = new QSocAgent(this, nullptr, reg, cfg);
        QVERIFY(agent->isToolAllowed(QStringLiteral("agent")));
        QVERIFY(!agent->isToolAllowed(QStringLiteral("file_read")));
        QCOMPARE(agent->getEffectiveToolDefinitions().size(), size_t{1});
    }

    /* Legacy override path: non-sub-agent + override returns it verbatim. */
    void testLegacyOverrideReturnsVerbatim()
    {
        QSocAgentConfig cfg;
        cfg.systemPromptOverride = QStringLiteral("LEGACY ONLY");
        auto *agent              = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QCOMPARE(agent->buildSystemPromptWithMemory(), QStringLiteral("LEGACY ONLY"));
    }

    /* Sub-agent override path: identity is replaced but Environment
     * section is still appended; default skill/memory injection is
     * gated by autoLoadMemory and skillListing flags so neither is
     * emitted under the default sub-agent config. */
    void testSubAgentOverrideAppendsDynamicSections()
    {
        QSocAgentConfig cfg;
        cfg.isSubAgent           = true;
        cfg.systemPromptOverride = QStringLiteral("YOU ARE A SUB-AGENT.");
        cfg.autoLoadMemory       = false;
        cfg.skillListing.clear();
        cfg.modelId    = QStringLiteral("test-model");
        auto   *agent  = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QString prompt = agent->buildSystemPromptWithMemory();
        QVERIFY(prompt.startsWith(QStringLiteral("YOU ARE A SUB-AGENT.")));
        QVERIFY(prompt.contains(QStringLiteral("# Environment")));
        QVERIFY(prompt.contains(QStringLiteral("test-model")));
        QVERIFY(!prompt.contains(QStringLiteral("You are QSoC Agent")));
        QVERIFY(!prompt.contains(QStringLiteral("# Memory")));
        QVERIFY(!prompt.contains(QStringLiteral("# Available skills")));
    }

    /* Sub-agent with skills and memory injection enabled still gets
     * those sections (consistent with parent config gates). */
    void testSubAgentOptInForSkillsAndRemoteSection()
    {
        QSocAgentConfig cfg;
        cfg.isSubAgent           = true;
        cfg.systemPromptOverride = QStringLiteral("HEAD");
        cfg.skillListing         = QStringLiteral("- skill_x: do X\n");
        cfg.remoteMode           = true;
        cfg.remoteDisplay        = QStringLiteral("user@host:22");
        cfg.remoteWorkspace      = QStringLiteral("/remote/ws");
        auto   *agent            = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QString prompt           = agent->buildSystemPromptWithMemory();
        QVERIFY(prompt.contains(QStringLiteral("# Available skills")));
        QVERIFY(prompt.contains(QStringLiteral("# Remote Workspace")));
        QVERIFY(prompt.contains(QStringLiteral("user@host:22")));
        QVERIFY(prompt.contains(QStringLiteral("/remote/ws")));
    }

    /* Legacy non-sub default path still emits the full identity prefix. */
    void testRegularAgentEmitsIdentity()
    {
        QSocAgentConfig cfg;
        auto           *agent  = new QSocAgent(this, nullptr, makeRegistry(), cfg);
        QString         prompt = agent->buildSystemPromptWithMemory();
        QVERIFY(prompt.contains(QStringLiteral("You are QSoC Agent")));
        QVERIFY(prompt.contains(QStringLiteral("# Environment")));
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocagentsubagentguards.moc"
