// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocloopscheduler.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

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

private slots:
    void initTestCase() { TestApp::instance(); }

    void parseInterval_basic()
    {
        QCOMPARE(QSocLoopScheduler::parseInterval("5m"), qint64(5 * 60 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("1h"), qint64(60 * 60 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("2d"), qint64(2 * 24 * 60 * 60 * 1000));
    }

    void parseInterval_secondsRoundUpToMinute()
    {
        /* Sub-minute granularity is meaningless for prompt scheduling
         * and the plan calls for ceil-to-minute. Anything from 1s to
         * 60s collapses to 1m; 61s..120s to 2m; etc. */
        QCOMPARE(QSocLoopScheduler::parseInterval("30s"), qint64(60 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("1s"), qint64(60 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("60s"), qint64(60 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("61s"), qint64(120 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("90s"), qint64(120 * 1000));
        QCOMPARE(QSocLoopScheduler::parseInterval("120s"), qint64(120 * 1000));
    }

    void parseInterval_invalid()
    {
        QCOMPARE(QSocLoopScheduler::parseInterval("foo"), qint64(0));
        QCOMPARE(QSocLoopScheduler::parseInterval("0m"), qint64(0));
        QCOMPARE(QSocLoopScheduler::parseInterval("5min"), qint64(0));
        QCOMPARE(QSocLoopScheduler::parseInterval(""), qint64(0));
    }

    void parseLoopArgs_leadingInterval()
    {
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("5m /status", 600000, ms, p);
        QCOMPARE(ms, qint64(5 * 60 * 1000));
        QCOMPARE(p, QString("/status"));
    }

    void parseLoopArgs_trailingEverySuffix()
    {
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("check the deploy every 20m", 600000, ms, p);
        QCOMPARE(ms, qint64(20 * 60 * 1000));
        QCOMPARE(p, QString("check the deploy"));
    }

    void parseLoopArgs_trailingEveryUnitWord()
    {
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("run tests every 5 minutes", 600000, ms, p);
        QCOMPARE(ms, qint64(5 * 60 * 1000));
        QCOMPARE(p, QString("run tests"));
    }

    void parseLoopArgs_everyNotFollowedByTime()
    {
        /* "every PR" must NOT be parsed as an interval: falls through to
         * default. The whole text stays as the prompt verbatim. */
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("check every PR", 600000, ms, p);
        QCOMPARE(ms, qint64(600000));
        QCOMPARE(p, QString("check every PR"));
    }

    void parseLoopArgs_default()
    {
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("ping the server", 600000, ms, p);
        QCOMPARE(ms, qint64(600000));
        QCOMPARE(p, QString("ping the server"));
    }

    void parseLoopArgs_intervalOnly_emptyPrompt()
    {
        qint64  ms = 0;
        QString p;
        QSocLoopScheduler::parseLoopArgs("5m", 600000, ms, p);
        QCOMPARE(ms, qint64(5 * 60 * 1000));
        QVERIFY(p.isEmpty());
    }

    void formatInterval_canonicalUnits()
    {
        QCOMPARE(QSocLoopScheduler::formatInterval(60 * 1000), QString("1m"));
        QCOMPARE(QSocLoopScheduler::formatInterval(10 * 60 * 1000), QString("10m"));
        QCOMPARE(QSocLoopScheduler::formatInterval(qint64(2) * 60 * 60 * 1000), QString("2h"));
        QCOMPARE(QSocLoopScheduler::formatInterval(qint64(3) * 24 * 60 * 60 * 1000), QString("3d"));
    }

    void addRemoveList_roundTrip()
    {
        QSocLoopScheduler sched;
        const QString     a = sched.addJob("foo", 60000);
        const QString     b = sched.addJob("bar", 120000);
        QCOMPARE(sched.listJobs().size(), 2);
        QVERIFY(sched.removeJob(a));
        QCOMPARE(sched.listJobs().size(), 1);
        QCOMPARE(sched.listJobs().first().name, b);
        QVERIFY(!sched.removeJob("nope"));
        sched.clearJobs();
        QCOMPARE(sched.listJobs().size(), 0);
    }

    void persist_roundTrip_acrossInstances()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            QSocLoopScheduler a;
            a.setProjectDir(tmp.path());
            QVERIFY(a.isOwner());
            a.addJob("ping", 60000);
            a.addJob("pong", 120000);
        }
        /* New instance same dir: takes over the lock and re-loads. */
        QSocLoopScheduler b;
        b.setProjectDir(tmp.path());
        QVERIFY(b.isOwner());
        const auto jobs = b.listJobs();
        QCOMPARE(jobs.size(), 2);
        QCOMPARE(jobs[0].prompt, QString("ping"));
        QCOMPARE(jobs[1].prompt, QString("pong"));
        /* Seq counter must skip past loaded ids so a new add cannot
         * collide with persisted L1/L2. */
        const QString fresh = b.addJob("ping3", 60000);
        QVERIFY(fresh != jobs[0].name);
        QVERIFY(fresh != jobs[1].name);
    }

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
        /* Two projects A and B. Session "rover" is non-owner in A (held
         * by "rooted"), then setProjectDir(B). It must release the A
         * QLockFile and re-acquire fresh in B; otherwise a non-owner
         * would carry the old lock object into the new dir, isOwner_
         * would always be false, and B's loops would never fire. */
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

    void nonOwnerList_seesOwnerAdds_immediately()
    {
        /* Two adds within the same mtime tick must both be visible to a
         * non-owner; an mtime-cache gate would have masked the second. */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler owner;
        owner.setProjectDir(tmp.path());
        QVERIFY(owner.isOwner());
        QSocLoopScheduler reader;
        reader.setProjectDir(tmp.path());
        QVERIFY(!reader.isOwner());

        owner.addJob("ping", 60000);
        owner.addJob("pong", 60000);
        QCOMPARE(reader.listJobs().size(), 2);
    }

    void nonOwnerList_seesOwnerRemove_immediately()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler owner;
        owner.setProjectDir(tmp.path());
        const QString name = owner.addJob("ephemeral", 60000);

        QSocLoopScheduler reader;
        reader.setProjectDir(tmp.path());
        QCOMPARE(reader.listJobs().size(), 1);

        QVERIFY(owner.removeJob(name));
        QCOMPARE(reader.listJobs().size(), 0);
    }

    void nonOwnerList_seesOwnerClear_immediately()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler owner;
        owner.setProjectDir(tmp.path());
        owner.addJob("a", 60000);
        owner.addJob("b", 60000);

        QSocLoopScheduler reader;
        reader.setProjectDir(tmp.path());
        QCOMPARE(reader.listJobs().size(), 2);

        QVERIFY(owner.clearJobs());
        QCOMPARE(reader.listJobs().size(), 0);
    }

    void mutate_refusedForNonOwner()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler owner;
        owner.setProjectDir(tmp.path());
        QVERIFY(owner.isOwner());
        const QString a = owner.addJob("alpha", 60000);
        QVERIFY(!a.isEmpty());

        QSocLoopScheduler nonOwner;
        nonOwner.setProjectDir(tmp.path());
        QVERIFY(!nonOwner.isOwner());

        /* Non-owner sees the loaded job but cannot mutate. The on-disk
         * file is left untouched so the owner's view stays authoritative. */
        QCOMPARE(nonOwner.listJobs().size(), 1);
        QVERIFY(nonOwner.addJob("beta", 60000).isEmpty());
        QVERIFY(!nonOwner.removeJob(a));
        QVERIFY(!nonOwner.clearJobs());
        QCOMPARE(owner.listJobs().size(), 1);
    }

    void durableRestart_doesNotBackFireMissedRecurring()
    {
        /* Plan rule: recurring task missed during downtime does not run
         * a catch-up volley at startup. We forge a loops.json where
         * lastFiredAt + interval is well in the past, then construct a
         * scheduler and verify no promptDue fires within ~3 ticks. */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString qsocDir = QDir(tmp.path()).filePath(".qsoc");
        QDir(tmp.path()).mkpath(".qsoc");

        const qint64 now    = QDateTime::currentMSecsSinceEpoch();
        const qint64 oneMin = 60 * 1000;
        QJsonObject  job;
        job["id"]          = QString("L1");
        job["prompt"]      = QString("dont-fire-me");
        job["intervalMs"]  = QString::number(oneMin);
        job["createdAt"]   = QString::number(now - (10 * oneMin));
        job["lastFiredAt"] = QString::number(now - (5 * oneMin));
        job["recurring"]   = true;
        job["enabled"]     = true;
        QJsonObject root;
        root["schema"] = 1;
        root["tasks"]  = QJsonArray{job};
        QFile out(QDir(qsocDir).filePath("loops.json"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.close();

        QSocLoopScheduler sched;
        QSignalSpy        spy(&sched, &QSocLoopScheduler::promptDue);
        sched.setProjectDir(tmp.path());

        const auto jobs = sched.listJobs();
        QCOMPARE(jobs.size(), 1);
        /* nextFireAt must have been advanced past now during load. */
        QVERIFY(jobs.first().nextFireAt > now);
        /* Wait through ~3 ticks; the job is firmly scheduled in the
         * future so no fire should land. */
        QVERIFY(!spy.wait(3500));
        QCOMPARE(spy.count(), 0);
    }

    void promptDue_doesNotEmitBeforeInterval()
    {
        /* Negative test: a brand-new 1m job must not fire within the
         * first second-or-two of ticks. Pairs with the positive
         * promptDue_firesOnceWhenIntervalElapses test below. */
        QSocLoopScheduler sched;
        QSignalSpy        spy(&sched, &QSocLoopScheduler::promptDue);
        const QString     name = sched.addJob("ping", 60000);
        QVERIFY(!name.isEmpty());
        QVERIFY(!spy.wait(1500));
        QCOMPARE(spy.count(), 0);
        QCOMPARE(sched.listJobs().size(), 1);
    }

    void promptDue_firesOnceWhenIntervalElapses()
    {
        /* Positive fire path. Use a sub-second interval (only the user
         * facing parser enforces the 1m floor; the API accepts any
         * positive ms) so the test finishes in seconds. The first tick
         * after createdAt+intervalMs should emit promptDue with the
         * job's prompt and id. */
        QSocLoopScheduler sched;
        QSignalSpy        spy(&sched, &QSocLoopScheduler::promptDue);
        const QString     name = sched.addJob("ping", 100);
        QVERIFY(!name.isEmpty());
        QVERIFY(spy.wait(2500));
        QVERIFY(spy.count() >= 1);
        const auto args = spy.first();
        QCOMPARE(args.at(0).toString(), QString("ping"));
        QCOMPARE(args.at(1).toString(), name);
        /* nextFireAt rolled forward; the same job stays scheduled. */
        QCOMPARE(sched.listJobs().size(), 1);
        QVERIFY(sched.listJobs().first().lastFiredAt > 0);
    }

    void scheduledDispatch_helperClassifiesInputs()
    {
        /* Pin the dispatch policy: slash and shell prompts must take
         * the CLI path; everything else can ride the agent queue. */
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

    void tickPersistFailure_emitsOnceUntilRecovery()
    {
        /* When fire-time persist() fails, the scheduler should keep
         * firing (silent halt is worse than disk drift) but emit
         * persistFailed exactly once until the next successful write. */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler sched;
        sched.setProjectDir(tmp.path());
        QVERIFY(sched.isOwner());
        const QString name = sched.addJob("ping", 100);
        QVERIFY(!name.isEmpty());

        QSignalSpy fireSpy(&sched, &QSocLoopScheduler::promptDue);
        QSignalSpy failSpy(&sched, &QSocLoopScheduler::persistFailed);

        const QString qsocDir = QDir(tmp.path()).filePath(".qsoc");
        QFile         dir(qsocDir);
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::ExeOwner));

        /* Wait through several ticks so multiple fires can stack up. */
        QVERIFY(fireSpy.wait(2500));
        QVERIFY(fireSpy.count() >= 1);
        /* Latched: only one persistFailed regardless of how many ticks
         * fired during the outage. */
        QCOMPARE(failSpy.count(), 1);

        /* Restore permissions and trigger another mutation; the next
         * persist succeeds and clears the latch. A future failure
         * window must be allowed to re-emit. */
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(!sched.addJob("delta", 60000).isEmpty());
    }

    void persist_failure_rollsBackInMemory()
    {
        /* Make the .qsoc directory unwritable mid-flight: the first
         * addJob writes loops.json successfully, then we strip write
         * permissions and the second addJob must be refused without
         * leaving a phantom job in jobs_. */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocLoopScheduler sched;
        sched.setProjectDir(tmp.path());
        QVERIFY(sched.isOwner());
        QVERIFY(!sched.addJob("alpha", 60000).isEmpty());

        const QString qsocDir = QDir(tmp.path()).filePath(".qsoc");
        QFile         dir(qsocDir);
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::ExeOwner));

        const QString rejected = sched.addJob("beta", 60000);
        const auto    jobs     = sched.listJobs();
        /* Restore so QTemporaryDir can clean up. */
        QVERIFY(dir.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(rejected.isEmpty());
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().prompt, QString("alpha"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocloopscheduler.moc"
