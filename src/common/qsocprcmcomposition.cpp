// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmcomposition.h"

#include <QSet>

namespace {

[[noreturn]] void reject(
    const QSocPrcmInput &input,
    const QString       &path,
    const QString       &message,
    const QString       &other = {})
{
    QList<QSocPrcmSource> source{input.source.value(path, {{}, path, 0, 0})};
    if (!other.isEmpty())
        source.append(input.source.value(other, {{}, other, 0, 0}));
    throw QSocPrcmDiagnostic{"PRCM_COMPOSITION_UNSUPPORTED", message, source};
}

QSocPrcmTarget modeTarget(
    const QSocPrcmInput           &input,
    const QSocPrcmCompositionPlan &plan,
    const QString                 &domain,
    const QString                 &mode,
    const QString                 &path)
{
    const auto source = input.domain.constFind(domain);
    if (source == input.domain.cend())
        reject(input, path, "Unknown domain: " + domain);
    const auto declared = source->mode.constFind(mode);
    if (declared == source->mode.cend())
        reject(input, path, "Unknown domain mode: " + domain + "." + mode);
    return plan.domain[domain].mode[declared->code];
}

void selectDomain(const QSocPrcmInput &input, QSocPrcmCompositionPlan &plan)
{
    if (input.domain.isEmpty())
        reject(input, "prcm.domain", "Expected at least one domain.");
    QMap<QString, QString> supplyOwner, completion;
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        const auto path = "prcm.domain." + domain.key() + ".supply";
        if (supplyOwner.contains(domain->supply))
            reject(
                input,
                path,
                "A switched supply needs one action owner.",
                supplyOwner[domain->supply]);
        supplyOwner.insert(domain->supply, path);
        const auto selected = QSocPrcmSequencePlanner::buildDomain(input, domain.key());
        if (!selected.plan)
            throw selected.diagnostic.first();
        plan.domain.insert(domain.key(), *selected.plan);
        const auto                           supply = input.supplyTable[domain->supply];
        const auto                           base   = "prcm.domain." + domain.key();
        const QList<QPair<QString, QString>> feedback{
            {supply.valid.signal, "prcm.supply." + domain->supply + ".valid.signal"},
            {domain->quiesce.completion.signal, base + ".quiesce.ack.signal"},
            {domain->isolation.completion.signal, base + ".isolation.active.signal"}};
        for (const auto &[signal, field] : feedback) {
            if (completion.contains(signal))
                reject(
                    input,
                    field,
                    "This action template needs independent completion feedback: " + signal,
                    completion[signal]);
            completion.insert(signal, field);
        }
    }
}

void selectService(const QSocPrcmInput &input, QSocPrcmCompositionPlan &plan)
{
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        const auto    path = "prcm.domain." + domain.key();
        QSet<QString> run;
        for (auto mode = domain->mode.cbegin(); mode != domain->mode.cend(); ++mode) {
            if (plan.domain[domain.key()].mode[mode->code] == QSocPrcmTarget::Run)
                run.insert(mode.key());
        }
        QMap<QString, QStringList> provider;
        for (auto use = domain->require.cbegin(); use != domain->require.cend(); ++use) {
            const auto field  = path + ".require." + use.key();
            const auto source = input.domain.constFind(use->domain);
            if (source == input.domain.cend() || !source->service.contains(use->service))
                reject(
                    input,
                    field + ".service",
                    "Unknown service: " + use->domain + "." + use->service);
            if (!source->require.isEmpty())
                reject(
                    input,
                    field + ".service",
                    "This action template supports one service layer.",
                    "prcm.domain." + use->domain + ".require");
            const auto servicePath = "prcm.domain." + use->domain + ".service." + use->service
                                     + ".mode";
            if (modeTarget(input, plan, use->domain, source->service[use->service], servicePath)
                != QSocPrcmTarget::Run)
                reject(
                    input,
                    servicePath,
                    "This service action needs a running mode with reset released.");
            const QSet<QString> requested(use->mode.cbegin(), use->mode.cend());
            if (requested.isEmpty() || requested != run)
                reject(
                    input,
                    field + ".mode",
                    "All running modes must use the same services in this action template.");
            provider[use->domain].append(field);
        }
        for (auto source = provider.cbegin(); source != provider.cend(); ++source)
            plan.service.append({domain.key(), source.key(), source.value()});
    }
}

void selectChip(const QSocPrcmInput &input, QSocPrcmCompositionPlan &plan)
{
    if (input.chipMode.isEmpty())
        return;
    const auto initial = input.chipMode.constFind(input.chipResetMode);
    if (initial == input.chipMode.cend())
        reject(input, "prcm.chip.reset_mode", "Unknown chip reset mode: " + input.chipResetMode);
    plan.resetCode = initial->code;
    QMap<quint64, QString> code;
    for (auto mode = input.chipMode.cbegin(); mode != input.chipMode.cend(); ++mode) {
        const auto path = "prcm.chip.mode." + mode.key();
        if (code.contains(mode->code))
            reject(input, path + ".code", "Chip mode code is used twice.", code[mode->code]);
        code.insert(mode->code, path + ".code");
        if (mode->domain.keys() != input.domain.keys())
            reject(input, path + ".domain", "Chip mode must name every domain exactly once.");
        QMap<QString, std::optional<QSocPrcmTarget>> policy;
        for (auto entry = mode->domain.cbegin(); entry != mode->domain.cend(); ++entry) {
            const auto field = path + ".domain." + entry.key();
            if (entry->allow.isEmpty() == entry->target.isEmpty())
                reject(input, field, "Specify either allow or target.");
            if (!entry->target.isEmpty()) {
                policy.insert(
                    entry.key(),
                    modeTarget(input, plan, entry.key(), entry->target, field + ".target"));
            } else {
                const QSet<QString> allowed(entry->allow.cbegin(), entry->allow.cend());
                const auto          names = input.domain[entry.key()].mode.keys();
                if (allowed != QSet<QString>(names.cbegin(), names.cend()))
                    reject(
                        input,
                        field + ".allow",
                        "Specify a target or allow every local mode in this action template.");
                policy.insert(entry.key(), std::nullopt);
            }
        }
        for (const auto &service : plan.service) {
            const auto provider = policy[service.provider];
            const auto consumer = policy[service.consumer];
            if (provider && *provider != QSocPrcmTarget::Run
                && (!consumer || *consumer == QSocPrcmTarget::Run))
                reject(
                    input,
                    path + ".domain." + service.consumer + (consumer ? ".target" : ".allow"),
                    "A running consumer needs its service provider to run in this chip mode.",
                    path + ".domain." + service.provider + ".target");
        }
        plan.chip.insert(mode->code, policy);
    }
}

} // namespace

QSocPrcmCompositionResult QSocPrcmComposition::build(const QSocPrcmInput &input)
{
    QSocPrcmCompositionResult result;
    try {
        QSocPrcmCompositionPlan plan;
        selectDomain(input, plan);
        selectService(input, plan);
        selectChip(input, plan);
        result.plan = std::move(plan);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    }
    return result;
}
