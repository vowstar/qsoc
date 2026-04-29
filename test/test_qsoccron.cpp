// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccron.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QObject>
#include <QtTest>

class TestQSocCron : public QObject
{
    Q_OBJECT

private slots:
    /* ---- isValid ---- */

    void isValid_classicalRecurring()
    {
        QVERIFY(QSocCron::isValid("*/5 * * * *"));
        QVERIFY(QSocCron::isValid("0 9 * * 1-5"));
        QVERIFY(QSocCron::isValid("0,30 * * * *"));
        QVERIFY(QSocCron::isValid("0 */2 * * *"));
        QVERIFY(QSocCron::isValid("0 0 */1 * *"));
    }

    void isValid_pinnedOneShot()
    {
        QVERIFY(QSocCron::isValid("30 14 27 2 *"));
        QVERIFY(QSocCron::isValid("0 0 1 1 *"));
    }

    void isValid_dowSundayAlias()
    {
        QVERIFY(QSocCron::isValid("0 9 * * 7"));
        QVERIFY(QSocCron::isValid("0 0 * * 5-7"));
    }

    void isValid_rejectsMalformed()
    {
        QVERIFY(!QSocCron::isValid(""));
        QVERIFY(!QSocCron::isValid("* * * *"));
        QVERIFY(!QSocCron::isValid("* * * * * *"));
        QVERIFY(!QSocCron::isValid("60 * * * *"));
        QVERIFY(!QSocCron::isValid("* 24 * * *"));
        QVERIFY(!QSocCron::isValid("* * 0 * *"));
        QVERIFY(!QSocCron::isValid("* * 32 * *"));
        QVERIFY(!QSocCron::isValid("* * * 13 *"));
        QVERIFY(!QSocCron::isValid("* * * * 8"));
        QVERIFY(!QSocCron::isValid("*/0 * * * *"));
        QVERIFY(!QSocCron::isValid("5-3 * * * *"));
        QVERIFY(!QSocCron::isValid("foo * * * *"));
    }

    /* ---- nextRunMs ---- */

