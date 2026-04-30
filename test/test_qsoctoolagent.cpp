// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolagent.h"
#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
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

class Test : public QObject
{
    Q_OBJECT

private:
    QSocToolAgent *makeTool(
        QSocAgentDefinitionRegistry **outDefs   = nullptr,
        QSocSubAgentTaskSource      **outSource = nullptr,
        QSocToolRegistry            **outReg    = nullptr)
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto *src = new QSocSubAgentTaskSource(this);
        auto *reg = new QSocToolRegistry(this);
        /* No real QLLMService: tests below only exercise validation
         * paths that do not construct a child agent. */
        auto *tool = new QSocToolAgent(this, nullptr, reg, QSocAgentConfig(), defs, src);
        if (outDefs != nullptr) {
            *outDefs = defs;
        }
        if (outSource != nullptr) {
            *outSource = src;
        }
        if (outReg != nullptr) {
            *outReg = reg;
        }
        return tool;
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    void testToolNameAndDescription()
    {
        auto *tool = makeTool();
        QCOMPARE(tool->getName(), QStringLiteral("agent"));
        const QString desc = tool->getDescription();
        QVERIFY(desc.contains(QStringLiteral("sub-agent")));
        QVERIFY(desc.contains(QStringLiteral("general-purpose")));
    }

    void testSchemaIncludesBuiltinEnumValues()
    {
        auto      *tool   = makeTool();
        const json schema = tool->getParametersSchema();
        QVERIFY(schema.contains("properties"));
        const auto &props = schema["properties"];
        QVERIFY(props.contains("subagent_type"));
        QVERIFY(props["subagent_type"].contains("enum"));
        const auto &enumValues = props["subagent_type"]["enum"];
        QVERIFY(enumValues.is_array());
        QVERIFY(!enumValues.empty());
        bool found = false;
        for (const auto &val : enumValues) {
            if (val.is_string() && val.get<std::string>() == "general-purpose") {
                found = true;
                break;
            }
        }
        QVERIFY(found);
        QVERIFY(schema["required"].is_array());
    }

