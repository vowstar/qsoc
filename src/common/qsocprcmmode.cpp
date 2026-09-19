// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmmode.h"

#include <QSet>

namespace {

QString domainPath(const QString &name)
{
    return "prcm.domain." + name;
}
QString modePath(const QString &domain, const QString &mode)
{
    return domainPath(domain) + ".mode." + mode;
}

QSocPrcmSource location(const QSocPrcmInput &input, const QString &path)
{
    return input.source.value(path, {{}, path, 0, 0});
}

[[noreturn]] void invalid(
    const QSocPrcmInput &input,
    const QString       &path,
    const QString       &message,
    const QString       &other = {})
{
    QList<QSocPrcmSource> source{location(input, path)};
    if (!other.isEmpty()) {
        source.append(location(input, other));
    }
    throw QSocPrcmDiagnostic{"PRCM_MODE_REFERENCE", message, source};
}

void requireMode(
    const QSocPrcmInput  &input,
    const QSocPrcmDomain &domain,
    const QString        &mode,
    const QString        &path)
{
    if (!domain.mode.contains(mode)) {
        invalid(input, path, "Unknown local mode: " + mode);
    }
}

void validateDomain(const QSocPrcmInput &input, const QString &name, const QSocPrcmDomain &domain)
{
    const auto path = domainPath(name);
    if (!input.supplyTable.contains(domain.supply)) {
        invalid(input, path + ".supply", "Unknown supply: " + domain.supply);
    }
    requireMode(input, domain, domain.resetMode, path + ".reset_mode");
    QMap<quint64, QString> code;
    for (auto mode = domain.mode.cbegin(); mode != domain.mode.cend(); ++mode) {
        const auto field = modePath(name, mode.key()) + ".code";
        if (code.contains(mode->code)) {
            invalid(input, field, "Mode code is used twice.", code.value(mode->code));
        }
        code.insert(mode->code, field);
    }
    QSet<QPair<QString, QString>> edge;
    for (qsizetype i = 0; i < domain.transition.size(); ++i) {
        const auto &transition = domain.transition[i];
        const auto  field      = path + QString(".transition[%1]").arg(i);
        requireMode(input, domain, transition.from, field + ".from");
        requireMode(input, domain, transition.to, field + ".to");
        const QPair<QString, QString> pair{transition.from, transition.to};
        if (edge.contains(pair)) {
            invalid(input, field, "Transition is declared twice.");
        }
        edge.insert(pair);
    }
    for (auto service = domain.service.cbegin(); service != domain.service.cend(); ++service) {
        requireMode(input, domain, service.value(), path + ".service." + service.key() + ".mode");
    }
    for (auto use = domain.require.cbegin(); use != domain.require.cend(); ++use) {
        const auto field    = path + ".require." + use.key();
        const auto provider = input.domain.constFind(use->domain);
        if (provider == input.domain.cend() || !provider->service.contains(use->service)) {
            invalid(input, field + ".service", "Unknown service: " + use->domain + "." + use->service);
        }
        if (use->mode.isEmpty()) {
            invalid(input, field + ".mode", "Service use needs at least one local mode.");
        }
        for (const auto &mode : use->mode) {
            requireMode(input, domain, mode, field + ".mode");
        }
    }
}

void validate(const QSocPrcmInput &input)
{
    if (input.domain.isEmpty()) {
        invalid(input, "prcm.domain", "Expected at least one domain.");
    }
    const auto controllerSupply = input.supplyTable.constFind(input.supply);
    if (controllerSupply == input.supplyTable.cend() || !controllerSupply->alwaysOn) {
        invalid(input, "prcm.controller.supply", "The controller needs a declared always-on supply.");
    }
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        validateDomain(input, domain.key(), domain.value());
    }
    if (!input.chipMode.isEmpty() && !input.chipMode.contains(input.chipResetMode)) {
        invalid(input, "prcm.chip.reset_mode", "Unknown chip mode: " + input.chipResetMode);
    }
    QMap<quint64, QString> code;
    for (auto mode = input.chipMode.cbegin(); mode != input.chipMode.cend(); ++mode) {
        const auto path = "prcm.chip.mode." + mode.key();
        if (code.contains(mode->code)) {
            invalid(input, path + ".code", "Chip mode code is used twice.", code.value(mode->code));
        }
        code.insert(mode->code, path + ".code");
        if (mode->domain.keys() != input.domain.keys()) {
            invalid(input, path + ".domain", "Chip mode must name every domain exactly once.");
        }
        for (auto policy = mode->domain.cbegin(); policy != mode->domain.cend(); ++policy) {
            const auto field = path + ".domain." + policy.key();
            if (policy->allow.isEmpty() == policy->target.isEmpty()) {
                invalid(input, field, "Specify either allow or target.");
            }
            const auto &domain = input.domain[policy.key()];
            if (!policy->target.isEmpty()) {
                requireMode(input, domain, policy->target, field + ".target");
            }
            for (const auto &allowed : policy->allow) {
                requireMode(input, domain, allowed, field + ".allow");
            }
        }
    }
}

struct Formula
{
    QSocPrcmQuery          query;
    QMap<QString, QString> origin;

    void add(const QString &id, const QString &path, const QList<QList<QSocPrcmLiteral>> &clause)
    {
        query.requirement.append({id, clause});
        origin.insert(id, path);
    }
};

