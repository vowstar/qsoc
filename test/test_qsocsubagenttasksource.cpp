// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctasksource.h"

#include <QSignalSpy>
#include <QtCore>
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

class Test : public QObject
{
    Q_OBJECT

private:
    /* Build a stand-alone agent QObject (no LLM service / registry). */
    QSocAgent *makeAgent() { return new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig()); }

private slots:
    void initTestCase() { TestApp::instance(); }

    void testRegisterEmitsAndPopulates()
    {
        QSocSubAgentTaskSource src;
        QSignalSpy             spy(&src, &QSocSubAgentTaskSource::tasksChanged);
        const QString          id = src.registerRun(
            QStringLiteral("read README"), QStringLiteral("general-purpose"), makeAgent());
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.count(), 1);
        const auto rows = src.listTasks();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].id, id);
        QCOMPARE(rows[0].kind, QSocTask::Kind::SubAgent);
        QCOMPARE(rows[0].status, QSocTask::Status::Running);
        QCOMPARE(rows[0].label, QStringLiteral("read README"));
        QVERIFY(rows[0].canKill);
        QVERIFY(rows[0].summary.startsWith(QStringLiteral("general-purpose")));
        QCOMPARE(src.runCount(), 1);
        QVERIFY(src.hasActiveRun());
    }

    void testAppendTranscriptAccumulates()
    {
        QSocSubAgentTaskSource src;
        const QString          id = src.registerRun(
            QStringLiteral("explore"), QStringLiteral("general-purpose"), makeAgent());
        src.appendTranscript(id, QStringLiteral("hello "));
        src.appendTranscript(id, QStringLiteral("world"));
        const QString tail = src.tailFor(id, 1024);
        QVERIFY(tail.contains(QStringLiteral("hello world")));
    }

    void testTailTruncates()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("long"), QStringLiteral("general-purpose"), makeAgent());
        const QString big = QString(QLatin1Char('x')).repeated(2048);
        src.appendTranscript(id, big);
        const QString tail = src.tailFor(id, 256);
        QVERIFY(tail.size() <= 256 + 64); /* "[... truncated ...]\n" prefix slack */
        QVERIFY(tail.startsWith(QStringLiteral("[... truncated ...]")));
    }

    void testMarkCompletedShapesTailAndDisablesKill()
    {
        QSocSubAgentTaskSource src;
        QSignalSpy             spy(&src, &QSocSubAgentTaskSource::tasksChanged);
        const QString          id = src.registerRun(
            QStringLiteral("explain"), QStringLiteral("general-purpose"), makeAgent());
        spy.clear();
        src.markCompleted(id, QStringLiteral("the answer is 42"));
        QCOMPARE(spy.count(), 1);
        const auto rows = src.listTasks();
        QCOMPARE(rows[0].status, QSocTask::Status::Completed);
        QVERIFY(!rows[0].canKill);
        QVERIFY(!src.hasActiveRun());
        const QString tail = src.tailFor(id, 1024);
        QVERIFY(tail.contains(QStringLiteral("=== final ===")));
        QVERIFY(tail.contains(QStringLiteral("the answer is 42")));
    }

    void testMarkFailedShapesTail()
    {
        QSocSubAgentTaskSource src;
        const QString          id = src.registerRun(
            QStringLiteral("flaky"), QStringLiteral("general-purpose"), makeAgent());
        src.markFailed(id, QStringLiteral("LLM timeout"));
        const auto rows = src.listTasks();
        QCOMPARE(rows[0].status, QSocTask::Status::Failed);
        QVERIFY(src.tailFor(id, 1024).contains(QStringLiteral("LLM timeout")));
    }

    void testKillRunningMarksFailedAndEmits()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("hung"), QStringLiteral("general-purpose"), makeAgent());
        QSignalSpy spy(&src, &QSocSubAgentTaskSource::tasksChanged);
        QVERIFY(src.killTask(id));
        QCOMPARE(spy.count(), 1);
        const auto rows = src.listTasks();
        QCOMPARE(rows[0].status, QSocTask::Status::Failed);
        QVERIFY(!src.hasActiveRun());
    }

    void testKillAlreadyDoneIsNoop()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("done"), QStringLiteral("general-purpose"), makeAgent());
        src.markCompleted(id, QStringLiteral("ok"));
        QVERIFY(!src.killTask(id));
        QVERIFY(!src.killTask(QStringLiteral("nonexistent")));
    }

    void testCompletedRunStaysWithinTtl()
    {
        QSocSubAgentTaskSource src;
        const QString          first = src.registerRun(
            QStringLiteral("first"), QStringLiteral("general-purpose"), makeAgent());
        src.markCompleted(first, QStringLiteral("ok"));
        const QString second = src.registerRun(
            QStringLiteral("second"), QStringLiteral("general-purpose"), makeAgent());
        QCOMPARE(src.runCount(), 2);
        QVERIFY(first != second);
    }

    void testIdsAreUniqueRolling()
    {
        QSocSubAgentTaskSource src;
        const QString          a
            = src.registerRun(QStringLiteral("x"), QStringLiteral("general-purpose"), makeAgent());
        const QString b
            = src.registerRun(QStringLiteral("y"), QStringLiteral("general-purpose"), makeAgent());
        QVERIFY(a != b);
        QVERIFY(a.startsWith(QLatin1Char('a')));
        QVERIFY(b.startsWith(QLatin1Char('a')));
    }

    void testTailForUnknownIdEmpty()
    {
        QSocSubAgentTaskSource src;
        QCOMPARE(src.tailFor(QStringLiteral("nope"), 1024), QString());
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocsubagenttasksource.moc"
