// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolschedule.h"
#include "cli/qsocloopscheduler.h"
#include "qsoc_test.h"

#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QtTest>

#include <nlohmann/json.hpp>

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

} /* namespace */

class TestQSocToolSchedule : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    /* ---- schedule_create ---- */

    void create_validRecurringEnqueuesJob()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleCreate tool(nullptr, &sched);
        const json             args = {{"prompt", "check the deploy"}, {"cron", "*/5 * * * *"}};
        const QString          out  = tool.execute(args);
        QVERIFY(out.startsWith("Scheduled "));
        QCOMPARE(sched.listJobs().size(), 1);
        QCOMPARE(sched.listJobs().first().cron, QString("*/5 * * * *"));
        QCOMPARE(sched.listJobs().first().recurring, true);
        QCOMPARE(sched.listJobs().first().durable, false);
    }

    void create_oneShotPinnedDate()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleCreate tool(nullptr, &sched);
        const json args = {{"prompt", "remind me"}, {"cron", "30 14 27 2 *"}, {"recurring", false}};
        const QString out = tool.execute(args);
        QVERIFY(out.contains("one-shot"));
        QCOMPARE(sched.listJobs().size(), 1);
        QCOMPARE(sched.listJobs().first().recurring, false);
    }

    void create_durableTrueWritesToDisk()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler sched;
        sched.setProjectDir(tmp.path());
        QSocToolScheduleCreate tool(nullptr, &sched);
        const json    args = {{"prompt", "ping"}, {"cron", "*/10 * * * *"}, {"durable", true}};
        const QString out  = tool.execute(args);
        QVERIFY(out.contains("durable"));
        /* New scheduler bound to same dir picks up the durable task. */
        sched.setProjectDir(QString());
        QSocLoopScheduler other;
        other.setProjectDir(tmp.path());
        QCOMPARE(other.listJobs().size(), 1);
        QCOMPARE(other.listJobs().first().prompt, QString("ping"));
    }

    void create_invalidCronReturnsError()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleCreate tool(nullptr, &sched);
        const json             args = {{"prompt", "x"}, {"cron", "totally bogus"}};
        const QString          out  = tool.execute(args);
        QVERIFY(out.startsWith("Error:"));
        QCOMPARE(sched.listJobs().size(), 0);
    }

    void create_emptyPromptReturnsError()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleCreate tool(nullptr, &sched);
        const json             args = {{"prompt", "   "}, {"cron", "*/5 * * * *"}};
        const QString          out  = tool.execute(args);
        QVERIFY(out.startsWith("Error:"));
        QCOMPARE(sched.listJobs().size(), 0);
    }

    void create_missingFieldsReturnsError()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleCreate tool(nullptr, &sched);
        QVERIFY(tool.execute(json{}).startsWith("Error:"));
        QVERIFY(tool.execute(json{{"prompt", "x"}}).startsWith("Error:"));
        QVERIFY(tool.execute(json{{"cron", "*/5 * * * *"}}).startsWith("Error:"));
    }

    /* ---- schedule_list ---- */

    void list_emptyScheduleReturnsMessage()
    {
        QSocLoopScheduler    sched;
        QSocToolScheduleList tool(nullptr, &sched);
        const QString        out = tool.execute(json{});
        QCOMPARE(out, QString("No scheduled tasks."));
    }

    void list_includesCreatedJobs()
    {
        QSocLoopScheduler    sched;
        const QString        idA = sched.addJob("*/5 * * * *", "alpha", true, false);
        const QString        idB = sched.addJob("0 9 * * *", "beta", true, false);
        QSocToolScheduleList tool(nullptr, &sched);
        const QString        out = tool.execute(json{});
        QVERIFY(out.contains(idA));
        QVERIFY(out.contains(idB));
        QVERIFY(out.contains("alpha"));
        QVERIFY(out.contains("beta"));
    }

    /* ---- schedule_delete ---- */

    void delete_byIdRemovesJob()
    {
        QSocLoopScheduler      sched;
        const QString          id = sched.addJob("*/5 * * * *", "ephemeral", true, false);
        QSocToolScheduleDelete tool(nullptr, &sched);
        const QString          out = tool.execute(json{{"id", id.toStdString()}});
        QVERIFY(out.startsWith("Cancelled"));
        QCOMPARE(sched.listJobs().size(), 0);
    }

    void delete_unknownIdReturnsNotFound()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleDelete tool(nullptr, &sched);
        const QString          out = tool.execute(json{{"id", std::string("deadbeef")}});
        QVERIFY(out.startsWith("No scheduled task"));
    }

    void delete_missingIdReturnsError()
    {
        QSocLoopScheduler      sched;
        QSocToolScheduleDelete tool(nullptr, &sched);
        const QString          out = tool.execute(json{});
        QVERIFY(out.startsWith("Error:"));
    }

    /* ---- nullptr scheduler safety ---- */

    void allTools_handleNullScheduler()
    {
        QSocToolScheduleCreate c;
        QSocToolScheduleList   l;
        QSocToolScheduleDelete d;
        QVERIFY(
            c.execute(json{{"prompt", "x"}, {"cron", "*/5 * * * *"}}).contains("not configured"));
        QVERIFY(l.execute(json{}).contains("not configured"));
        QVERIFY(d.execute(json{{"id", std::string("abc")}}).contains("not configured"));
    }
};

QSOC_TEST_MAIN(TestQSocToolSchedule)

#include "test_qsoctoolschedule.moc"
