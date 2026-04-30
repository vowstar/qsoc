// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/tool/qsoctoolsendmessage.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
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
        QSocToolSendMessage    tool(this, &src);
        QCOMPARE(tool.getName(), QStringLiteral("send_message"));
        QVERIFY(!tool.getDescription().isEmpty());
        const json schema = tool.getParametersSchema();
        QVERIFY(schema["properties"].contains("task_id"));
        QVERIFY(schema["properties"].contains("message"));
        QVERIFY(schema["required"].is_array());
    }

    void testMissingFieldsReturnError()
    {
        QSocSubAgentTaskSource src;
        QSocToolSendMessage    tool(this, &src);
        const QString          a = tool.execute(json{{"task_id", "x"}});
        QVERIFY(json::parse(a.toStdString())["status"].get<std::string>() == "error");
        const QString b = tool.execute(json{{"message", "hi"}});
        QVERIFY(json::parse(b.toStdString())["status"].get<std::string>() == "error");
        const QString c = tool.execute(
            json{{"task_id", "x"}, {"message", QStringLiteral("").toStdString()}});
        QVERIFY(json::parse(c.toStdString())["status"].get<std::string>() == "error");
    }

    void testUnknownTaskIdRejected()
    {
        QSocSubAgentTaskSource src;
        QSocToolSendMessage    tool(this, &src);
        const QString          out = tool.execute(json{{"task_id", "missing"}, {"message", "hi"}});
        const json             parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("unknown id") != std::string::npos);
    }

    void testQueuesMessageOnRunningChild()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("live"), QStringLiteral("explore"), makeAgent());

        QSocToolSendMessage tool(this, &src);
        const QString       out = tool.execute(
            json{{"task_id", id.toStdString()}, {"message", "follow-up instruction"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("ok"));
        QVERIFY(parsed["queued_bytes"].get<int>() > 0);
    }

    void testRejectsCompletedChild()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("done"), QStringLiteral("general-purpose"), makeAgent());
        src.markCompleted(id, QStringLiteral("ok"));

        QSocToolSendMessage tool(this, &src);
        const QString       out = tool.execute(
            json{{"task_id", id.toStdString()}, {"message", "too late"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolsendmessage.moc"
