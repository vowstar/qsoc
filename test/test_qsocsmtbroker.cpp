// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "smt/qsocsmtbroker.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <QJsonArray>
#include <QScopeGuard>
#include <QtTest>

namespace {

class Gate
{
public:
    QJsonObject solve(const QJsonObject &request, std::stop_token stop)
    {
        const auto label = request.value("smtlib").toString();
        {
            const std::lock_guard lock(mutex);
            started.append(label);
            ++active;
            peak = qMax(peak, active);
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [&] { return released.contains(label); });
        if (stop.stop_requested()) {
            ++stopping;
            changed.wait(lock, [&] { return allowExit; });
        }
        --active;
        return {{"execution", "completed"}, {"solver_status", "sat"}};
    }

    void release(const QString &label)
    {
        const std::lock_guard lock(mutex);
        released.insert(label);
        changed.notify_all();
    }

    void finishStops()
    {
        const std::lock_guard lock(mutex);
        allowExit = true;
        changed.notify_all();
    }

    QStringList order()
    {
        const std::lock_guard lock(mutex);
        return started;
    }

    std::mutex                  mutex;
    std::condition_variable_any changed;
    QSet<QString>               released;
    QStringList                 started;
    int                         active    = 0;
    int                         peak      = 0;
    std::atomic<int>            stopping  = 0;
    bool                        allowExit = false;
};

QJsonObject request(const QString &label)
{
    return {{"smtlib", label}, {"mode", "check"}, {"timeout_ms", 1000}};
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void boundedConcurrencyAndFairOwners()
    {
        Gate          gate;
        QSocSmtBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto    cleanup = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        QStringList   results;
        auto          submit = [&](quint64 owner, const QString &label) {
            return broker.submit(owner, request(label), [&, label](const auto &) {
                results.append(label);
            });
        };
        submit(1, "first");
        submit(1, "second");
        submit(1, "a1");
        submit(1, "a2");
        submit(2, "b1");
        submit(3, "c1");
        QTRY_COMPARE(gate.order().size(), 2);
        QCOMPARE(broker.activeCount(), 2);
        QCOMPARE(broker.queuedCount(), 4);
        gate.release("first");
        QTRY_VERIFY(gate.order().contains("a1"));
        gate.release("a1");
        QTRY_VERIFY(gate.order().contains("b1"));
        gate.release("b1");
        QTRY_VERIFY(gate.order().contains("c1"));
        gate.release("c1");
        QTRY_VERIFY(gate.order().contains("a2"));
        gate.release("a2");
        gate.release("second");
        QTRY_COMPARE(results.size(), 6);
        QCOMPARE(broker.activeCount(), 0);
        QCOMPARE(gate.peak, 2);
    }

    void cancelRetainsRunningSlotUntilExit()
    {
        Gate               gate;
        QSocSmtBroker      broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto         cleanup = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        QList<QJsonObject> replies;
        const auto         first = broker.submit(1, request("first"), [&](const auto &result) {
            replies.append(result);
        });
        broker.submit(2, request("second"), [&](const auto &result) { replies.append(result); });
        broker.submit(3, request("third"), [&](const auto &result) { replies.append(result); });
        QTRY_COMPARE(gate.order().size(), 2);
        QVERIFY(broker.cancel(first));
        QTRY_COMPARE(gate.stopping.load(), 1);
        QCOMPARE(broker.activeCount(), 2);
        QCOMPARE(broker.queuedCount(), 1);
        QVERIFY(!gate.order().contains("third"));
        QVERIFY(replies.isEmpty());
        gate.finishStops();
        QTRY_VERIFY(gate.order().contains("third"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.first().value("execution").toString(), "cancelled");
        QVERIFY(!broker.cancel(first));
        gate.release("second");
        gate.release("third");
        QTRY_COMPARE(replies.size(), 3);
    }

    void ownerQuotaAndQueuedCancellation()
    {
        Gate          gate;
        QSocSmtBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto    cleanup   = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        int           cancelled = 0;
        int           busy      = 0;
        auto          reply     = [&](const QJsonObject &result) {
            cancelled += result.value("execution") == "cancelled";
            busy += result.value("execution") == "busy";
        };
        QList<quint64> tasks;
        for (int i = 0; i < 16; ++i)
            tasks.append(broker.submit(1, request(QString::number(i)), reply));
        QCOMPARE(broker.submit(1, request("overflow"), reply), quint64(0));
        QCOMPARE(busy, 1);
        QCOMPARE(broker.queuedCount(), 14);
        QVERIFY(broker.cancel(tasks.last()));
        QCOMPARE(cancelled, 1);
        QCOMPARE(broker.queuedCount(), 13);
        broker.removeOwner(1);
        QCOMPARE(broker.queuedCount(), 0);
        QCOMPARE(cancelled, 14);
        gate.finishStops();
        QTRY_COMPARE(broker.activeCount(), 0);
        QCOMPARE(cancelled, 16);
    }

    void queuedDeadlineAndGlobalLimit()
    {
        Gate          gate;
        QSocSmtBroker broker(
            nullptr, [&gate](const auto &input, auto stop) { return gate.solve(input, stop); }, 250);
        const auto cleanup  = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        int        timedOut = 0;
        int        busy     = 0;
        auto       reply    = [&](const QJsonObject &result) {
            timedOut += result.value("execution") == "timeout";
            busy += result.value("execution") == "busy";
        };
        for (int i = 0; i < 66; ++i)
            QVERIFY(broker.submit(i + 1, request(QString::number(i)), reply));
        QCOMPARE(broker.activeCount(), 2);
        QCOMPARE(broker.queuedCount(), 64);
        QCOMPARE(broker.submit(100, request("overflow"), reply), quint64(0));
        QCOMPARE(busy, 1);
        QTRY_COMPARE(timedOut, 64);
        QCOMPARE(broker.activeCount(), 2);
        QCOMPARE(broker.queuedCount(), 0);
    }

    void serializedQueueBytesAreBoundedAndReclaimed()
    {
        Gate          gate;
        QSocSmtBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto    cleanup = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        broker.submit(1, request("first"), [](const auto &) {});
        broker.submit(2, request("second"), [](const auto &) {});
        const auto large = request(QString(256 * 1024, QChar(1)));
        int        busy  = 0;
        auto reply = [&](const QJsonObject &result) { busy += result.value("execution") == "busy"; };
        QList<quint64> accepted;
        for (int i = 0; i < 32 && busy == 0; ++i) {
            const auto task = broker.submit(i + 3, large, reply);
            if (task)
                accepted.append(task);
        }
        QCOMPARE(busy, 1);
        QVERIFY(!accepted.isEmpty());
        QVERIFY(accepted.size() < 32);
        QVERIFY(broker.cancel(accepted.first()));
        QVERIFY(broker.submit(100, large, reply));
        QCOMPARE(busy, 1);
    }

    void shutdownStopsActiveTasks()
    {
        Gate          gate;
        QSocSmtBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        broker.submit(1, request("first"), [](const auto &) {});
        broker.submit(2, request("second"), [](const auto &) {});
        QTRY_COMPARE(gate.order().size(), 2);
        gate.finishStops();
        broker.shutdown();
        QCOMPARE(broker.activeCount(), 0);
        QCOMPARE(broker.queuedCount(), 0);
        QCOMPARE(gate.stopping.load(), 2);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsmtbroker.moc"
