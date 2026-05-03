// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctaskeventqueue.h"
#include "agent/tool/qsoctoolmonitor.h"
#include "qsoc_test.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

QStringList eventKinds(const QSignalSpy &spy)
{
    QStringList out;
    for (const auto &args : spy) {
        const auto event = args.at(0).value<QSocTaskEvent>();
        out.append(event.kind + QLatin1Char(':') + event.content);
    }
    return out;
}

} /* namespace */

class Test : public QObject
{
    Q_OBJECT

private slots:
    void localMonitorEmitsLineEventsAndCompletion()
    {
        QSocTaskEventQueue    queue;
        QSocMonitorTaskSource source(nullptr, &queue, nullptr);
        QSignalSpy            spy(&queue, &QSocTaskEventQueue::taskEventQueued);

        const auto started = source.startLocal(
            QStringLiteral("printf 'a\\nb\\n'"), QStringLiteral("two lines"), 0, false);
        QVERIFY2(started.ok, qPrintable(started.error));

        QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 4, 3000);
        const QStringList kinds = eventKinds(spy);
        QVERIFY(kinds.contains(QStringLiteral("task_started:two lines")));
        QVERIFY(kinds.contains(QStringLiteral("monitor_line:a")));
        QVERIFY(kinds.contains(QStringLiteral("monitor_line:b")));
        QVERIFY(
            kinds.join(QLatin1Char('\n')).contains(QStringLiteral("task_notification:completed")));
        QVERIFY(source.tailFor(started.taskId, 1024).contains(QStringLiteral("a\nb")));
    }

    void localMonitorFlushesPartialLineOnExit()
    {
        QSocTaskEventQueue    queue;
        QSocMonitorTaskSource source(nullptr, &queue, nullptr);
        QSignalSpy            spy(&queue, &QSocTaskEventQueue::taskEventQueued);

        const auto started = source.startLocal(
            QStringLiteral("printf partial"), QStringLiteral("partial"), 0, false);
        QVERIFY2(started.ok, qPrintable(started.error));

        QTRY_VERIFY_WITH_TIMEOUT(
            eventKinds(spy).contains(QStringLiteral("monitor_line:partial")), 3000);
    }

    void monitorStopEmitsStoppedNotification()
    {
        QSocTaskEventQueue    queue;
        QSocMonitorTaskSource source(nullptr, &queue, nullptr);
        QSignalSpy            spy(&queue, &QSocTaskEventQueue::taskEventQueued);

        const auto started = source.startLocal(
            QStringLiteral("while true; do sleep 1; done"), QStringLiteral("long"), 0, true);
        QVERIFY2(started.ok, qPrintable(started.error));
        QVERIFY(source.stop(started.taskId));

        const auto rows = source.listTasks();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().status, QSocTask::Status::Completed);
        QVERIFY(!rows.first().canKill);

        QTRY_VERIFY_WITH_TIMEOUT(
            eventKinds(spy)
                .join(QLatin1Char('\n'))
                .contains(QStringLiteral("task_notification:stopped")),
            3000);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmonitortasksource.moc"
