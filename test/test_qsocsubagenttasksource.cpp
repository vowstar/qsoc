// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctasksource.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

class ScopedWaitTool final : public QSocTool
{
public:
    explicit ScopedWaitTool(QObject *parent = nullptr)
        : QSocTool(parent)
    {}

    QString getName() const override { return QStringLiteral("scoped_wait"); }
    QString getDescription() const override { return QStringLiteral("Wait for a test timer"); }
    json    getParametersSchema() const override
    {
        return {{"type", "object"}, {"properties", json::object()}};
    }

    QString execute(const json &arguments) override
    {
        QPointer<QSocToolCallContext> callContext(currentCallContext());
        const QString label = QString::fromStdString(arguments.value("label", std::string()));
        const int     delay = arguments.value("delay", 50);

        QEventLoop loop;
        WaitState  wait{&loop, false};
        const auto cancel = [&wait]() {
            wait.cancelled = true;
            if (wait.loop->isRunning()) {
                wait.loop->quit();
            }
        };
        if (!callContext.isNull()) {
            QObject::connect(
                callContext.data(), &QSocToolCallContext::cancellationRequested, &loop, cancel);
            if (callContext->isCancellationRequested()) {
                cancel();
            }
        }

        activeWaits_.insert(&wait);
        QTimer::singleShot(delay, &loop, &QEventLoop::quit);
        if (!wait.cancelled) {
            loop.exec();
        }
        activeWaits_.remove(&wait);

        return (wait.cancelled ? QStringLiteral("aborted:") : QStringLiteral("completed:")) + label;
    }

    void abort() override
    {
        ++globalAbortCount_;
        const QSet<WaitState *> waits = activeWaits_;
        for (WaitState *wait : waits) {
            wait->cancelled = true;
            if (wait->loop->isRunning()) {
                wait->loop->quit();
            }
        }
    }

    int globalAbortCount() const { return globalAbortCount_; }

private:
    struct WaitState
    {
        QEventLoop *loop;
        bool        cancelled;
    };

    QSet<WaitState *> activeWaits_;
    int               globalAbortCount_ = 0;
};

class Test : public QObject
{
    Q_OBJECT

private:
    /* Build a stand-alone agent QObject (no LLM service / registry). */
    QSocAgent *makeAgent() { return new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig()); }

