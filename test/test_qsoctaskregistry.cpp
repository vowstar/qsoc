// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocbashtasksource.h"
#include "agent/qsoclooptasksource.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctaskregistry.h"
#include "agent/qsoctasksource.h"
#include "cli/qsocloopscheduler.h"
#include "qsoc_test.h"

#include <QObject>
#include <QSignalSpy>
#include <QtTest>

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

/* Minimal in-memory source for registry-aggregation tests so the cases
 * do not depend on scheduler or bash tool wiring. */
class FakeTaskSource : public QSocTaskSource
{
public:
    QString              tag;
    QList<QSocTask::Row> rows;
    QString              tailContent;
    bool                 killReturn = true;
    QString              lastKilledId;

    QString sourceTag() const override { return tag; }

    QList<QSocTask::Row> listTasks() const override { return rows; }

    QString tailFor(const QString &id, int /*maxBytes*/) const override
    {
        for (const auto &row : rows) {
            if (row.id == id)
                return tailContent;
        }
        return QString();
    }

    bool killTask(const QString &id) override
    {
        lastKilledId = id;
        if (killReturn) {
            rows.removeIf([&](const QSocTask::Row &row) { return row.id == id; });
            emit tasksChanged();
        }
        return killReturn;
    }

    void notifyChanged() { emit tasksChanged(); }
};

QSocTask::Row makeRow(
    const QString &id, QSocTask::Status status = QSocTask::Status::Running, qint64 startedAt = 0)
{
    QSocTask::Row row;
    row.id          = id;
    row.label       = QStringLiteral("label-%1").arg(id);
    row.summary     = QStringLiteral("summary-%1").arg(id);
    row.kind        = QSocTask::Kind::Loop;
    row.status      = status;
    row.startedAtMs = startedAt;
    row.canKill     = true;
    return row;
}

} /* namespace */

