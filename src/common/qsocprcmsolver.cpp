// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsolver.h"

#include <QElapsedTimer>
#include <QSet>

#include <z3++.h>

namespace {

QString validateQuery(const QSocPrcmQuery &query, const QSocPrcmCheckBudget &budget)
{
    if (budget.timeoutMs == 0) {
        return QStringLiteral("The check timeout must be greater than zero.");
    }
    QSet<QString> symbol;
    for (const auto &name : query.symbol) {
        if (name.isEmpty() || symbol.contains(name)) {
            return QStringLiteral("Empty or duplicate symbol: %1").arg(name);
        }
        symbol.insert(name);
    }
    QSet<QString> source;
    for (const auto &requirement : query.requirement) {
        if (requirement.source.isEmpty() || source.contains(requirement.source)) {
            return QStringLiteral("Empty or duplicate requirement source: %1")
                .arg(requirement.source);
        }
        source.insert(requirement.source);
        for (const auto &clause : requirement.clause) {
            for (const auto &literal : clause) {
                if (!symbol.contains(literal.symbol)) {
                    return QStringLiteral("Unknown symbol %1 in %2.")
                        .arg(literal.symbol, requirement.source);
                }
            }
        }
    }
    return {};
}

QSocPrcmCheckStatus unknownStatus(const QString &reason, bool cancelled, bool timeoutOnly)
{
    if (cancelled) {
        return QSocPrcmCheckStatus::Cancelled;
    }
    /* The search timeout can also return "canceled". */
    if (reason == QStringLiteral("timeout")
        || (reason == QStringLiteral("canceled") && timeoutOnly)) {
        return QSocPrcmCheckStatus::Timeout;
    }
    return QSocPrcmCheckStatus::Unknown;
}

} // namespace

QSocPrcmCheckResult QSocPrcmSolver::check(
    const QSocPrcmQuery &query, const QSocPrcmCheckBudget &budget, std::stop_token stop)
{
    QSocPrcmCheckResult result;
    result.version = QString::fromLatin1(Z3_get_full_version());
    result.reason  = validateQuery(query, budget);
    if (!result.reason.isEmpty()) {
        return result;
    }
    if (stop.stop_requested()) {
        result.status = QSocPrcmCheckStatus::Cancelled;
        result.reason = QStringLiteral("Check cancelled.");
        return result;
    }

    try {
        z3::context context;
        z3::solver  solver(context);
        solver.set("timeout", budget.timeoutMs);
        solver.set("rlimit", budget.resourceLimit);
        solver.set("ctrl_c", false);
        QMap<QString, unsigned> index;
        z3::expr_vector         symbol(context);
        for (const auto &name : query.symbol) {
            const unsigned position = symbol.size();
            index.insert(name, position);
            const QByteArray internalName = "v" + QByteArray::number(position);
            symbol.push_back(context.bool_const(internalName.constData()));
        }

        z3::expr_vector assumption(context);
        for (const auto &requirement : query.requirement) {
            z3::expr predicate = context.bool_val(true);
            for (const auto &clause : requirement.clause) {
                z3::expr disjunction = context.bool_val(false);
                for (const auto &literal : clause) {
                    const z3::expr term = symbol[index.value(literal.symbol)];
                    disjunction         = disjunction || (literal.value ? term : !term);
                }
                predicate = predicate && disjunction;
            }
            const QByteArray label = "a" + QByteArray::number(assumption.size());
            assumption.push_back(context.bool_const(label.constData()));
            solver.add(z3::implies(assumption.back(), predicate));
        }

        /* Include every activation in the exported query so replay checks the same formula. */
        z3::solver replay(context);
        replay.add(solver.assertions());
        replay.add(assumption);
        result.smt = QString::fromStdString(replay.to_smt2());

        /* The callback must finish before the context leaves scope. */
        std::stop_callback cancellation(stop, [&context] { context.interrupt(); });
        if (stop.stop_requested()) {
            result.status = QSocPrcmCheckStatus::Cancelled;
            result.reason = QStringLiteral("Check cancelled.");
            return result;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        const z3::check_result status = solver.check(assumption);
        if (stop.stop_requested()) {
            result.status = QSocPrcmCheckStatus::Cancelled;
            result.reason = QStringLiteral("Check cancelled.");
        } else if (status == z3::sat) {
            result.status         = QSocPrcmCheckStatus::Sat;
            const z3::model model = solver.get_model();
            for (auto it = index.cbegin(); it != index.cend(); ++it) {
                result.value.insert(it.key(), model.eval(symbol[it.value()], true).is_true());
            }
        } else if (status == z3::unsat) {
            result.status              = QSocPrcmCheckStatus::Unsat;
            const z3::expr_vector core = solver.unsat_core();
            for (unsigned i = 0; i < assumption.size(); ++i) {
                for (const auto &entry : core) {
                    if (z3::eq(entry, assumption[i])) {
                        result.conflict.append(query.requirement.at(i).source);
                    }
                }
            }
        } else {
            result.reason = QString::fromStdString(solver.reason_unknown());
            result.status = unknownStatus(
                result.reason,
                stop.stop_requested(),
                budget.resourceLimit == 0 && elapsed.elapsed() >= budget.timeoutMs);
        }
    } catch (const z3::exception &error) {
        result.status = stop.stop_requested() ? QSocPrcmCheckStatus::Cancelled
                                              : QSocPrcmCheckStatus::Error;
        result.reason = QString::fromUtf8(error.msg());
    }
    return result;
}
