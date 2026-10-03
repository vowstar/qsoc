// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "smt/qsocsmtbroker.h"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <QAbstractEventDispatcher>
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

class TestBroker : public QSocSmtBroker
{
public:
    TestBroker(
        QObject                 *parent,
        Solver                   solver,
        int                      queueWaitMs = 120000,
        QSocMemoryBudget::Policy policy      = {},
        Sampler                  sampler =
            [] {
                return QSocMemoryBudget::Snapshot{
                    quint64(4) * 1024 * 1024 * 1024,
                    QSocMemoryBudget::Coverage::Effective,
                    QSocMemoryBudget::Clock::now(),
                    "test"};
            })
        : QSocSmtBroker(parent, std::move(solver), queueWaitMs, policy, std::move(sampler))
    {}
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void memoryAdmissionUsesCompleteRequirements()
    {
        using namespace QSocMemoryBudget;
        const auto now = Clock::now();
        Snapshot   sample{1024, Coverage::Effective, now, "test"};
        QCOMPARE(evaluate({}, sample, 512, 512, now), Admission::Admitted);
        QCOMPARE(evaluate({false, 1}, sample, 512, 512, now), Admission::WaitingMemory);
        QCOMPARE(
            evaluate({}, sample, std::numeric_limits<quint64>::max(), 512, now),
            Admission::WaitingMemory);
        sample.availableBytes = 0;
        QCOMPARE(evaluate({}, sample, 0, 1, now), Admission::WaitingMemory);
        sample.availableBytes.reset();
        QCOMPARE(evaluate({}, sample, 0, 512, now), Admission::Unverified);
        QCOMPARE(evaluate({true, 0}, sample, 0, 512, now), Admission::WaitingMeasurement);
        sample.availableBytes = 1024;
        sample.sampledAt      = now - std::chrono::seconds(2);
        QCOMPARE(evaluate({}, sample, 0, 512, now), Admission::Unverified);
        sample.sampledAt = now + std::chrono::seconds(1);
        QCOMPARE(evaluate({true, 0}, sample, 0, 512, now), Admission::WaitingMeasurement);
        sample.sampledAt = now;
        sample.coverage  = Coverage::HostOnly;
        QCOMPARE(evaluate({}, sample, 0, 512, now), Admission::Partial);
        QCOMPARE(evaluate({true, 0}, sample, 0, 512, now), Admission::WaitingMeasurement);
        sample.availableBytes = 1;
        QCOMPARE(evaluate({}, sample, 0, 512, now), Admission::WaitingMemory);
    }

    void memoryWaitRecoversAndRetainsReservationsUntilExit()
    {
        Gate              gate;
        quint64           available   = 0;
        constexpr quint64 workerBytes = 512 * 1024 * 1024;
        TestBroker        broker(
            nullptr,
            [&gate](const auto &input, auto stop) { return gate.solve(input, stop); },
            5000,
            {},
            [&] {
                return QSocMemoryBudget::Snapshot{
                    available,
                    QSocMemoryBudget::Coverage::Effective,
                    QSocMemoryBudget::Clock::now(),
                    "test"};
            });
        const auto cleanup = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        int        replies = 0;
        const auto first   = broker.submit(1, request("first"), [&](const auto &) { ++replies; });
        broker.submit(2, request("second"), [&](const auto &) { ++replies; });
        QCOMPARE(broker.activeCount(), 0);
        QCOMPARE(broker.queuedCount(), 2);
        QCOMPARE(broker.resourceStatus().value("admission").toString(), "waiting_memory");
        available = workerBytes;
        QTRY_COMPARE(gate.order().size(), 1);
        QCOMPARE(broker.activeCount(), 1);
        QCOMPARE(broker.resourceStatus().value("reserved_bytes").toInteger(), qint64(workerBytes));
        QVERIFY(broker.cancel(first));
        QTRY_COMPARE(gate.stopping.load(), 1);
        QCOMPARE(broker.activeCount(), 1);
        QCOMPARE(broker.queuedCount(), 1);
        QCOMPARE(replies, 0);
        gate.finishStops();
        QTRY_VERIFY(gate.order().contains("second"));
        QCOMPARE(replies, 1);
        gate.release("second");
        QTRY_COMPARE(replies, 2);
        QCOMPARE(broker.resourceStatus().value("reserved_bytes").toInteger(), qint64(0));
    }