    void next_minuteBoundary()
    {
        const QDateTime base(QDate(2026, 4, 29), QTime(14, 23, 30));
        const qint64    next = QSocCron::nextRunMs("*/5 * * * *", base.toMSecsSinceEpoch());
        QVERIFY(next > 0);
        const QDateTime got = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.time().minute(), 25);
        QCOMPARE(got.time().second(), 0);
    }

    void next_strictlyGreaterThanFromMs()
    {
        const QDateTime exact(QDate(2026, 4, 29), QTime(14, 25, 0));
        const qint64    next = QSocCron::nextRunMs("*/5 * * * *", exact.toMSecsSinceEpoch());
        QVERIFY(next > exact.toMSecsSinceEpoch());
        const QDateTime got = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.time().minute(), 30);
    }

    void next_dayWrap()
    {
        const QDateTime base(QDate(2026, 4, 29), QTime(23, 59));
        const qint64    next = QSocCron::nextRunMs("0 0 * * *", base.toMSecsSinceEpoch());
        const QDateTime got  = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.date(), QDate(2026, 4, 30));
        QCOMPARE(got.time(), QTime(0, 0));
    }

    void next_monthWrap()
    {
        const QDateTime base(QDate(2026, 4, 30), QTime(23, 59));
        const qint64    next = QSocCron::nextRunMs("0 0 * * *", base.toMSecsSinceEpoch());
        const QDateTime got  = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.date(), QDate(2026, 5, 1));
    }

    void next_leapDay()
    {
        const QDateTime base(QDate(2024, 2, 27), QTime(0, 0));
        const qint64    next = QSocCron::nextRunMs("0 12 29 2 *", base.toMSecsSinceEpoch());
        const QDateTime got  = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.date(), QDate(2024, 2, 29));
        QCOMPARE(got.time(), QTime(12, 0));
    }

    void next_invalidDateSkipped()
    {
        /* Feb 30 never exists; cron expression is otherwise valid. The
         * scanner should fail to find a match within the year and return 0. */
        const QDateTime base(QDate(2026, 1, 1), QTime(0, 0));
        const qint64    next = QSocCron::nextRunMs("0 0 30 2 *", base.toMSecsSinceEpoch());
        QCOMPARE(next, qint64(0));
    }

    void next_dowDomOrSemantics()
    {
        /* "first of month OR Monday" — fires on 2026-05-01 (Friday) because
         * dom=1; without OR semantics it would skip to next Monday. */
        const QDateTime base(QDate(2026, 4, 28), QTime(0, 0));
        const qint64    next = QSocCron::nextRunMs("0 0 1 * 1", base.toMSecsSinceEpoch());
        const QDateTime got  = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.date(), QDate(2026, 5, 1));
    }

    void next_dowSundayMatches7Or0()
    {
        const QDateTime base(QDate(2026, 4, 28), QTime(0, 0)); /* Tue */
        const qint64    via0 = QSocCron::nextRunMs("0 9 * * 0", base.toMSecsSinceEpoch());
        const qint64    via7 = QSocCron::nextRunMs("0 9 * * 7", base.toMSecsSinceEpoch());
        QCOMPARE(via0, via7);
        const QDateTime got = QDateTime::fromMSecsSinceEpoch(via0);
        QCOMPARE(got.date().dayOfWeek(), 7); /* Qt: 7=Sunday */
    }

    void next_invalidExprReturnsZero()
    {
        const qint64 next = QSocCron::nextRunMs("totally bogus", 0);
        QCOMPARE(next, qint64(0));
    }

    void next_yearWrap()
    {
        const QDateTime base(QDate(2026, 12, 31), QTime(23, 59));
        const qint64    next = QSocCron::nextRunMs("0 0 1 1 *", base.toMSecsSinceEpoch());
        const QDateTime got  = QDateTime::fromMSecsSinceEpoch(next);
        QCOMPARE(got.date(), QDate(2027, 1, 1));
    }

    /* ---- intervalToCron ---- */

    void interval_cleanMinutes()
    {
        QCOMPARE(QSocCron::intervalToCron("5m"), QString("*/5 * * * *"));
        QCOMPARE(QSocCron::intervalToCron("15m"), QString("*/15 * * * *"));
        QCOMPARE(QSocCron::intervalToCron("30m"), QString("*/30 * * * *"));
    }

    void interval_minutesAsHours()
    {
        QCOMPARE(QSocCron::intervalToCron("60m"), QString("0 */1 * * *"));
        QCOMPARE(QSocCron::intervalToCron("120m"), QString("0 */2 * * *"));
    }

    void interval_hours()
    {
        QCOMPARE(QSocCron::intervalToCron("1h"), QString("0 */1 * * *"));
        QCOMPARE(QSocCron::intervalToCron("2h"), QString("0 */2 * * *"));
        QCOMPARE(QSocCron::intervalToCron("12h"), QString("0 */12 * * *"));
    }

    void interval_days()
    {
        QCOMPARE(QSocCron::intervalToCron("1d"), QString("0 0 */1 * *"));
        QCOMPARE(QSocCron::intervalToCron("7d"), QString("0 0 */7 * *"));
    }

    void interval_secondsRoundUpToMinutes()
    {
        QCOMPARE(QSocCron::intervalToCron("30s"), QString("*/1 * * * *"));
        QCOMPARE(QSocCron::intervalToCron("60s"), QString("*/1 * * * *"));
        QCOMPARE(QSocCron::intervalToCron("61s"), QString("*/2 * * * *"));
        QCOMPARE(QSocCron::intervalToCron("300s"), QString("*/5 * * * *"));
    }

    void interval_rejectsNonClean()
    {
        QCOMPARE(QSocCron::intervalToCron("7m"), QString());
        QCOMPARE(QSocCron::intervalToCron("90m"), QString());
        QCOMPARE(QSocCron::intervalToCron("5h"), QString());
        QCOMPARE(QSocCron::intervalToCron("0m"), QString());
        QCOMPARE(QSocCron::intervalToCron(""), QString());
        QCOMPARE(QSocCron::intervalToCron("foo"), QString());
        QCOMPARE(QSocCron::intervalToCron("5"), QString());
    }

    /* ---- cronToHuman ---- */

    void human_recognizedShapes()
    {
        QCOMPARE(QSocCron::cronToHuman("*/5 * * * *"), QString("Every 5m"));
        QCOMPARE(QSocCron::cronToHuman("*/1 * * * *"), QString("Every minute"));
        QCOMPARE(QSocCron::cronToHuman("0 */2 * * *"), QString("Every 2h at :00"));
        QCOMPARE(QSocCron::cronToHuman("0 0 */1 * *"), QString("Daily 00:00"));
        QCOMPARE(QSocCron::cronToHuman("30 14 27 2 *"), QString("Once 02-27 14:30"));
    }

    void human_unrecognizedFallsThroughToRaw()
    {
        const QString weird = "0 9 1-5 * 1-5";
        QCOMPARE(QSocCron::cronToHuman(weird), weird);
    }
};

QSOC_TEST_MAIN(TestQSocCron)

#include "test_qsoccron.moc"
