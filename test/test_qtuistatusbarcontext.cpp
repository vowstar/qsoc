// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuistatusbar.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testHiddenWhenNoData()
    {
        /* No usage or no budget hides the chip entirely. */
        QVERIFY(QTuiStatusBar::formatContextChip(0, 100000, 0.6).isEmpty());
        QVERIFY(QTuiStatusBar::formatContextChip(5000, 0, 0.6).isEmpty());
    }

    void testFarFromThreshold()
    {
        /* Well below the compaction threshold: plain percentage only. */
        const QString chip = QTuiStatusBar::formatContextChip(10000, 100000, 0.6);
        QCOMPARE(chip, QStringLiteral(" [ctx 10%]"));
        QVERIFY(!chip.contains("to compact"));
    }

    void testApproachingThreshold()
    {
        /* Within 15% of the 60% threshold: show the countdown hint. */
        const QString chip = QTuiStatusBar::formatContextChip(50000, 100000, 0.6);
        QCOMPARE(chip, QStringLiteral(" [ctx 50%, 10% to compact]"));
    }

    void testAtOrOverThreshold()
    {
        /* At or above the threshold: compaction is imminent. */
        const QString chip = QTuiStatusBar::formatContextChip(65000, 100000, 0.6);
        QCOMPARE(chip, QStringLiteral(" [ctx 65%, compacting]"));
    }

    void testLowThresholdNoPrematureWarning()
    {
        /* With a low threshold (10%), a near-empty 3% context must not read
         * as "about to compact" — countdown stays off below half-threshold. */
        QCOMPARE(QTuiStatusBar::formatContextChip(3000, 100000, 0.10), QStringLiteral(" [ctx 3%]"));
        /* But at 8% (past half of the 10% threshold) the countdown shows. */
        QCOMPARE(
            QTuiStatusBar::formatContextChip(8000, 100000, 0.10),
            QStringLiteral(" [ctx 8%, 2% to compact]"));
    }

    void testNoThresholdGivesPlainPercent()
    {
        /* A zero compaction fraction yields a plain percentage, no hint. */
        const QString chip = QTuiStatusBar::formatContextChip(90000, 100000, 0.0);
        QCOMPARE(chip, QStringLiteral(" [ctx 90%]"));
    }

    /* The remote chip must be independent of the status text. Everything the
     * agent writes there ("Ready", the running tool) is a statement about the
     * agent, so a dead workspace used to sit behind an unchallenged "Ready". */
    void remoteChipIsIndependentOfStatusText()
    {
        QTuiStatusBar bar;
        QVERIFY(bar.remoteChip().isEmpty());

        bar.setStatus(QStringLiteral("Ready"));
        bar.setRemoteState(QStringLiteral("build01"), true);
        QCOMPARE(bar.remoteChip(), QStringLiteral(" [SSH:build01]"));

        /* A later status write must not clear it. */
        bar.setStatus(QStringLiteral("bash done, reasoning"));
        QCOMPARE(bar.remoteChip(), QStringLiteral(" [SSH:build01]"));

        bar.setRemoteState(QStringLiteral("build01"), false);
        QCOMPARE(bar.remoteChip(), QStringLiteral(" [SSH:build01 \u2717]"));

        /* Local mode has no link to report on. */
        bar.setRemoteState(QString(), true);
        QVERIFY(bar.remoteChip().isEmpty());
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qtuistatusbarcontext.moc"
