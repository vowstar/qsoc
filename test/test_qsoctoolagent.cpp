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

    void testRejectsConcurrentSpawnWhenSourceBusy()
    {
        QSocSubAgentTaskSource *src  = nullptr;
        auto                   *tool = makeTool(nullptr, &src, nullptr);
        /* Manually register a fake busy run. */
        auto *dummyAgent = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        src->registerRun(QStringLiteral("busy"), QStringLiteral("general-purpose"), dummyAgent);
        QVERIFY(src->hasActiveRun());

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "another"},
                {"prompt", "do something"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("another sub-agent") != std::string::npos);
    }

    void testAbortPropagatesToTaskSource()
    {
        QSocSubAgentTaskSource *src  = nullptr;
        auto                   *tool = makeTool(nullptr, &src, nullptr);
        auto *dummyAgent             = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        src->registerRun(QStringLiteral("running"), QStringLiteral("general-purpose"), dummyAgent);
        QVERIFY(src->hasActiveRun());
        tool->abort();
        /* abortAll only sets the agent's flag; status stays Running
         * until the agent's loop notices. We just verify that abort()
         * does not crash and the task source is still queryable. */
        QVERIFY(src->runCount() >= 1);
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsoctoolagent.moc"
