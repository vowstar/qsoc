// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsolver.h"
#include "qsoc_test.h"

#include <QtTest>

#include <z3++.h>

#include <chrono>
#include <thread>

namespace {

using Status = QSocPrcmCheckStatus;

QSocPrcmQuery powerModel()
{
    return {
        {"power", "clock", "reset"},
        {{"clock.supply", {{{"clock", false}, {"power", true}}}},
         {"reset.supply", {{{"power", true}, {"reset", true}}}}}};
}

void selectState(QSocPrcmQuery &query, unsigned state)
{
    query.requirement.append({
        "mode",
        {{{"power", (state & 4U) != 0}},
         {{"clock", (state & 2U) != 0}},
         {{"reset", (state & 1U) != 0}}},
    });
}

bool evaluate(const QSocPrcmQuery &query, const QMap<QString, bool> &value)
{
    for (const auto &requirement : query.requirement) {
        for (const auto &clause : requirement.clause) {
            bool satisfied = false;
            for (const auto &literal : clause) {
                satisfied |= value.contains(literal.symbol)
                             && value.value(literal.symbol) == literal.value;
            }
            if (!satisfied) {
                return false;
            }
        }
    }
    return true;
}

QSocPrcmQuery pigeonhole()
{
    QSocPrcmQuery       query;
    QSocPrcmRequirement requirement{QStringLiteral("capacity"), {}};
    const auto          name = [](unsigned item, unsigned slot) {
        return QStringLiteral("item%1.slot%2").arg(item).arg(slot);
    };
    for (unsigned item = 0; item < 24; ++item) {
        QList<QSocPrcmLiteral> placement;
        for (unsigned slot = 0; slot < 23; ++slot) {
            query.symbol.append(name(item, slot));
            placement.append({name(item, slot), true});
            for (unsigned previous = 0; previous < item; ++previous) {
                requirement.clause.append(
                    {{name(item, slot), false}, {name(previous, slot), false}});
            }
        }
        requirement.clause.append(placement);
    }
    query.requirement.append(requirement);
    return query;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void modeTruthTable()
    {
        for (unsigned state = 0; state < 8; ++state) {
            auto query = powerModel();
            selectState(query, state);
            const auto result = QSocPrcmSolver::check(query);
            const bool legal  = state == 1 || state >= 4;
            QCOMPARE(result.status, legal ? Status::Sat : Status::Unsat);
            QVERIFY(result.version.startsWith(QStringLiteral("4.16.0")));
            if (legal) {
                QVERIFY(evaluate(query, result.value));
            } else {
                QVERIFY(result.value.isEmpty());
                QVERIFY(result.conflict.contains(QStringLiteral("mode")));
            }
        }
    }

    void conflictSourceAndReplay()
    {
        auto query = powerModel();
        selectState(query, 3);
        query.requirement.append({"unrelated", {{{"reset", true}, {"reset", false}}}});
        const auto result = QSocPrcmSolver::check(query);
        QCOMPARE(result.status, Status::Unsat);
        QVERIFY(result.conflict.contains(QStringLiteral("clock.supply")));
        QVERIFY(result.conflict.contains(QStringLiteral("mode")));
        QVERIFY(!result.conflict.contains(QStringLiteral("unrelated")));

        z3::context context;
        z3::solver  replay(context);
        replay.from_string(result.smt.toUtf8().constData());
        QCOMPARE(replay.check(), z3::unsat);

        QSocPrcmQuery core{query.symbol, {}};
        for (const auto &requirement : query.requirement) {
            if (result.conflict.contains(requirement.source)) {
                core.requirement.append(requirement);
            }
        }
        QCOMPARE(QSocPrcmSolver::check(core).status, Status::Unsat);
    }

    void implicationCycle()
    {
        QSocPrcmQuery query{
            {"a", "b"},
            {{"a.require", {{{"a", false}, {"b", true}}}},
             {"b.require", {{{"b", false}, {"a", true}}}}}};
        for (bool active : {false, true}) {
            auto selected = query;
            selected.requirement.append({"mode", {{{"a", active}}, {{"b", active}}}});
            const auto result = QSocPrcmSolver::check(selected);
            QCOMPARE(result.status, Status::Sat);
            QVERIFY(evaluate(selected, result.value));
        }
    }

    void missingConstraintWitness()
    {
        auto baseline = powerModel();
        selectState(baseline, 3);
        QCOMPARE(QSocPrcmSolver::check(baseline).status, Status::Unsat);
        auto fault = baseline;
        fault.requirement.removeFirst();
        const auto result = QSocPrcmSolver::check(fault);
        QCOMPARE(result.status, Status::Sat);
        QVERIFY(!result.value.value(QStringLiteral("power")));
        QVERIFY(result.value.value(QStringLiteral("clock")));
        QCOMPARE(QSocPrcmSolver::check(baseline).status, Status::Unsat);
    }

    void emptyFormulaAndNames()
    {
        const QSocPrcmQuery empty;
        const QSocPrcmQuery contradiction{{}, {{"false", {{}}}}};
        const QSocPrcmQuery tautology{{}, {{"true", {}}}};
        QCOMPARE(QSocPrcmSolver::check(empty).status, Status::Sat);
        QCOMPARE(QSocPrcmSolver::check(contradiction).status, Status::Unsat);
        QCOMPARE(QSocPrcmSolver::check(tautology).status, Status::Sat);
        QSocPrcmQuery query{
            {"a0", "v0", "x|)\n(assert false)"},
            {{"a0", {{{"a0", true}}}},
             {"v0", {{{"v0", false}}}},
             {"x|)\n(assert false)", {{{"x|)\n(assert false)", true}}}}}};
        const auto result = QSocPrcmSolver::check(query);
        QCOMPARE(result.status, Status::Sat);
        QVERIFY(evaluate(query, result.value));
        z3::context context;
        z3::solver  replay(context);
        replay.from_string(result.smt.toUtf8().constData());
        QCOMPARE(replay.check(), z3::sat);
    }

    void invalidQuery()
    {
        const QList<QSocPrcmQuery> invalid{
            {{"x", "x"}, {}},
            {{""}, {}},
            {{}, {{"a", {}}, {"a", {}}}},
            {{}, {{"", {}}}},
            {{"x"}, {{"a", {{{"y", true}}}}}},
        };
        for (const auto &query : invalid) {
            const auto result = QSocPrcmSolver::check(query);
            QCOMPARE(result.status, Status::Error);
            QVERIFY(!result.reason.isEmpty());
            QVERIFY(result.smt.isEmpty());
            QVERIFY(result.value.isEmpty());
        }
        QCOMPARE(QSocPrcmSolver::check({}, {0, 0}).status, Status::Error);
    }

    void budgetAndCancellation()
    {
        const auto limited = QSocPrcmSolver::check(powerModel(), {10000, 1});
        QCOMPARE(limited.status, Status::Unknown);
        QVERIFY(!limited.reason.isEmpty());
        QVERIFY(limited.value.isEmpty());
        QVERIFY(limited.conflict.isEmpty());

        std::stop_source source;
        source.request_stop();
        const auto cancelled = QSocPrcmSolver::check(powerModel(), {}, source.get_token());
        QCOMPARE(cancelled.status, Status::Cancelled);
        QVERIFY(cancelled.value.isEmpty());
        QCOMPARE(QSocPrcmSolver::check(powerModel()).status, Status::Sat);
    }

    void timeoutAndRunningCancellation()
    {
        const auto query = pigeonhole();
        const auto timed = QSocPrcmSolver::check(query, {1, 0});
        QVERIFY2(timed.status == Status::Timeout, qPrintable(timed.reason));
        QVERIFY(timed.value.isEmpty());
        QVERIFY(timed.conflict.isEmpty());

        std::stop_source source;
        std::jthread     cancel([&source] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            source.request_stop();
        });
        const auto       interrupted = QSocPrcmSolver::check(query, {10000, 0}, source.get_token());
        QCOMPARE(interrupted.status, Status::Cancelled);
        QVERIFY(interrupted.value.isEmpty());
        QCOMPARE(QSocPrcmSolver::check(powerModel()).status, Status::Sat);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmsolver.moc"
