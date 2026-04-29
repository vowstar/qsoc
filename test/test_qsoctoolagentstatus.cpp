// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/tool/qsoctoolagentstatus.h"

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
    QSocAgent *makeAgent() { return new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig()); }

private slots:
    void initTestCase() { TestApp::instance(); }

    void testNameAndSchema()
    {
        QSocSubAgentTaskSource src;
        QSocToolAgentStatus    tool(this, &src);
        QCOMPARE(tool.getName(), QStringLiteral("agent_status"));
        QVERIFY(!tool.getDescription().isEmpty());
        const json schema = tool.getParametersSchema();
        QVERIFY(schema["properties"].contains("task_id"));
        QVERIFY(schema["properties"].contains("max_bytes"));
        QVERIFY(schema["required"].is_array());
        QVERIFY(
            std::find(schema["required"].begin(), schema["required"].end(), json("task_id"))
            != schema["required"].end());
    }

    void testMissingTaskIdReturnsError()
    {
        QSocSubAgentTaskSource src;
        QSocToolAgentStatus    tool(this, &src);
        const QString          out    = tool.execute(json::object());
        const json             parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
    }

    void testUnknownTaskIdReturnsError()
    {
        QSocSubAgentTaskSource src;
        QSocToolAgentStatus    tool(this, &src);
        const QString out    = tool.execute(json{{"task_id", "nonexistent"}, {"max_bytes", 100}});
        const json    parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("nonexistent") != std::string::npos);
    }

    void testRunningRunReportsRunningStatus()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("loop"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(id, QStringLiteral("[tool] read_file foo\n"));

        QSocToolAgentStatus tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", id.toStdString()}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("ok"));
        QCOMPARE(parsed["run_status"].get<std::string>(), std::string("running"));
        QCOMPARE(parsed["subagent_type"].get<std::string>(), std::string("explore"));
        QCOMPARE(parsed["label"].get<std::string>(), std::string("loop"));
        QVERIFY(parsed["tail"].get<std::string>().find("read_file") != std::string::npos);
        QVERIFY(parsed["elapsed_seconds"].get<int>() >= 0);
    }

    void testCompletedRunReportsCompletedAndFinal()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("done"), QStringLiteral("general-purpose"), makeAgent());
        src.markCompleted(id, QStringLiteral("FINAL TEXT"));
        QSocToolAgentStatus tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", id.toStdString()}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["run_status"].get<std::string>(), std::string("completed"));
        QVERIFY(parsed["tail"].get<std::string>().find("FINAL TEXT") != std::string::npos);
    }

    void testFailedRunReportsFailedAndError()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("oops"), QStringLiteral("verification"), makeAgent());
        src.markFailed(id, QStringLiteral("LLM timeout"));
        QSocToolAgentStatus tool(this, &src);
        const QString       out    = tool.execute(json{{"task_id", id.toStdString()}});
        const json          parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["run_status"].get<std::string>(), std::string("failed"));
        QVERIFY(parsed["tail"].get<std::string>().find("LLM timeout") != std::string::npos);
    }

    void testMaxBytesTruncatesTail()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("big"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(id, QString(QLatin1Char('x')).repeated(2048));
        QSocToolAgentStatus tool(this, &src);
        const QString out    = tool.execute(json{{"task_id", id.toStdString()}, {"max_bytes", 64}});
        const json    parsed = json::parse(out.toStdString());
        const QString tail   = QString::fromStdString(parsed["tail"].get<std::string>());
        QVERIFY(tail.size() <= 64 + 32); /* + truncation marker slack */
        QVERIFY(tail.contains(QStringLiteral("truncated")));
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsoctoolagentstatus.moc"
