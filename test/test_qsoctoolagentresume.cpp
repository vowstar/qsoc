// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/tool/qsoctoolagentresume.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    QSocAgent *makeAgent() { return new QSocAgent(this, nullptr, nullptr, QSocAgentConfig()); }

private slots:
    void initTestCase() {}

    void testNameAndSchema()
    {
        QSocSubAgentTaskSource src;
        QSocToolAgentResume    tool(this, &src);
        QCOMPARE(tool.getName(), QStringLiteral("agent_resume"));
        QVERIFY(!tool.getDescription().isEmpty());
        const json schema = tool.getParametersSchema();
        QVERIFY(schema["properties"].contains("task_id"));
        QVERIFY(schema["properties"].contains("new_instructions"));
        QVERIFY(schema["properties"].contains("max_tail_bytes"));
    }

    void testUnknownTaskIdReturnsError()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        QSocToolAgentResume tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", "nope"}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("no metadata") != std::string::npos);
    }

    void testResumePayloadCarriesPriorContext()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());

        const QString runId
            = src.registerRun(QStringLiteral("rtl-research"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(runId, QStringLiteral("found module clk_gen at clk.v:42\n"));
        src.markCompleted(runId, QStringLiteral("PRIOR FINAL"));

        QSocToolAgentResume tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", runId.toStdString()}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("ok"));
        QCOMPARE(parsed["original_subagent_type"].get<std::string>(), std::string("explore"));
        QCOMPARE(parsed["original_label"].get<std::string>(), std::string("rtl-research"));
        QCOMPARE(parsed["original_status"].get<std::string>(), std::string("completed"));
        const std::string resume = parsed["resume_prompt"].get<std::string>();
        QVERIFY(resume.find("RESUMING") != std::string::npos);
        QVERIFY(resume.find("rtl-research") != std::string::npos);
        QVERIFY(resume.find("clk_gen") != std::string::npos);
        QVERIFY(resume.find("PRIOR FINAL") != std::string::npos);
    }

    void testNewInstructionsAppendedToResumePrompt()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("dummy"), QStringLiteral("verification"), makeAgent());
        src.markCompleted(runId, QStringLiteral("PASS"));

        QSocToolAgentResume tool(this, &src);
        const QString       out = tool.execute(
            json{
                {"task_id", runId.toStdString()},
                {"new_instructions", "Continue with the second test suite."}});
        const json    parsed       = json::parse(out.toStdString());
        const QString resumePrompt = QString::fromStdString(
            parsed["resume_prompt"].get<std::string>());
        QVERIFY(resumePrompt.contains(QStringLiteral("New instructions:")));
        QVERIFY(resumePrompt.contains(QStringLiteral("second test suite")));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagentresume.moc"
