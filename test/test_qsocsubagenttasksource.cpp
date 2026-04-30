// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctasksource.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
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
        src.setTranscriptDir(QString()); /* skip disk fallback */
        QCOMPARE(src.tailFor(QStringLiteral("nope"), 1024), QString());
    }

    /* === N: disk transcript persistence ============================== */

    void testTranscriptIsMirroredToDisk()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("disk-run"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(runId, QStringLiteral("hello "));
        src.appendTranscript(runId, QStringLiteral("world\n"));
        src.markCompleted(runId, QStringLiteral("FINAL"));

        QFile file(src.transcriptPathFor(runId));
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(file.readAll());
        /* Now JSONL format: each line is a JSON object {ts, kind, data}. */
        QVERIFY(content.contains(QStringLiteral("\"kind\":\"chunk\"")));
        QVERIFY(content.contains(QStringLiteral("\"kind\":\"final\"")));
        QVERIFY(content.contains(QStringLiteral("\"data\":\"FINAL\"")));
    }

    void testMetaSidecarRecordsRunStatus()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId = src.registerRun(
            QStringLiteral("with-meta"), QStringLiteral("verification"), makeAgent());
        src.setIsolationMetadata(runId, QStringLiteral("worktree"), QStringLiteral("/tmp/wt/abc"));
        src.markCompleted(runId, QStringLiteral("PASS"));

        QFile meta(src.metaPathFor(runId));
        QVERIFY(meta.exists());
        QVERIFY(meta.open(QIODevice::ReadOnly));
        const QByteArray bytes = meta.readAll();
        const QString    text  = QString::fromUtf8(bytes);
        QVERIFY(text.contains(QStringLiteral("\"task_id\"")));
        QVERIFY(text.contains(QStringLiteral("\"status\": \"completed\"")));
        QVERIFY(text.contains(QStringLiteral("\"isolation\": \"worktree\"")));
        QVERIFY(text.contains(QStringLiteral("/tmp/wt/abc")));
        QVERIFY(text.contains(QStringLiteral("\"subagent_type\": \"verification\"")));
    }

    void testTailForReadsJsonlAfterEviction()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("done"), QStringLiteral("explore"), makeAgent());
        src.appendTranscript(runId, QStringLiteral("first chunk "));
        src.appendTranscript(runId, QStringLiteral("second chunk"));
        src.markCompleted(runId, QStringLiteral("THE FINAL ANSWER"));

        /* Manually clear in-memory state to simulate eviction:
         * agent_status's "evicted run" path falls back to disk. */
        QSocSubAgentTaskSource src2; /* fresh source pointed at same dir */
        src2.setTranscriptDir(tmp.path());
        const QString tail = src2.tailFor(runId, /*maxBytes*/ 0);
        QVERIFY(tail.contains(QStringLiteral("first chunk second chunk")));
        QVERIFY(tail.contains(QStringLiteral("=== final ===")));
        QVERIFY(tail.contains(QStringLiteral("THE FINAL ANSWER")));
    }

    void testFailedRunWritesFailedMarkerToDisk()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("flaky"), QStringLiteral("verification"), makeAgent());
        src.markFailed(runId, QStringLiteral("LLM timeout"));
        QFile file(src.transcriptPathFor(runId));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("\"kind\":\"error\"")));
        QVERIFY(content.contains(QStringLiteral("LLM timeout")));
    }

    /* tailFor falls back to the disk file when the in-memory entry
     * has been evicted. We simulate "evicted" by using an unknown
     * id whose disk file we create manually. */
    void testTailForFallsBackToDisk()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());

        const QString fakeId = QStringLiteral("a99");
        const QString path   = src.transcriptPathFor(fakeId);
        QFile         outFile(path);
        QVERIFY(outFile.open(QIODevice::WriteOnly));
        /* Write a JSONL chunk event so tailFor's fallback parses it. */
        outFile.write("{\"ts\":1,\"kind\":\"chunk\",\"data\":\"evicted-run-tail-content\"}\n");
        outFile.close();

        const QString tail = src.tailFor(fakeId, 1024);
        QVERIFY(tail.contains(QStringLiteral("evicted-run-tail-content")));
    }

    void testTailForDiskTruncationMarker()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString fakeId = QStringLiteral("a98");
        const QString path   = src.transcriptPathFor(fakeId);
        QFile         outFile(path);
        QVERIFY(outFile.open(QIODevice::WriteOnly));
        /* Many JSONL chunk events combine into a long rendered tail. */
        for (int n = 0; n < 200; ++n) {
            outFile.write("{\"ts\":1,\"kind\":\"chunk\",\"data\":\"yyyyyyyyyyyy\"}\n");
        }
        outFile.close();
        const QString tail = src.tailFor(fakeId, 64);
        QVERIFY(tail.startsWith(QStringLiteral("[... truncated ...]")));
    }

    /* Disk file survives evictStaleCompleted: even after the
     * in-memory RunState is gone, the file is still present. */
    void testDiskFileSurvivesEviction()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const QString runId
            = src.registerRun(QStringLiteral("once"), QStringLiteral("explore"), makeAgent());
        src.markCompleted(runId, QStringLiteral("done"));
        QVERIFY(QFile::exists(src.transcriptPathFor(runId)));
        /* Evict by deleting the in-memory entry directly via kill +
         * a long-since-finished simulation. (Eviction window is
         * 60 s wall clock; we just verify that even after we drop
         * a fresh second run on top, the disk file for the first is
         * still available via the disk fallback path.) */
        src.registerRun(QStringLiteral("two"), QStringLiteral("explore"), makeAgent());
        QVERIFY(QFile::exists(src.transcriptPathFor(runId)));
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocsubagenttasksource.moc"
