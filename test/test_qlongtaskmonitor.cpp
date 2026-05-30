// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlongtaskmonitor.h"
#include "qsoc_test.h"

#include <unistd.h>

#include <QObject>
#include <QSignalSpy>
#include <QtCore>
#include <QtTest>

namespace {

/* Tests use compressed timings (tick 25 ms, stall 100 ms) so the whole
 * suite finishes in a couple of seconds. Production defaults are 1 s /
 * 30 s / 5 min. */
QLongTaskMonitor::Config fastConfig(int wallClockMs = 0)
{
    return QLongTaskMonitor::Config{
        25 /*tickIntervalMs*/,
        100 /*stallThresholdMs*/,
        wallClockMs,
        2 /*consecutiveIdleTicks*/,
    };
}

class TestQLongTaskMonitor : public QObject
{
    Q_OBJECT

private slots:
    void progressThenFinishEmitsNoStall()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        /* Keep beating the progress drum across several tick intervals.
         * Stall must never fire. */
        for (int i = 0; i < 12; ++i) {
            QTest::qWait(25);
            monitor.notifyProgress();
        }
        monitor.finish();
        QCOMPARE(stallSpy.count(), 0);
    }

    void silenceBeyondThresholdEmitsStallOnce()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        /* Threshold 100 ms + 2 idle ticks at 25 ms = ~150 ms. Wait 300
         * to be safely past the debounce. */
        QTest::qWait(300);
        QCOMPARE(stallSpy.count(), 1);
        /* Staying silent must not re-fire. */
        QTest::qWait(200);
        QCOMPARE(stallSpy.count(), 1);
        monitor.finish();
    }

    void shortSilenceUnderThresholdNeverEmits()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        /* Brief gap (< 100 ms) then progress: debounce should swallow. */
        QTest::qWait(50);
        monitor.notifyProgress();
        QTest::qWait(50);
        monitor.notifyProgress();
        QCOMPARE(stallSpy.count(), 0);
        monitor.finish();
    }

    void stallReArmsAfterProgress()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        QTest::qWait(300);
        QCOMPARE(stallSpy.count(), 1);
        /* Progress arrives, rearming the detector for the next gap. */
        monitor.notifyProgress();
        QTest::qWait(300);
        QCOMPARE(stallSpy.count(), 2);
        monitor.finish();
    }

    void wallClockFiresOnceWhenExceeded()
    {
        QLongTaskMonitor monitor(this, fastConfig(/*wallClockMs=*/150));
        QSignalSpy       wallSpy(&monitor, &QLongTaskMonitor::wallClockExceeded);
        monitor.start();
        /* Keep feeding progress so stall stays quiet; wall clock still
         * trips regardless of progress. */
        for (int i = 0; i < 16; ++i) {
            QTest::qWait(25);
            monitor.notifyProgress();
        }
        QVERIFY(wallSpy.count() >= 1);
        const int firstCount = wallSpy.count();
        QTest::qWait(150);
        QCOMPARE(wallSpy.count(), firstCount);
        monitor.finish();
    }

    void cancelEmitsOnceAndStopsTicking()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       cancelSpy(&monitor, &QLongTaskMonitor::cancelled);
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        monitor.cancel(QStringLiteral("under test"));
        QCOMPARE(cancelSpy.count(), 1);
        QCOMPARE(cancelSpy.at(0).at(0).toString(), QStringLiteral("under test"));
        QVERIFY(monitor.isCancelled());
        /* Already past terminate. Even a long wait must not raise any
         * further signal. */
        QTest::qWait(300);
        QCOMPARE(cancelSpy.count(), 1);
        QCOMPARE(stallSpy.count(), 0);
        /* Second cancel is a no-op. */
        monitor.cancel(QStringLiteral("redundant"));
        QCOMPARE(cancelSpy.count(), 1);
    }

    void notifyAfterFinishIsNoOp()
    {
        QLongTaskMonitor monitor(this, fastConfig());
        QSignalSpy       stallSpy(&monitor, &QLongTaskMonitor::stalled);
        monitor.start();
        monitor.finish();
        /* Late progress events must neither crash nor reactivate. */
        for (int i = 0; i < 6; ++i) {
            monitor.notifyProgress();
        }
        QTest::qWait(200);
        QCOMPARE(stallSpy.count(), 0);
    }

    void parentDestructionDuringRunIsSafe()
    {
        auto      *parent  = new QObject();
        auto      *monitor = new QLongTaskMonitor(parent, fastConfig());
        QSignalSpy stallSpy(monitor, &QLongTaskMonitor::stalled);
        monitor->start();
        QTest::qWait(50);
        /* Parent destruction must auto-stop the timer via Qt's child
         * cleanup. No segfault on subsequent event-loop spins. */
        delete parent;
        QTest::qWait(200);
        /* No assertion needed beyond the implicit "did not crash". */
    }

    void backToBackTerminateOnlyOneWins()
    {
        /* finish() and cancel() may both be called from the same slot
         * if a stalled() handler decides to cancel just as the stream
         * completes. The CAS on terminated_ must let exactly one path
         * run; the second call is a no-op. */
        {
            QLongTaskMonitor monitor(this, fastConfig());
            QSignalSpy       cancelSpy(&monitor, &QLongTaskMonitor::cancelled);
            monitor.start();
            monitor.finish();
            monitor.cancel(QStringLiteral("after finish"));
            QCOMPARE(cancelSpy.count(), 0);
            QVERIFY(!monitor.isCancelled());
        }
        {
            QLongTaskMonitor monitor(this, fastConfig());
            QSignalSpy       cancelSpy(&monitor, &QLongTaskMonitor::cancelled);
            monitor.start();
            monitor.cancel(QStringLiteral("first"));
            monitor.finish();
            QCOMPARE(cancelSpy.count(), 1);
            QVERIFY(monitor.isCancelled());
        }
    }
};

} // namespace

QSOC_TEST_MAIN(TestQLongTaskMonitor)
#include "test_qlongtaskmonitor.moc"
