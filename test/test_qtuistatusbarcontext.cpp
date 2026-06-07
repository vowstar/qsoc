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
};

QSOC_TEST_MAIN(Test)

#include "test_qtuistatusbarcontext.moc"