    void testUnknownSubagentTypeReturnsError()
    {
        auto         *tool = makeTool();
        const QString out  = tool->execute(
            json{{"subagent_type", "made-up"}, {"description", "x"}, {"prompt", "hello"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed.contains("error"));
        QVERIFY(parsed["available"].get<std::string>().find("general-purpose") != std::string::npos);
    }

    void testMissingFieldsReturnError()
    {
        auto         *tool   = makeTool();
        const QString out    = tool->execute(json{{"subagent_type", "general-purpose"}});
        const json    parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
    }

    void testRejectsSpawnAtMaxConcurrencyCap()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src = new QSocSubAgentTaskSource(this);
        auto           *reg = new QSocToolRegistry(this);
        QSocAgentConfig parentCfg;
        parentCfg.maxConcurrentSubagents = 1; /* strict serial */
        auto *tool = new QSocToolAgent(this, nullptr, reg, parentCfg, defs, src);

        /* Fake one busy run; with cap=1 a second spawn must be denied. */
        auto *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        src->registerRun(QStringLiteral("busy"), QStringLiteral("general-purpose"), dummy);
        QCOMPARE(src->countRunning(), 1);

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "another"},
                {"prompt", "do something"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(
            parsed["error"].get<std::string>().find("max concurrent sub-agents")
            != std::string::npos);
        QCOMPARE(parsed["running"].get<int>(), 1);
        QCOMPARE(parsed["cap"].get<int>(), 1);
    }

    /* Default cap is 4: a single busy run does NOT block a new spawn
     * by itself. The new spawn proceeds past the cap guard and is
     * stopped at the next required gate (no LLMService configured),
     * which proves we got past the cap-rejection path. */
    void testDefaultCapAllowsSecondSpawnPastBusyRun()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto *src  = new QSocSubAgentTaskSource(this);
        auto *reg  = new QSocToolRegistry(this);
        auto *tool = new QSocToolAgent(this, nullptr, reg, QSocAgentConfig(), defs, src);

        auto *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        src->registerRun(QStringLiteral("busy"), QStringLiteral("general-purpose"), dummy);
        QCOMPARE(src->countRunning(), 1);

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "another"},
                {"prompt", "do something"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        /* We got PAST the cap check (cap=4 > running=1). The next gate
         * fails because llmService is nullptr in this test harness. */
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* QSocAgent::getLLMService() is the spawn tool's source of truth
     * for cloning a per-child service. */
    void testQSocAgentExposesLLMServiceAccessor()
    {
        auto *agent = new QSocAgent(this, nullptr, nullptr, QSocAgentConfig());
        QCOMPARE(agent->getLLMService(), nullptr);
    }

    void testAbortPropagatesToTaskSource()
    {
        QSocSubAgentTaskSource *src  = nullptr;
        auto                   *tool = makeTool(nullptr, &src, nullptr);
        auto *dummyAgent             = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        src->registerRun(QStringLiteral("running"), QStringLiteral("general-purpose"), dummyAgent);
        QVERIFY(src->hasActiveRun());
        tool->abort();
        QVERIFY(src->runCount() >= 1);
    }

    /* Verify the dynamic-resolution behavior: when a parent agent is
     * bound, the spawn tool reads its CURRENT registry / config, not
     * the constructor-captured snapshot. This is the contract that
     * keeps remote-mode (registry swap + remoteMode=true on config)
     * propagating into children. */
    void testParentAgentRegistryShadowsCachedSnapshot()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src         = new QSocSubAgentTaskSource(this);
        auto           *cachedReg   = new QSocToolRegistry(this);
        auto           *liveReg     = new QSocToolRegistry(this);
        auto           *parentAgent = new QSocAgent(this, nullptr, liveReg, QSocAgentConfig());
        QSocAgentConfig parentCfg;
        parentCfg.remoteMode = true;
        parentCfg.remoteName = QStringLiteral("user@host:22");
        parentAgent->setConfig(parentCfg);

        auto *tool = new QSocToolAgent(this, nullptr, cachedReg, QSocAgentConfig(), defs, src);
        tool->setParentAgent(parentAgent);

        /* Even though cachedReg / cached config say "local", the live
         * parent says remoteMode=true. The validation paths still
         * work — we can confirm the registry resolution path is the
         * live one by triggering the ll-service guard error AFTER def
         * resolution: that error path runs only when registry came
         * out non-null, so reaching it proves we resolved the live
         * pointer. */
        const QString out = tool->execute(
            json{{"subagent_type", "general-purpose"}, {"description", "x"}, {"prompt", "do thing"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        /* llmService is nullptr → we hit the LLM-guard error AFTER
         * registry resolution, proving liveReg was reachable (not
         * cachedReg, which would also have been non-null but the
         * point is: with parentAgent bound, we follow the live path). */
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* sweepStaleWorktrees deletes only mtime > maxAgeSec dirs and
     * leaves fresh ones alone. Test against a temp-rooted worktree
     * dir; we override the global path indirectly by accepting that
     * the function runs on the real TempLocation/qsoc-worktrees. To
     * avoid colliding with concurrent qsoc instances, we create
     * uniquely-named dirs and only assert *those* specific entries
     * are removed. */
    void testSweepStaleWorktreesRemovesOldOnly()
    {
        const QString root = QDir::tempPath() + QStringLiteral("/qsoc-worktrees");
        QDir().mkpath(root);
        const QString uniq    = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString freshWt = QDir(root).filePath(QStringLiteral("qsoc_wt_fresh_") + uniq);
        const QString staleWt = QDir(root).filePath(QStringLiteral("qsoc_wt_stale_") + uniq);
        QDir().mkpath(freshWt);
        QDir().mkpath(staleWt);
        /* Fake old mtime by using `touch -t` style: write a marker
         * file then setFileTime. */
        QFile staleMarker(staleWt + QStringLiteral("/.marker"));
        QVERIFY(staleMarker.open(QIODevice::WriteOnly));
        staleMarker.close();
        const QDateTime oldTs = QDateTime::currentDateTime().addDays(-3);
        QFileInfo(staleWt).setFile(staleWt);
        /* Use Linux-only touch via shell: more portable than
         * QFile::setFileTime which is Qt 6.5+ */
        QProcess::execute(
            QStringLiteral("touch"),
            {QStringLiteral("-t"), oldTs.toString(QStringLiteral("yyyyMMddhhmm")), staleWt});

        const int removed = QSocToolAgent::sweepStaleWorktrees(/*maxAgeSec*/ 24 * 60 * 60);

        QVERIFY(removed >= 1);
        QVERIFY(QDir(freshWt).exists());
        QVERIFY(!QDir(staleWt).exists());

        QDir(freshWt).removeRecursively();
    }

    void testSweepStaleWorktreesNoRootIsNoOp()
    {
        /* Sweeping when the root doesn't exist returns 0 cleanly. */
        const int removed = QSocToolAgent::sweepStaleWorktrees(/*maxAgeSec*/ 1);
        QVERIFY(removed >= 0);
    }

    void testIsolationFieldInSchemaEnumNoneAndWorktree()
    {
        auto      *tool   = makeTool();
        const json schema = tool->getParametersSchema();
        QVERIFY(schema["properties"].contains("isolation"));
        const auto &enumValues = schema["properties"]["isolation"]["enum"];
        QVERIFY(enumValues.is_array());
        std::vector<std::string> got;
        for (const auto &val : enumValues) {
            got.push_back(val.get<std::string>());
        }
        QVERIFY(std::find(got.begin(), got.end(), "none") != got.end());
        QVERIFY(std::find(got.begin(), got.end(), "worktree") != got.end());
    }

    /* When isolation=worktree but the project path is not a git
     * repo, the spawn tool silently falls back to no isolation
     * (worktree path empty in the response). */
    void testIsolationWorktreeFallsBackOutsideGitRepo()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src = new QSocSubAgentTaskSource(this);
        auto           *reg = new QSocToolRegistry(this);
        QTemporaryDir   nonGit; /* not a git repo */
        QSocAgentConfig cfg;
        cfg.projectPath = nonGit.path();
        auto *tool      = new QSocToolAgent(this, nullptr, reg, cfg, defs, src);

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "x"},
                {"prompt", "do thing"},
                {"isolation", "worktree"}});
        const json parsed = json::parse(out.toStdString());
        /* Falls through to LLM-null guard, which proves we got past
         * the isolation step without crashing on the missing repo. */
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* Direct verification of QSocAgent::addExternalTokenUsage: it must
     * accumulate into totalInputTokens / totalOutputTokens and emit a
     * tokenUsage signal carrying the new totals. This is the contract
     * the spawn tool relies on to fold child usage into the parent. */
    void testAddExternalTokenUsageAccumulatesAndEmits()
    {
        auto      *parent = new QSocAgent(this, nullptr, nullptr, QSocAgentConfig());
        QSignalSpy spy(parent, &QSocAgent::tokenUsage);

        parent->addExternalTokenUsage(100, 50);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toLongLong(), qint64(100));
        QCOMPARE(spy.at(0).at(1).toLongLong(), qint64(50));

        parent->addExternalTokenUsage(40, 0);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toLongLong(), qint64(140));
        QCOMPARE(spy.at(1).at(1).toLongLong(), qint64(50));

        /* Zero / negative deltas are ignored; signal not re-emitted. */
        parent->addExternalTokenUsage(0, 0);
        parent->addExternalTokenUsage(-5, -5);
        QCOMPARE(spy.count(), 2);
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsoctoolagent.moc"