    void memoryPollingDoesNotExtendTheQueueDeadline()
    {
        int        samples = 0;
        int        started = 0;
        TestBroker broker(
            nullptr,
            [&](const auto &, auto) {
                ++started;
                return QJsonObject{};
            },
            1250,
            {},
            [&] {
                ++samples;
                return QSocMemoryBudget::Snapshot{
                    0,
                    QSocMemoryBudget::Coverage::Effective,
                    QSocMemoryBudget::Clock::now(),
                    "test"};
            });
        QJsonObject result;
        broker.submit(1, request("waiting"), [&](const auto &reply) { result = reply; });
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
        QVERIFY(samples >= 2);
        QCOMPARE(started, 0);
        QCOMPARE(result.value("execution").toString(), "timeout");
        QVERIFY(result.value("reason").toString().contains("memory budget"));
        QCOMPARE(broker.queuedCount(), 0);
    }

    void unknownSamplingHasExplicitStrictMode()
    {
        std::atomic<int> started = 0;
        auto             solver  = [&](const auto &, auto) {
            ++started;
            return QJsonObject{{"execution", "resource_limit"}};
        };
        TestBroker         permissive(nullptr, solver, 100, {}, [] {
            return QSocMemoryBudget::Snapshot{
                1024,
                QSocMemoryBudget::Coverage::Effective,
                QSocMemoryBudget::Clock::now() - std::chrono::seconds(2),
                "test"};
        });
        QList<QJsonObject> replies;
        permissive.submit(1, request("first"), [&](const auto &result) { replies.append(result); });
        QCOMPARE(permissive.resourceStatus().value("unverified_active").toInt(), 1);
        QVERIFY(permissive.resourceStatus().value("available_bytes").isNull());
        QVERIFY(!permissive.resourceStatus().value("sample_fresh").toBool());
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(replies.first().value("execution").toString(), "resource_limit");
        QVERIFY(!replies.first().contains("resource_status"));
        QCOMPARE(started.load(), 1);
        TestBroker strict(nullptr, solver, 100, {true, 0}, [] {
            return QSocMemoryBudget::Snapshot{};
        });
        strict.submit(1, request("second"), [&](const auto &result) { replies.append(result); });
        QCOMPARE(strict.activeCount(), 0);
        QCOMPARE(strict.queuedCount(), 1);
        QTRY_COMPARE(replies.size(), 2);
        QCOMPARE(replies.last().value("execution").toString(), "timeout");
        QVERIFY(replies.last().value("reason").toString().contains("unverified"));
        QCOMPARE(strict.queuedCount(), 0);
        QCOMPARE(started.load(), 1);
    }

    void boundedConcurrencyAndFairOwners()
    {
        Gate        gate;
        TestBroker  broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto  cleanup = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        QStringList results;
        auto        submit = [&](quint64 owner, const QString &label) {
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
        TestBroker         broker(nullptr, [&gate](const auto &input, auto stop) {
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
        Gate       gate;
        TestBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto cleanup   = qScopeGuard([&] {
            gate.finishStops();
            broker.shutdown();
        });
        int        cancelled = 0;
        int        busy      = 0;
        auto       reply     = [&](const QJsonObject &result) {
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
        Gate       gate;
        TestBroker broker(
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
        Gate       gate;
        TestBroker broker(nullptr, [&gate](const auto &input, auto stop) {
            return gate.solve(input, stop);
        });
        const auto cleanup = qScopeGuard([&] {
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

    void completedTasksReleaseTheirQueueDeadlines()
    {
        TestBroker broker(nullptr, [](const auto &, auto) {
            return QJsonObject{{"execution", "completed"}};
        });
        auto      *dispatcher = QAbstractEventDispatcher::instance();
        QVERIFY(dispatcher);
        auto timers = [&] {
            const auto objects = dispatcher->findChildren<QObject *>()
                                 + broker.findChildren<QObject *>();
            int        count   = 0;
            for (auto *object : objects)
                count += dispatcher->registeredTimers(object).size();
            return count;
        };
        const int baseline  = timers();
        int       completed = 0;
        for (int batch = 0; batch < 8; ++batch) {
            for (int task = 0; task < 8; ++task)
                QVERIFY(broker.submit(task + 1, request("(assert true)"), [&](const auto &) {
                    ++completed;
                }));
            QTRY_COMPARE(completed, (batch + 1) * 8);
            QCOMPARE(broker.activeCount(), 0);
            QCOMPARE(broker.queuedCount(), 0);
            QCOMPARE(timers(), baseline);
        }
    }

    void shutdownStopsActiveTasks()
    {
        Gate       gate;
        TestBroker broker(nullptr, [&gate](const auto &input, auto stop) {
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
