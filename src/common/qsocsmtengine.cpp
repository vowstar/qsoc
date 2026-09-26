// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocsmtengine.h"
#include "qsocsmtinput.h"
#include "qsocsmtservice.h"

#include <memory>
#include <unordered_set>
#include <z3++.h>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif

namespace {

void observe(const QSocSmtEngine::PhaseObserver &observer, QSocSmtEngine::Phase phase)
{
    if (observer) {
        observer(phase);
    }
}

QString numeral(const z3::expr &value)
{
    if (!value.is_numeral()) {
        throw std::runtime_error("Expected an exact numeral");
    }
    return QString::fromLatin1(Z3_get_numeral_string(value.ctx(), value));
}

QJsonObject bound(const z3::expr_vector &value)
{
    if (value.size() != 3) {
        throw std::runtime_error("Invalid objective bound");
    }
    return {
        {"infinity", numeral(value[0])},
        {"rational", numeral(value[1])},
        {"epsilon", numeral(value[2])}};
}

bool zero(const z3::expr &value)
{
    return (value == 0).simplify().is_true();
}

bool sameBounds(const z3::expr_vector &lower, const z3::expr_vector &upper)
{
    for (unsigned i = 0; i < 3; ++i) {
        if (!(lower[i] == upper[i]).simplify().is_true()) {
            return false;
        }
    }
    return true;
}

bool containsQuantifier(const z3::expr_vector &expressions)
{
    std::vector<z3::expr>        pending;
    std::unordered_set<unsigned> visited;
    for (const auto &expression : expressions) {
        pending.push_back(expression);
    }
    while (!pending.empty()) {
        const auto expression = pending.back();
        pending.pop_back();
        if (!visited.insert(expression.id()).second) {
            continue;
        }
        if (expression.is_quantifier()) {
            return true;
        }
        if (!expression.is_app()) {
            continue;
        }
        for (unsigned i = 0; i < expression.num_args(); ++i) {
            pending.push_back(expression.arg(i));
        }
    }
    return false;
}

bool canCertify(const z3::expr_vector &expressions)
{
    std::vector<z3::expr>        pending;
    std::unordered_set<unsigned> visited;
    for (const auto &expression : expressions) {
        pending.push_back(expression);
    }
    while (!pending.empty()) {
        const auto expression = pending.back();
        pending.pop_back();
        if (!visited.insert(expression.id()).second) {
            continue;
        }
        if (!expression.is_app() || expression.is_algebraic()
            || (!expression.is_bool() && !expression.is_arith() && !expression.is_bv())) {
            return false;
        }
        const auto kind = expression.decl().decl_kind();
        if (kind == Z3_OP_POWER || (kind == Z3_OP_UNINTERPRETED && expression.num_args() != 0)) {
            return false;
        }
        unsigned variables = 0;
        for (unsigned i = 0; i < expression.num_args(); ++i) {
            const auto argument = expression.arg(i);
            pending.push_back(argument);
            if (kind == Z3_OP_MUL && !argument.simplify().is_numeral()) {
                ++variables;
            }
        }
        if (variables > 1) {
            return false;
        }
        if (kind == Z3_OP_DIV || kind == Z3_OP_IDIV || kind == Z3_OP_MOD || kind == Z3_OP_REM) {
            const auto divisor = expression.arg(1).simplify();
            if (!divisor.is_numeral() || zero(divisor)) {
                return false;
            }
        }
    }
    return true;
}

bool verifyModel(z3::model &model, const z3::expr_vector &assertions, const QStringList &labels)
{
    for (const auto &assertion : assertions) {
        if (!model.eval(assertion, true).is_true()) {
            return false;
        }
    }
    for (const auto &label : labels) {
        if (!model.eval(model.ctx().bool_const(label.toUtf8().constData()), true).is_true()) {
            return false;
        }
    }
    return true;
}

QJsonObject initialResult(z3::check_result status)
{
    auto result = QSocSmtService::failure("completed", {});
    result.remove("reason");
    result.insert(
        "solver_status",
        status == z3::sat     ? "sat"
        : status == z3::unsat ? "unsat"
                              : "unknown");
    result.insert(
        "feasibility",
        status == z3::sat     ? "feasible"
        : status == z3::unsat ? "infeasible"
                              : "unknown");
    result.insert("optimality", status == z3::unsat ? "not_applicable" : "not_proven");
    result.insert("objectives", QJsonArray());
    result.insert("unsat_core", QJsonArray());
    result.insert("core_minimal", false);
    result.insert("version", QString::fromLatin1(Z3_get_full_version()));
    return result;
}

void addCore(QJsonObject &result, const z3::expr_vector &core)
{
    QJsonArray labels;
    for (const auto &label : core) {
        labels.append(QString::fromStdString(label.decl().name().str()));
    }
    result.insert("unsat_core", labels);
}

template<typename Solver>
std::unique_ptr<z3::model> readModel(
    Solver &solver, z3::check_result status, const QStringList &labels, QJsonObject &result)
{
    if (status == z3::unsat) {
        return {};
    }
    try {
        auto model = std::make_unique<z3::model>(solver.get_model());
        if (status != z3::sat && !verifyModel(*model, solver.assertions(), labels)) {
            return {};
        }
        result.insert("feasibility", "feasible");
        return model;
    } catch (const z3::exception &) {
        return {};
    }
}

QJsonObject check(
    z3::context                        &context,
    const QByteArray                   &input,
    const QSocSmtInput                 &scan,
    const QJsonObject                  &request,
    unsigned                            remaining,
    const QSocSmtEngine::PhaseObserver &observer)
{
    QDeadlineTimer deadline(remaining);
    z3::solver     solver(context);
    solver.set("ctrl_c", false);
    solver.from_string(input.constData());
    if (deadline.hasExpired()) {
        return QSocSmtService::failure("timeout", "SMT parsing exceeded the time limit");
    }
    solver.set("timeout", static_cast<unsigned>(qMax<qint64>(1, deadline.remainingTime())));
    observe(observer, QSocSmtEngine::Phase::Solve);
    const auto status = solver.check();
    auto       result = initialResult(status);
    result.insert("optimality", "not_applicable");
    if (status == z3::unknown) {
        result.insert("reason_unknown", QString::fromStdString(solver.reason_unknown()).left(4096));
    }
    if (status == z3::unsat && request.value("return_unsat_core").toBool(true)) {
        addCore(result, solver.unsat_core());
    }
    observe(observer, QSocSmtEngine::Phase::Verify);
    auto model = readModel(solver, status, scan.labels, result);
    observe(observer, QSocSmtEngine::Phase::Serialize);
    if (model && request.value("return_model").toBool(true)) {
        result.insert("model_smtlib", QString::fromStdString(model->to_string()));
    }
    return result;
}

QString classifyObjective(
    z3::context           &context,
    const z3::expr_vector &lower,
    const z3::expr_vector &upper,
    bool                   maximize,
    const z3::expr        *modelValue)
{
    const auto extreme = maximize ? upper[0] : lower[0];
    if ((maximize ? extreme > 0 : extreme < 0).simplify().is_true()) {
        return QStringLiteral("unbounded");
    }
    if (!sameBounds(lower, upper) || !zero(lower[0])) {
        return QStringLiteral("not_proven");
    }
    if (!zero(lower[2])) {
        return QStringLiteral("limit");
    }
    if (modelValue && modelValue->is_numeral()) {
        const auto actual   = context.real_val(numeral(*modelValue).toLatin1().constData());
        const auto expected = context.real_val(numeral(lower[1]).toLatin1().constData());
        if ((actual == expected).simplify().is_true()) {
            return QStringLiteral("attained");
        }
    }
    return QStringLiteral("not_proven");
}

QJsonObject optimize(
    z3::context                        &context,
    const QByteArray                   &input,
    const QSocSmtInput                 &scan,
    const QJsonObject                  &request,
    unsigned                            remaining,
    const QSocSmtEngine::PhaseObserver &observer)
{
    QDeadlineTimer deadline(remaining);
    z3::optimize   solver(context);
    z3::params     parameters(context);
    parameters.set("timeout", remaining);
    parameters.set("ctrl_c", false);
    parameters.set("priority", "lex");
    solver.set(parameters);
    solver.from_string(input.constData());
    const auto normalized = solver.objectives();
    if (normalized.size() != scan.objectives.size() || containsQuantifier(solver.assertions())
        || containsQuantifier(normalized)) {
        return QSocSmtService::failure("error", "Quantified optimization or inconsistent targets");
    }
    for (const auto &objective : normalized) {
        if (!objective.is_arith() && !objective.is_bv()) {
            return QSocSmtService::failure("error", "Only numeric objectives are supported");
        }
    }
    if (deadline.hasExpired()) {
        return QSocSmtService::failure("timeout", "SMT parsing exceeded the time limit");
    }
    parameters.set("timeout", static_cast<unsigned>(qMax<qint64>(1, deadline.remainingTime())));
    solver.set(parameters);
    observe(observer, QSocSmtEngine::Phase::Solve);
    const auto status = solver.check();
    auto       result = initialResult(status);
    if (status == z3::unsat) {
        if (request.value("return_unsat_core").toBool(true)) {
            addCore(result, solver.unsat_core());
        }
        return result;
    }
    if (status == z3::unknown) {
        result.insert(
            "reason_unknown",
            QString::fromLatin1(Z3_optimize_get_reason_unknown(context, solver)).left(4096));
    }
    observe(observer, QSocSmtEngine::Phase::Verify);
    auto       model = readModel(solver, status, scan.labels, result);
    QJsonArray objectives;
    const bool certifiable       = canCertify(solver.assertions()) && canCertify(normalized);
    bool       precedingAttained = status == z3::sat && certifiable;
    QString    optimality        = precedingAttained ? QStringLiteral("optimal")
                                                     : QStringLiteral("not_proven");
    if (!certifiable) {
        result.insert(
            "optimality_reason", "Nonlinear arithmetic or an unsupported certification theory");
    }
    for (unsigned i = 0; i < normalized.size(); ++i) {
        const auto &specification = scan.objectives[i];
        const auto  original      = specification.maximize ? -normalized[i] : normalized[i];
        QJsonObject entry{
            {"index", static_cast<int>(i)},
            {"direction", specification.maximize ? "maximize" : "minimize"},
            {"expression", specification.expression},
            {"model_value", QJsonValue::Null},
            {"lower", QJsonValue::Null},
            {"upper", QJsonValue::Null},
            {"classification", "not_classified"},
            {"bounds_proven", false}};
        std::unique_ptr<z3::expr> modelValue;
        if (model) {
            modelValue = std::make_unique<z3::expr>(model->eval(original, true));
            entry.insert("model_value", QString::fromStdString(modelValue->to_string()));
        }
        try {
            const z3::expr_vector lower(context, Z3_optimize_get_lower_as_vector(context, solver, i));
            context.check_error();
            const z3::expr_vector upper(context, Z3_optimize_get_upper_as_vector(context, solver, i));
            context.check_error();
            entry.insert("lower", bound(lower));
            entry.insert("upper", bound(upper));
            if (precedingAttained) {
                const auto state = classifyObjective(
                    context, lower, upper, specification.maximize, modelValue.get());
                entry.insert("classification", state);
                entry.insert(
                    "bounds_proven",
                    state == "attained" || state == "limit" || state == "unbounded");
                precedingAttained = state == "attained";
                if (!precedingAttained) {
                    optimality = state;
                }
            }
        } catch (const z3::exception &) {
            precedingAttained = false;
            optimality        = "not_proven";
        }
        objectives.append(entry);
    }
    result.insert("objectives", objectives);
    result.insert("optimality", optimality);
    observe(observer, QSocSmtEngine::Phase::Serialize);
    if (model && request.value("return_model").toBool(true)) {
        result.insert("model_smtlib", QString::fromStdString(model->to_string()));
    }
    return result;
}

} // namespace

