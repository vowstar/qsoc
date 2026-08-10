// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctaskregistry.h"
#include "agent/qsoctasksource.h"
#include "agent/tool/qsoctoolagentstatus.h"
#include "qsoc_test.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuitaskoverlay.h"

#include <nlohmann/json.hpp>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;

    QSocAgent *makeAgent() { return new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig()); }

    /* What the `agent_status` tool reports for a run. */
    QString reportedStatus(QSocSubAgentTaskSource *src, const QString &id)
    {
        QSocToolAgentStatus tool(nullptr, src);
        const json          parsed = json::parse(
            tool.execute(json{{"task_id", id.toStdString()}}).toStdString());
        if (!parsed.contains("run_status")) {
            return QStringLiteral("<absent>");
        }
        return QString::fromStdString(parsed["run_status"].get<std::string>());
    }

    /* What the on-disk meta sidecar persists for a run. */
    QString persistedStatus(QSocSubAgentTaskSource *src, const QString &id)
    {
        QFile file(src->metaPathFor(id));
        if (!file.open(QIODevice::ReadOnly)) {
            return QStringLiteral("<no sidecar>");
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        return doc.object().value(QStringLiteral("status")).toString();
    }

private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    /* Counterexample: a killed run was cut off, so what it already did is
     * unknown. Persisting it as `failed` invites the parent to retry it. */
    void killedRunPersistsAbortedNotFailed()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("kill")));
        const QString id
            = src.registerRun(QStringLiteral("victim"), QStringLiteral("explore"), makeAgent());
        QVERIFY(src.killTask(id));

        QCOMPARE(persistedStatus(&src, id), QStringLiteral("aborted"));
        QCOMPARE(reportedStatus(&src, id), QStringLiteral("aborted"));
        QSocTask::Row row;
        QVERIFY(src.findRow(id, &row));
        QCOMPARE(row.status, QSocTask::Status::Aborted);
        QVERIFY(!row.canKill);
    }

    /* Same fact through the parent-ESC cascade. */
    void abortAllPersistsAbortedNotFailed()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("cascade")));
        const QString id
            = src.registerRun(QStringLiteral("victim"), QStringLiteral("explore"), makeAgent());
        src.abortAll();

        QCOMPARE(persistedStatus(&src, id), QStringLiteral("aborted"));
        QCOMPARE(reportedStatus(&src, id), QStringLiteral("aborted"));
        QSocTask::Row row;
        QVERIFY(src.findRow(id, &row));
        QCOMPARE(row.status, QSocTask::Status::Aborted);
    }

    /* The three terminal flavours must reach every sink under three
     * distinct names. */
    void terminalFlavoursDoNotCollide()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("flavours")));
        const QString okId
            = src.registerRun(QStringLiteral("ok"), QStringLiteral("explore"), makeAgent());
        const QString errId
            = src.registerRun(QStringLiteral("err"), QStringLiteral("explore"), makeAgent());
        const QString cutId
            = src.registerRun(QStringLiteral("cut"), QStringLiteral("explore"), makeAgent());
        src.markCompleted(okId, QStringLiteral("DONE TEXT"));
        src.markFailed(errId, QStringLiteral("LLM refused"));
        src.markAborted(cutId, QStringLiteral("stop requested"));

        QCOMPARE(persistedStatus(&src, okId), QStringLiteral("completed"));
        QCOMPARE(persistedStatus(&src, errId), QStringLiteral("failed"));
        QCOMPARE(persistedStatus(&src, cutId), QStringLiteral("aborted"));
        QCOMPARE(reportedStatus(&src, okId), QStringLiteral("completed"));
        QCOMPARE(reportedStatus(&src, errId), QStringLiteral("failed"));
        QCOMPARE(reportedStatus(&src, cutId), QStringLiteral("aborted"));
        QCOMPARE(QSocTask::statusWord(QSocTask::Status::Aborted), QStringLiteral("aborted"));

        /* The tail names the flavour too, in memory and after eviction. */
        QVERIFY(src.tailFor(cutId, 0).contains(QStringLiteral("=== aborted ===")));
        QVERIFY(src.tailFor(cutId, 0).contains(QStringLiteral("stop requested")));
    }

    /* A reloaded sidecar keeps the flavour: `agent_resume` reports it as
     * `original_status`, and `/agents-history` prints it. */
    void reloadedHistoryKeepsAborted()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("history")));
        const QString id
            = src.registerRun(QStringLiteral("cut"), QStringLiteral("explore"), makeAgent());
        src.markAborted(id, QStringLiteral("stop requested"));

        QSocSubAgentTaskSource::HistoricalRun run;
        QVERIFY(src.findHistoricalRun(id, &run));
        QCOMPARE(run.status, QStringLiteral("aborted"));
        QVERIFY(run.finishedAtMs > 0);
        QCOMPARE(run.error, QStringLiteral("stop requested"));
    }

    /* An aborted run is terminal, so it must age out of the panel like
     * any other finished run instead of pinning its child agent. */
    void abortedRunEvictsLikeOtherTerminalRuns()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("evict")));
        src.setCompletionTtlMs(0);
        const QString id
            = src.registerRun(QStringLiteral("cut"), QStringLiteral("explore"), makeAgent());
        src.markAborted(id, QStringLiteral("stop requested"));
        QCOMPARE(src.runCount(), 1);

        /* Registration is what sweeps; the aborted run must not survive it. */
        src.registerRun(QStringLiteral("next"), QStringLiteral("explore"), makeAgent());
        QCOMPARE(src.runCount(), 1);
        QVERIFY(!src.findRow(id, nullptr));
    }

    /* The task panel names the flavour too, so the user is not told a
     * cut-off child failed. */
    void overlayLabelsAbortedRun()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("overlay")));
        const QString id
            = src.registerRun(QStringLiteral("cut"), QStringLiteral("explore"), makeAgent());
        src.markAborted(id, QStringLiteral("stop requested"));

        QSocTaskRegistry registry;
        registry.registerSource(&src);
        QTuiTaskOverlay overlay;
        overlay.setRegistry(&registry);
        overlay.open();
        constexpr int kWidth = 80;
        QTuiScreen    screen(kWidth, overlay.lineCount());
        overlay.render(screen, 0, kWidth);

        QString rendered;
        for (int row = 0; row < overlay.lineCount(); ++row) {
            for (int col = 0; col < kWidth; ++col) {
                rendered += screen.at(col, row).character;
            }
            rendered += QLatin1Char('\n');
        }
        QVERIFY2(
            rendered.contains(QStringLiteral("aborted")),
            qPrintable(QStringLiteral("overlay did not label the run aborted:\n") + rendered));
    }

    /* An aborted run is a finished run, so it ranks with the finished
     * ones instead of falling into the registry's unknown-state bucket
     * below Failed. */
    void registryRanksAbortedWithFinishedRuns()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("rank")));
        const QString done
            = src.registerRun(QStringLiteral("done"), QStringLiteral("explore"), makeAgent());
        const QString cut
            = src.registerRun(QStringLiteral("cut"), QStringLiteral("explore"), makeAgent());
        const QString bad
            = src.registerRun(QStringLiteral("bad"), QStringLiteral("explore"), makeAgent());
        src.markCompleted(done, QStringLiteral("ok"));
        src.markAborted(cut, QStringLiteral("stop requested"));
        src.markFailed(bad, QStringLiteral("LLM refused"));

        QSocTaskRegistry registry;
        registry.registerSource(&src);
        QStringList order;
        for (const auto &tagged : registry.listAll()) {
            order.append(tagged.row.id);
        }
        QCOMPARE(order, QStringList({done, cut, bad}));
    }

    /* An abort frees its concurrency slot: the queue must not stall
     * because the terminal state changed name. */
    void abortedRunFreesConcurrencySlot()
    {
        QSocSubAgentTaskSource src;
        src.setTranscriptDir(dir.filePath(QStringLiteral("slot")));
        src.setMaxConcurrent(1);
        const QString first
            = src.registerRun(QStringLiteral("first"), QStringLiteral("explore"), makeAgent());
        const QString second
            = src.registerRun(QStringLiteral("second"), QStringLiteral("explore"), makeAgent());
        src.start(first, []() {});
        src.start(second, []() {});
        QCOMPARE(src.countRunning(), 1);

        src.markAborted(first, QStringLiteral("stop requested"));
        QSocTask::Row row;
        QVERIFY(src.findRow(second, &row));
        QCOMPARE(row.status, QSocTask::Status::Running);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsubagentabortstate.moc"