void encodeDomain(Formula &formula, const QString &name, const QSocPrcmDomain &domain)
{
    const auto path  = domainPath(name);
    const auto power = "prcm.supply." + domain.supply;
    for (const auto &field : {"clock", "reset", "isolation"}) {
        formula.query.symbol.append(path + "." + field);
    }
    QList<QSocPrcmLiteral> choice;
    for (auto mode = domain.mode.cbegin(); mode != domain.mode.cend(); ++mode) {
        const auto selected = modePath(name, mode.key());
        formula.query.symbol.append(selected);
        choice.append({selected, true});
        const QMap<QString, QPair<QString, bool>> state{
            {"power", {power, mode->power}},
            {"clock", {path + ".clock", mode->clock}},
            {"reset", {path + ".reset", mode->reset}},
            {"isolation", {path + ".isolation", mode->isolation}}};
        for (auto field = state.cbegin(); field != state.cend(); ++field) {
            const auto source = selected + "." + field.key();
            formula.add(source, source, {{{selected, false}, {field->first, field->second}}});
        }
    }
    QList<QList<QSocPrcmLiteral>> unique{choice};
    for (qsizetype i = 0; i < choice.size(); ++i) {
        for (qsizetype j = 0; j < i; ++j) {
            unique.append({{choice[i].symbol, false}, {choice[j].symbol, false}});
        }
    }
    formula.add(path + ".one_mode", path + ".mode", unique);
    formula
        .add(path + ".clock_supply", path + ".supply", {{{path + ".clock", false}, {power, true}}});
    formula.add(path + ".reset_supply", path + ".supply", {{{power, true}, {path + ".reset", true}}});
    formula.add(
        path + ".isolation_supply",
        path + ".supply",
        {{{power, true}, {path + ".isolation", true}}});
}

Formula encode(const QSocPrcmInput &input)
{
    Formula formula;
    for (auto supply = input.supplyTable.cbegin(); supply != input.supplyTable.cend(); ++supply) {
        const auto path = "prcm.supply." + supply.key();
        formula.query.symbol.append(path);
        if (supply->alwaysOn) {
            formula.add(path + ".always_on", path + ".always_on", {{{path, true}}});
        }
    }
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        encodeDomain(formula, domain.key(), domain.value());
        for (auto use = domain->require.cbegin(); use != domain->require.cend(); ++use) {
            const auto &provider = input.domain[use->domain];
            const auto  target   = modePath(use->domain, provider.service[use->service]);
            const auto  path     = domainPath(domain.key()) + ".require." + use.key();
            QList<QList<QSocPrcmLiteral>> clause;
            for (const auto &mode : use->mode) {
                clause.append({{modePath(domain.key(), mode), false}, {target, true}});
            }
            formula.add(path, path + ".service", clause);
        }
    }
    return formula;
}

void runCase(
    const QSocPrcmInput       &input,
    Formula                    formula,
    const QString             &name,
    const QSocPrcmCheckBudget &budget,
    std::stop_token            stop,
    QSocPrcmModeResult        &report)
{
    auto result = QSocPrcmSolver::check(formula.query, budget, stop);
    if (result.status == QSocPrcmCheckStatus::Unsat) {
        QSocPrcmDiagnostic diagnostic{"PRCM_MODE_CONFLICT", "No legal stable state for " + name, {}};
        QSet<QString> seen;
        for (const auto &id : result.conflict) {
            const auto path = formula.origin.value(id);
            if (!seen.contains(path)) {
                diagnostic.source.append(location(input, path));
                seen.insert(path);
            }
        }
        report.diagnostic.append(diagnostic);
    }
    report.check.append({name, std::move(result)});
}

} // namespace

QSocPrcmModeResult QSocPrcmModeCheck::check(
    const QSocPrcmInput &input, const QSocPrcmCheckBudget &budget, std::stop_token stop)
{
    QSocPrcmModeResult report;
    try {
        validate(input);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        report.diagnostic.append(diagnostic);
        return report;
    }
    const auto base = encode(input);
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        for (auto mode = domain->mode.cbegin(); mode != domain->mode.cend(); ++mode) {
            auto       formula = base;
            const auto path    = modePath(domain.key(), mode.key());
            formula.add("query.select", path, {{{path, true}}});
            runCase(input, formula, path, budget, stop, report);
            if (stop.stop_requested()) {
                return report;
            }
        }
    }
    for (auto chip = input.chipMode.cbegin(); chip != input.chipMode.cend(); ++chip) {
        auto       formula = base;
        const auto path    = "prcm.chip.mode." + chip.key();
        for (auto policy = chip->domain.cbegin(); policy != chip->domain.cend(); ++policy) {
            QList<QSocPrcmLiteral> choice;
            const auto             allowed = policy->target.isEmpty() ? policy->allow
                                                                      : QStringList{policy->target};
            for (const auto &mode : allowed) {
                choice.append({modePath(policy.key(), mode), true});
            }
            const auto source = path + ".domain." + policy.key();
            formula.add(source, source, {choice});
        }
        runCase(input, formula, path, budget, stop, report);
        if (stop.stop_requested()) {
            return report;
        }
    }
    return report;
}