bool QSocSmtEngine::applyLimits()
{
#ifdef Q_OS_LINUX
    const rlimit memory{
        QSocSmtService::memoryLimitMiB * 1024ULL * 1024ULL,
        QSocSmtService::memoryLimitMiB * 1024ULL * 1024ULL};
    const rlimit core{0, 0};
    if (setrlimit(RLIMIT_AS, &memory) != 0 || setrlimit(RLIMIT_CORE, &core) != 0) {
        return false;
    }
    Z3_global_param_set("memory_max_size", "448");
    Z3_global_param_set("unsat_core", "true");
    return true;
#else
    return false;
#endif
}

QJsonObject QSocSmtEngine::execute(const QJsonObject &request, const PhaseObserver &observer)
{
    const auto error = QSocSmtService::validateRequest(request);
    if (!error.isEmpty()) {
        return QSocSmtService::failure("error", error);
    }
    QElapsedTimer elapsed;
    elapsed.start();
    observe(observer, Phase::Parse);
    const auto input      = request.value("smtlib").toString().toUtf8();
    const bool optimizing = request.value("mode").toString() == "optimize";
    const auto scan       = QSocSmtInput::scan(input, optimizing);
    if (!scan.error.isEmpty()) {
        return QSocSmtService::failure("error", scan.error);
    }
    const auto remaining = request.value("timeout_ms").toInt(10000) - elapsed.elapsed();
    if (remaining <= 0) {
        return QSocSmtService::failure("timeout", "Input parsing exceeded the time limit");
    }
    try {
        z3::config config;
        config.set("unsat_core", true);
        z3::context context(config);
        auto        result
            = optimizing
                  ? optimize(context, input, scan, request, static_cast<unsigned>(remaining), observer)
                  : check(context, input, scan, request, static_cast<unsigned>(remaining), observer);
        if (result.value("solver_status").toString() == "unknown") {
            const auto reason = result.value("reason_unknown").toString();
            if (reason == "timeout" || reason == "canceled") {
                result.insert("execution", "timeout");
            } else if (reason == "max. memory exceeded" || reason == "out of memory") {
                result.insert("execution", "resource_limit");
            }
        }
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size()
            > QSocSmtService::outputLimit) {
            result.insert("model_smtlib", QJsonValue::Null);
            result.insert("truncated", true);
        }
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size()
            > QSocSmtService::outputLimit) {
            result.remove("objectives");
            result.remove("unsat_core");
            result.insert("truncated", true);
        }
        return result;
    } catch (const z3::exception &failure) {
        const auto message = QString::fromUtf8(failure.msg());
        return QSocSmtService::failure(
            message == "out of memory" || message == "max. memory exceeded" ? "resource_limit"
                                                                            : "error",
            message);
    } catch (const std::runtime_error &failure) {
        return QSocSmtService::failure("error", QString::fromUtf8(failure.what()));
    }
}