class TestQSocTaskRegistry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    /* ---- registry merging ---- */

    void singleSourceListsAllRows()
    {
        QSocTaskRegistry registry;
        FakeTaskSource   src;
        src.tag = "fake";
        src.rows.append(makeRow("a", QSocTask::Status::Running, 100));
        src.rows.append(makeRow("b", QSocTask::Status::Pending, 200));
        registry.registerSource(&src);

        const auto rows = registry.listAll();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].sourceTag, QString("fake"));
    }

    void multipleSourcesUnioned()
    {
        QSocTaskRegistry registry;
        FakeTaskSource   one;
        FakeTaskSource   two;
        one.tag = "one";
        two.tag = "two";
        one.rows.append(makeRow("x", QSocTask::Status::Running, 100));
        two.rows.append(makeRow("y", QSocTask::Status::Running, 200));
        registry.registerSource(&one);
        registry.registerSource(&two);
        QCOMPARE(registry.activeCount(), 2);
        const auto rows = registry.listAll();
        QCOMPARE(rows.size(), 2);
    }

    void sortOrderRunningFirstThenStartedAtDesc()
    {
        QSocTaskRegistry registry;
        FakeTaskSource   src;
        src.tag = "fake";
        src.rows.append(makeRow("old-running", QSocTask::Status::Running, 100));
        src.rows.append(makeRow("new-pending", QSocTask::Status::Pending, 200));
        src.rows.append(makeRow("new-running", QSocTask::Status::Running, 300));
        src.rows.append(makeRow("stuck-bash", QSocTask::Status::Stuck, 50));
        registry.registerSource(&src);

        const auto rows = registry.listAll();
        QCOMPARE(rows.size(), 4);
        /* Running > Stuck > Pending; within Running, newer first. */
        QCOMPARE(rows[0].row.id, QString("new-running"));
        QCOMPARE(rows[1].row.id, QString("old-running"));
        QCOMPARE(rows[2].row.id, QString("stuck-bash"));
        QCOMPARE(rows[3].row.id, QString("new-pending"));
    }

    void tailRoutingByTag()
    {
        QSocTaskRegistry registry;
        FakeTaskSource   one;
        FakeTaskSource   two;
        one.tag = "one";
        two.tag = "two";
        one.rows.append(makeRow("a"));
        one.tailContent = "tail-from-one";
        two.rows.append(makeRow("a"));
        two.tailContent = "tail-from-two";
        registry.registerSource(&one);
        registry.registerSource(&two);

        QCOMPARE(registry.tailFor("one", "a", 1024), QString("tail-from-one"));
        QCOMPARE(registry.tailFor("two", "a", 1024), QString("tail-from-two"));
        QVERIFY(registry.tailFor("missing", "a", 1024).isEmpty());
    }

    void killRoutingAndChangeFanout()
    {
        QSocTaskRegistry registry;
        FakeTaskSource   src;
        src.tag = "fake";
        src.rows.append(makeRow("victim"));
        registry.registerSource(&src);
        QSignalSpy fanoutSpy(&registry, &QSocTaskRegistry::anySourceChanged);

        QVERIFY(registry.killTask("fake", "victim"));
        QCOMPARE(src.lastKilledId, QString("victim"));
        QCOMPARE(registry.activeCount(), 0);
        QVERIFY(fanoutSpy.count() >= 1);

        QVERIFY(!registry.killTask("missing", "x"));
    }

    void emptyRegistryHandlesQueriesGracefully()
    {
        QSocTaskRegistry registry;
        QCOMPARE(registry.activeCount(), 0);
        QCOMPARE(registry.listAll().size(), 0);
        QVERIFY(registry.tailFor("x", "y", 1024).isEmpty());
        QVERIFY(!registry.killTask("x", "y"));
    }

    /* ---- concrete adapters ---- */

    void loopSourceMapsScheduler()
    {
        QSocLoopScheduler  sched;
        QSocLoopTaskSource src(&sched);
        QCOMPARE(src.sourceTag(), QString("loop"));

        const QString jobId = sched.addJob("*/5 * * * *", "ping", true, false);
        QVERIFY(!jobId.isEmpty());
        const auto rows = src.listTasks();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].id, jobId);
        QCOMPARE(rows[0].label, QString("ping"));
        QVERIFY(rows[0].canKill);

        const QString tail = src.tailFor(jobId, 1024);
        QVERIFY(tail.contains("Cron:"));
        QVERIFY(tail.contains("*/5 * * * *"));
        QVERIFY(tail.contains("ping"));

        QVERIFY(src.killTask(jobId));
        QCOMPARE(src.listTasks().size(), 0);
    }

    void loopSourceTailReturnsEmptyForMissingId()
    {
        QSocLoopScheduler  sched;
        QSocLoopTaskSource src(&sched);
        QVERIFY(src.tailFor("nope", 1024).isEmpty());
    }

    void loopSourceFiresChanged()
    {
        QSocLoopScheduler  sched;
        QSocLoopTaskSource src(&sched);
        QSignalSpy         spy(&src, &QSocTaskSource::tasksChanged);
        sched.addJob("*/5 * * * *", "ping", true, false);
        QVERIFY(spy.count() >= 1);
    }

    void subAgentStubReturnsEmptyButFitsAggregation()
    {
        /* Validate the third concrete source: present in the abstraction
         * but contributes no rows. Unioning with a real source must not
         * shadow the real source's rows. */
        QSocLoopScheduler      sched;
        QSocLoopTaskSource     loopSrc(&sched);
        QSocSubAgentTaskSource agentSrc;
        QSocTaskRegistry       registry;
        registry.registerSource(&loopSrc);
        registry.registerSource(&agentSrc);
        QCOMPARE(agentSrc.sourceTag(), QString("agent"));
        QVERIFY(agentSrc.listTasks().isEmpty());
        QVERIFY(agentSrc.tailFor("anything", 1024).isEmpty());
        QVERIFY(!agentSrc.killTask("anything"));

        const QString id = sched.addJob("*/5 * * * *", "ping", true, false);
        QVERIFY(!id.isEmpty());
        QCOMPARE(registry.activeCount(), 1);
        const auto rows = registry.listAll();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].sourceTag, QString("loop"));
    }
};

QSOC_TEST_MAIN(TestQSocTaskRegistry)
#include "test_qsoctaskregistry.moc"
