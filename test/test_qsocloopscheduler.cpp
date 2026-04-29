// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocloopscheduler.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

QString writePastRecurringLoopFile(const QString &projectDir, const QString &prompt)
{
    /* Forge a schema-2 loops.json with createdAt 5 minutes in the past so
     * the very first tick computes a nextRunMs that is in the past too,
     * causing an immediate fire. Returns the assigned id. */
    const QString id = QStringLiteral("abcd1234");
    QDir(projectDir).mkpath(QStringLiteral(".qsoc"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QJsonObject job;
    job["id"]          = id;
    job["prompt"]      = prompt;
    job["cron"]        = QStringLiteral("*/1 * * * *");
    job["recurring"]   = true;
    job["createdAt"]   = QString::number(now - (5 * 60 * 1000));
    job["lastFiredAt"] = QString::number(0);

    QJsonObject root;
    root["schema"] = 2;
    root["tasks"]  = QJsonArray{job};

    QFile out(QDir(projectDir).filePath(QStringLiteral(".qsoc/loops.json")));
    if (!out.open(QIODevice::WriteOnly))
        return QString();
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    return id;
}

bool isShortHexId(const QString &str)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{8}$"));
    return re.match(str).hasMatch();
}

} /* namespace */

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

class TestQSocLoopScheduler : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    /* ---- parseLoopArgs (cron-based) ---- */

    void parseLoopArgs_leadingInterval()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "5m /status", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/5 * * * *"));
        QCOMPARE(prompt, QString("/status"));
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_trailingEverySuffix()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "check the deploy every 20m", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/20 * * * *"));
        QCOMPARE(prompt, QString("check the deploy"));
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_trailingEveryUnitWord()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "run tests every 5 minutes", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/5 * * * *"));
        QCOMPARE(prompt, QString("run tests"));
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_everyNotFollowedByTime()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "check every PR", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/10 * * * *"));
        QCOMPARE(prompt, QString("check every PR"));
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_default()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "ping the server", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/10 * * * *"));
        QCOMPARE(prompt, QString("ping the server"));
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_intervalOnly_emptyPrompt()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs("5m", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QCOMPARE(cron, QString("*/5 * * * *"));
        QVERIFY(prompt.isEmpty());
        QVERIFY(err.isEmpty());
    }

    void parseLoopArgs_uncleanInterval_setsError()
    {
        QString cron, prompt, err;
        QSocLoopScheduler::parseLoopArgs(
            "7m do stuff", QStringLiteral("*/10 * * * *"), cron, prompt, err);
        QVERIFY(cron.isEmpty());
        QVERIFY(!err.isEmpty());
    }

    /* ---- addJob / removeJob / clearJobs (in-memory mode) ---- */

    void addRemoveList_roundTrip()
    {
        QSocLoopScheduler sched;
        const QString     a = sched.addJob("*/5 * * * *", "foo", true, false);
        const QString     b = sched.addJob("*/2 * * * *", "bar", true, false);
        QVERIFY(isShortHexId(a));
        QVERIFY(isShortHexId(b));
        QVERIFY(a != b);
        QCOMPARE(sched.listJobs().size(), 2);
        QVERIFY(sched.removeJob(a));
        QCOMPARE(sched.listJobs().size(), 1);
        QCOMPARE(sched.listJobs().first().id, b);
        QVERIFY(!sched.removeJob("nopeNope"));
        sched.clearJobs();
        QCOMPARE(sched.listJobs().size(), 0);
    }

    void addJob_invalidCron_returnsEmpty()
    {
        QSocLoopScheduler sched;
        QVERIFY(sched.addJob("totally bogus", "x", true, false).isEmpty());
        QVERIFY(sched.addJob("", "x", true, false).isEmpty());
        QVERIFY(sched.addJob("60 * * * *", "x", true, false).isEmpty());
    }

    void addJob_emptyPrompt_returnsEmpty()
    {
        QSocLoopScheduler sched;
        QVERIFY(sched.addJob("*/5 * * * *", "", true, false).isEmpty());
        QVERIFY(sched.addJob("*/5 * * * *", "   ", true, false).isEmpty());
    }

    void addJob_maxJobsCap()
    {
        QSocLoopScheduler sched;
        for (int i = 0; i < QSocLoopScheduler::kMaxJobs; ++i) {
            const QString id = sched.addJob("*/5 * * * *", QString("p%1").arg(i), true, false);
            QVERIFY(!id.isEmpty());
        }
        const QString rejected = sched.addJob("*/5 * * * *", "overflow", true, false);
        QVERIFY(rejected.isEmpty());
        QCOMPARE(sched.listJobs().size(), QSocLoopScheduler::kMaxJobs);
    }

    /* ---- durable persistence ---- */

    void persist_roundTrip_acrossInstances()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString idA, idB;
        {
            QSocLoopScheduler a;
            a.setProjectDir(tmp.path());
            QVERIFY(a.isOwner());
            idA = a.addJob("*/5 * * * *", "ping", true, true);
            idB = a.addJob("0 9 * * *", "morning report", true, true);
            QVERIFY(!idA.isEmpty());
            QVERIFY(!idB.isEmpty());
        }
        QSocLoopScheduler b;
        b.setProjectDir(tmp.path());
        QVERIFY(b.isOwner());
        const auto jobs = b.listJobs();
        QCOMPARE(jobs.size(), 2);
        QCOMPARE(jobs[0].prompt, QString("ping"));
        QCOMPARE(jobs[1].prompt, QString("morning report"));
        QVERIFY(b.removeJob(idA));
        QCOMPARE(b.listJobs().size(), 1);
    }

    void sessionOnlyJob_notPersisted()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            QSocLoopScheduler a;
            a.setProjectDir(tmp.path());
            QVERIFY(!a.addJob("*/5 * * * *", "ephemeral", true, false).isEmpty());
            QCOMPARE(a.listJobs().size(), 1);
        }
        QSocLoopScheduler b;
        b.setProjectDir(tmp.path());
        /* Session-only task disappeared with previous instance. */
        QCOMPARE(b.listJobs().size(), 0);
    }

    void legacySchema1_dropped()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).mkpath(".qsoc");
        QJsonObject job;
        job["id"]          = QString("L1");
        job["prompt"]      = QString("legacy");
        job["intervalMs"]  = QString::number(60000);
        job["createdAt"]   = QString::number(0);
        job["lastFiredAt"] = QString::number(0);
        job["recurring"]   = true;
        job["enabled"]     = true;
        QJsonObject root;
        root["schema"] = 1;
        root["tasks"]  = QJsonArray{job};
        QFile out(QDir(tmp.path()).filePath(".qsoc/loops.json"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.close();

        QSocLoopScheduler sched;
        sched.setProjectDir(tmp.path());
        QCOMPARE(sched.listJobs().size(), 0);
    }

    /* ---- locking ---- */

    void lock_secondInstance_isNotOwner()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler first;
        first.setProjectDir(tmp.path());
        QVERIFY(first.isOwner());

        QSocLoopScheduler second;
        second.setProjectDir(tmp.path());
        QVERIFY(!second.isOwner());
    }

    void projectSwitch_releasesPriorLock()
    {
        QTemporaryDir projA;
        QTemporaryDir projB;
        QVERIFY(projA.isValid());
        QVERIFY(projB.isValid());

        QSocLoopScheduler rooted;
        rooted.setProjectDir(projA.path());
        QVERIFY(rooted.isOwner());

        QSocLoopScheduler rover;
        rover.setProjectDir(projA.path());
        QVERIFY(!rover.isOwner());

        rover.setProjectDir(projB.path());
        QVERIFY(rover.isOwner());
    }

    void mutate_durableRefusedForNonOwner()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler owner;
        owner.setProjectDir(tmp.path());
        const QString a = owner.addJob("*/5 * * * *", "alpha", true, true);
        QVERIFY(!a.isEmpty());

        QSocLoopScheduler nonOwner;
        nonOwner.setProjectDir(tmp.path());
        QVERIFY(!nonOwner.isOwner());

        QCOMPARE(nonOwner.listJobs().size(), 1);
        QVERIFY(nonOwner.addJob("*/2 * * * *", "beta", true, true).isEmpty());
        QVERIFY(!nonOwner.removeJob(a));
        QVERIFY(!nonOwner.clearJobs());
        QCOMPARE(owner.listJobs().size(), 1);
    }

    /* ---- fire path ---- */

    void recurringFire_emitsOnceWithinTick()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString id = writePastRecurringLoopFile(tmp.path(), "ping");
        QVERIFY(!id.isEmpty());

        QSocLoopScheduler sched;
        QSignalSpy        spy(&sched, &QSocLoopScheduler::promptDue);
        sched.setProjectDir(tmp.path());
        QVERIFY(sched.isOwner());

        QVERIFY(spy.wait(2500));
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy.first().at(0).toString(), QString("ping"));
        QCOMPARE(spy.first().at(1).toString(), id);
        /* Recurring task stays in jobs_ with lastFiredAt advanced. */
        const auto jobs = sched.listJobs();
        QCOMPARE(jobs.size(), 1);
        QVERIFY(jobs.first().lastFiredAt > 0);
    }

    void oneShotFire_eraseFromJobsAfterFire()
    {
        /* One-shot pinned to the very-recent past: fires once on the next
         * tick, then must be erased from jobs_ entirely. */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).mkpath(".qsoc");
        const qint64    nowMs = QDateTime::currentMSecsSinceEpoch();
        const QDateTime past  = QDateTime::fromMSecsSinceEpoch(nowMs).addSecs(-180);
        QJsonObject     job;
        job["id"]          = QString("oneoneone");
        job["prompt"]      = QString("once");
        job["cron"]        = QStringLiteral("%1 %2 %3 %4 *")
                                 .arg(past.time().minute())
                                 .arg(past.time().hour())
                                 .arg(past.date().day())
                                 .arg(past.date().month());
        job["recurring"]   = false;
        job["createdAt"]   = QString::number(past.toMSecsSinceEpoch() - 60000);
        job["lastFiredAt"] = QString::number(0);
        QJsonObject root;
        root["schema"] = 2;
        root["tasks"]  = QJsonArray{job};
        QFile out(QDir(tmp.path()).filePath(".qsoc/loops.json"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.close();

        QSocLoopScheduler sched;
        QSignalSpy        spy(&sched, &QSocLoopScheduler::promptDue);
        sched.setProjectDir(tmp.path());

        QVERIFY(spy.wait(2500));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(sched.listJobs().size(), 0);
    }

    /* ---- dispatch helper (CLI vs queueRequest) ---- */

    void scheduledDispatch_helperClassifiesInputs()
    {
        using L = QSocLoopScheduler;
        QVERIFY(L::scheduledInputRequiresCliDispatch("/status"));
        QVERIFY(L::scheduledInputRequiresCliDispatch("  /compact"));
        QVERIFY(L::scheduledInputRequiresCliDispatch("!make test"));
        QVERIFY(L::scheduledInputRequiresCliDispatch("\t!ls"));
        QVERIFY(!L::scheduledInputRequiresCliDispatch("ping the server"));
        QVERIFY(!L::scheduledInputRequiresCliDispatch("check every PR"));
        QVERIFY(!L::scheduledInputRequiresCliDispatch(""));
        QVERIFY(!L::scheduledInputRequiresCliDispatch("  "));
    }

    /* ---- persist failure rollback ---- */

    void persist_failure_rollsBackInMemory()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler sched;
        sched.setProjectDir(tmp.path());
        QVERIFY(sched.isOwner());
        QVERIFY(!sched.addJob("*/5 * * * *", "alpha", true, true).isEmpty());

        const QString qsocDir = QDir(tmp.path()).filePath(".qsoc");
        QFile         dir(qsocDir);
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::ExeOwner));

        const QString rejected = sched.addJob("*/2 * * * *", "beta", true, true);
        const auto    jobs     = sched.listJobs();
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(rejected.isEmpty());
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().prompt, QString("alpha"));
    }
};

QSOC_TEST_MAIN(TestQSocLoopScheduler)
#include "test_qsocloopscheduler.moc"