private slots:
    void testRegisterEmitsAndPopulates()
    {
        QSocSubAgentTaskSource src;
        QSignalSpy             spy(&src, &QSocSubAgentTaskSource::tasksChanged);
        const QString          id = src.registerRun(
            QStringLiteral("read README"), QStringLiteral("general-purpose"), makeAgent());
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.count(), 1);
        auto rows = src.listTasks();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].id, id);
        QCOMPARE(rows[0].kind, QSocTask::Kind::SubAgent);
        /* A freshly registered run is queued (Pending) and cancellable;
         * it does not count as active until admitted by start(). */
        QCOMPARE(rows[0].status, QSocTask::Status::Pending);
        QCOMPARE(rows[0].label, QStringLiteral("read README"));
        QVERIFY(rows[0].canKill);
        QVERIFY(rows[0].summary.startsWith(QStringLiteral("general-purpose")));
        QCOMPARE(src.runCount(), 1);
        QVERIFY(!src.hasActiveRun());

        /* Admitting it (a slot is free) flips it Running and active. */
        src.start(id, []() {});
        rows = src.listTasks();
        QCOMPARE(rows[0].status, QSocTask::Status::Running);
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

    void testKillRunningWaitsForTerminal()
    {
        QSocSubAgentTaskSource src;
        const QString          id
            = src.registerRun(QStringLiteral("hung"), QStringLiteral("general-purpose"), makeAgent());
        src.start(id, []() {});
        QSignalSpy spy(&src, &QSocSubAgentTaskSource::tasksChanged);
        QVERIFY(src.killTask(id));
        QCOMPARE(spy.count(), 0);
        QCOMPARE(src.listTasks()[0].status, QSocTask::Status::Running);
        QVERIFY(src.hasActiveRun());

        src.markFailed(id, QStringLiteral("aborted"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(src.listTasks()[0].status, QSocTask::Status::Failed);
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

    void testKillRunningAbortsOnlySelectedAgentCall()
    {
        QSocToolRegistry registry;
        ScopedWaitTool   tool(&registry);
        registry.registerTool(&tool);

        QSocAgentConfig childConfig;
        childConfig.isSubAgent = true;
        auto *childA           = new QSocAgent(nullptr, nullptr, &registry, childConfig);
        auto *childB           = new QSocAgent(nullptr, nullptr, &registry, childConfig);

        QSocSubAgentTaskSource src;
        const QString idA = src.registerRun(QStringLiteral("a"), QStringLiteral("test"), childA);
        const QString idB = src.registerRun(QStringLiteral("b"), QStringLiteral("test"), childB);
        src.start(idA, []() {});
        src.start(idB, []() {});

        bool    killed   = false;
        bool    timedOut = false;
        QString resultB;
        QTimer::singleShot(0, &registry, [&]() {
            QTimer::singleShot(20, &registry, [&]() { killed = src.killTask(idA); });
            resultB = registry.executeTool(
                QStringLiteral("scoped_wait"), json{{"label", "b"}, {"delay", 100}}, childB);
        });
        QTimer::singleShot(2000, &registry, [&]() {
            timedOut = true;
            registry.abortAll();
        });

        const QString resultA = registry.executeTool(
            QStringLiteral("scoped_wait"), json{{"label", "a"}, {"delay", 1000}}, childA);

        QVERIFY(!timedOut);
        QVERIFY(killed);
        QCOMPARE(resultA, QStringLiteral("aborted:a"));
        QCOMPARE(resultB, QStringLiteral("completed:b"));
        QCOMPARE(tool.globalAbortCount(), 0);
    }

    void testRootAbortCancelsOnlyRootAgentCall()
    {
        QSocToolRegistry registry;
        ScopedWaitTool   tool(&registry);
        registry.registerTool(&tool);

        QSocAgentConfig childConfig;
        childConfig.isSubAgent = true;
        QSocAgent root(nullptr, nullptr, &registry, QSocAgentConfig());
        QSocAgent child(nullptr, nullptr, &registry, childConfig);

        bool    timedOut = false;
        QString childResult;
        QTimer::singleShot(0, &registry, [&]() {
            QTimer::singleShot(20, &registry, [&]() { root.abort(); });
            childResult = registry.executeTool(
                QStringLiteral("scoped_wait"), json{{"label", "child"}, {"delay", 1000}}, &child);
        });
        QTimer::singleShot(2000, &registry, [&]() {
            timedOut = true;
            registry.abortAll();
        });

        const QString rootResult = registry.executeTool(
            QStringLiteral("scoped_wait"), json{{"label", "root"}, {"delay", 1000}}, &root);

        QVERIFY(!timedOut);
        QCOMPARE(rootResult, QStringLiteral("aborted:root"));
        QCOMPARE(childResult, QStringLiteral("completed:child"));
        QCOMPARE(tool.globalAbortCount(), 0);
    }

    void testQueueRequestForPropagatesHardStopRejection()
    {
        QSocSubAgentTaskSource src;
        auto                  *child = makeAgent();
        const QString          id
            = src.registerRun(QStringLiteral("stopping"), QStringLiteral("test"), child);
        src.start(id, []() {});
        child->abortAndDiscardPendingRequests();

        QVERIFY(!src.queueRequestFor(id, QStringLiteral("must not queue")));
        QCOMPARE(child->pendingRequestCount(), 0);
    }

    void testPumpQueueSkipsPendingRunWithoutLauncher()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        src.setMaxConcurrent(1);

        const QString waiting
            = src.registerRun(QStringLiteral("waiting"), QStringLiteral("test"), makeAgent());
        const QString ready
            = src.registerRun(QStringLiteral("ready"), QStringLiteral("test"), makeAgent());
        int launchCount = 0;
        src.start(ready, [&]() { ++launchCount; });

        QSocTask::Row waitingRow;
        QSocTask::Row readyRow;
        QVERIFY(src.findRow(waiting, &waitingRow));
        QVERIFY(src.findRow(ready, &readyRow));
        QCOMPARE(waitingRow.status, QSocTask::Status::Pending);
        QCOMPARE(readyRow.status, QSocTask::Status::Running);
        QCOMPARE(launchCount, 1);
    }

    void testPumpQueueSurvivesTasksChangedRegistration()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        src.setMaxConcurrent(1);

        const QString first
            = src.registerRun(QStringLiteral("first"), QStringLiteral("test"), makeAgent());
        QString second;
        bool    injected = false;
        connect(&src, &QSocSubAgentTaskSource::tasksChanged, &src, [&]() {
            QSocTask::Row row;
            if (injected || !src.findRow(first, &row) || row.status != QSocTask::Status::Running) {
                return;
            }
            injected = true;
            second = src.registerRun(QStringLiteral("second"), QStringLiteral("test"), makeAgent());
            src.start(second, [&]() { src.markCompleted(second, QStringLiteral("done")); });
        });

        int firstLaunches = 0;
        src.start(first, [&]() {
            ++firstLaunches;
            src.markCompleted(first, QStringLiteral("done"));
        });

        QSocTask::Row secondRow;
        QVERIFY(injected);
        QCOMPARE(firstLaunches, 1);
        QVERIFY(src.findRow(second, &secondRow));
        QCOMPARE(secondRow.status, QSocTask::Status::Completed);
        QCOMPARE(src.countRunning(), 0);
    }

    void testPumpQueueObserverMayDeleteSource()
    {
        auto                            *src = new QSocSubAgentTaskSource;
        QPointer<QSocSubAgentTaskSource> owner(src);
        const QString                    id
            = src->registerRun(QStringLiteral("delete source"), QStringLiteral("test"), makeAgent());
        int launchCount = 0;
        connect(src, &QSocSubAgentTaskSource::tasksChanged, this, [src, id]() {
            QSocTask::Row row;
            if (src->findRow(id, &row) && row.status == QSocTask::Status::Running) {
                delete src;
            }
        });

        src->start(id, [&]() { ++launchCount; });
        QVERIFY(owner.isNull());
        QCOMPARE(launchCount, 0);
    }

    void testPumpQueueSurvivesLauncherRegistration()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        src.setMaxConcurrent(1);

        QStringList   started;
        const QString first
            = src.registerRun(QStringLiteral("first"), QStringLiteral("test"), makeAgent());
        QString second;
        src.start(first, [&]() {
            started.append(first);
            src.markCompleted(first, QStringLiteral("done"));
            second = src.registerRun(QStringLiteral("second"), QStringLiteral("test"), makeAgent());
            src.start(second, [&]() {
                started.append(second);
                src.markCompleted(second, QStringLiteral("done"));
            });
        });

        QCOMPARE(started, QStringList({first, second}));
        QCOMPARE(src.countRunning(), 0);
    }

    void testPumpQueueDropsLauncherKilledByTasksChanged()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());

        const QString id = src.registerRun(
            QStringLiteral("kill-before-launch"), QStringLiteral("test"), makeAgent());
        bool killed = false;
        connect(&src, &QSocSubAgentTaskSource::tasksChanged, &src, [&]() {
            QSocTask::Row row;
            if (killed || !src.findRow(id, &row) || row.status != QSocTask::Status::Running) {
                return;
            }
            killed = true;
            QVERIFY(src.killTask(id));
        });

        int launchCount = 0;
        src.start(id, [&]() { ++launchCount; });

        QSocTask::Row row;
        QVERIFY(killed);
        QVERIFY(src.findRow(id, &row));
        QCOMPARE(row.status, QSocTask::Status::Failed);
        QCOMPARE(launchCount, 0);
    }

    void testPumpQueueKeepsFirstPendingLauncher()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        src.setMaxConcurrent(1);

        const QString blocker
            = src.registerRun(QStringLiteral("blocker"), QStringLiteral("test"), makeAgent());
        src.start(blocker, []() {});
        const QString pending
            = src.registerRun(QStringLiteral("pending"), QStringLiteral("test"), makeAgent());

        int firstLaunches  = 0;
        int secondLaunches = 0;
        src.start(pending, [&]() { ++firstLaunches; });
        src.start(pending, [&]() { ++secondLaunches; });
        src.markCompleted(blocker, QStringLiteral("done"));

        QCOMPARE(firstLaunches, 1);
        QCOMPARE(secondLaunches, 0);
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

    void testLoadHistoricalRunsParsesMetaSidecars()
    {
        QTemporaryDir          tmp;
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());

        const QString done
            = src.registerRun(QStringLiteral("done-x"), QStringLiteral("explore"), makeAgent());
        src.markCompleted(done, QStringLiteral("PASSED"));
        const QString fail
            = src.registerRun(QStringLiteral("flaky-y"), QStringLiteral("verification"), makeAgent());
        src.markFailed(fail, QStringLiteral("network"));

        QSocSubAgentTaskSource freshSrc;
        freshSrc.setTranscriptDir(tmp.path());
        const auto runs = freshSrc.loadHistoricalRuns();
        QVERIFY(runs.size() >= 2);
        bool sawDone = false;
        bool sawFail = false;
        for (const auto &run : runs) {
            if (run.id == done && run.status == QStringLiteral("completed")) {
                sawDone = true;
            }
            if (run.id == fail && run.status == QStringLiteral("failed")) {
                sawFail = true;
            }
        }
        QVERIFY(sawDone);
        QVERIFY(sawFail);
    }

    void testLoadHistoricalRunsRewritesStaleRunning()
    {
        /* Hand-craft a "running" meta whose timestamp is 2 hours
         * old; loadHistoricalRuns must rewrite it as failed. */
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path());
        const QString metaPath = QDir(tmp.path()).filePath(QStringLiteral("a55.meta.json"));
        const qint64  twoHrAgo = QDateTime::currentMSecsSinceEpoch() - qint64{2} * 3600 * 1000;
        QJsonObject   meta;
        meta["task_id"]       = QStringLiteral("a55");
        meta["label"]         = QStringLiteral("ghost");
        meta["subagent_type"] = QStringLiteral("explore");
        meta["status"]        = QStringLiteral("running");
        meta["started_at_ms"] = twoHrAgo;
        meta["isolation"]     = QStringLiteral("none");
        QFile file(metaPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
        file.close();

        QSocSubAgentTaskSource src;
        src.setTranscriptDir(tmp.path());
        const auto runs         = src.loadHistoricalRuns(/*staleAgeSec*/ 60 * 60);
        bool       sawRewritten = false;
        for (const auto &run : runs) {
            if (run.id == QStringLiteral("a55")) {
                QCOMPARE(run.status, QStringLiteral("failed"));
                QVERIFY(run.error.contains(QStringLiteral("process restart")));
                sawRewritten = true;
            }
        }
        QVERIFY(sawRewritten);

        /* The meta file on disk has been rewritten too. */
        QFile reread(metaPath);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        const QJsonObject newObj = QJsonDocument::fromJson(reread.readAll()).object();
        QCOMPARE(newObj.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
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

QSOC_TEST_MAIN(Test)
#include "test_qsocsubagenttasksource.moc"
