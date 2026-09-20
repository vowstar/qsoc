// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequenceplan.h"

#include <QSet>

namespace {

[[noreturn]] void unsupported(
    const QSocPrcmInput &input,
    const QString       &path,
    const QString       &message,
    const QString       &other = {})
{
    QList<QSocPrcmSource> source{input.source.value(path, {{}, path, 0, 0})};
    if (!other.isEmpty())
        source.append(input.source.value(other, {{}, other, 0, 0}));
    throw QSocPrcmDiagnostic{"PRCM_SEQUENCE_UNSUPPORTED", message, source};
}

std::optional<QSocPrcmTarget> target(const QSocPrcmMode &mode)
{
    if (!mode.power && !mode.clock && mode.reset && mode.isolation)
        return QSocPrcmTarget::Off;
    if (mode.power && mode.clock && mode.reset && mode.isolation)
        return QSocPrcmTarget::Reset;
    if (mode.power && mode.clock && !mode.reset && !mode.isolation)
        return QSocPrcmTarget::Run;
    return std::nullopt;
}

void selectMode(
    const QSocPrcmInput  &input,
    const QSocPrcmDomain &domain,
    const QString        &path,
    QSocPrcmSequencePlan &plan)
{
    QMap<quint64, QString> code;
    for (auto mode = domain.mode.cbegin(); mode != domain.mode.cend(); ++mode) {
        const auto field = path + ".mode." + mode.key();
        const auto value = target(mode.value());
        if (!value)
            unsupported(input, field, "This resource state has no action template yet.");
        if (code.contains(mode->code))
            unsupported(input, field + ".code", "Mode code is used twice.", code[mode->code]);
        code.insert(mode->code, field + ".code");
        plan.mode.insert(mode->code, *value);
    }
    if (!plan.mode.values().contains(QSocPrcmTarget::Off))
        unsupported(input, path + ".mode", "Fault recovery needs a mode with power off.");
    const auto initial = domain.mode.constFind(domain.resetMode);
    if (initial == domain.mode.cend())
        unsupported(input, path + ".reset_mode", "Unknown reset mode: " + domain.resetMode);
    plan.resetCode = initial->code;
}

void checkTransition(const QSocPrcmInput &input, const QSocPrcmDomain &domain, const QString &path)
{
    QSet<QPair<QString, QString>> edge;
    for (const auto &transition : domain.transition)
        edge.insert({transition.from, transition.to});
    for (auto from = domain.mode.cbegin(); from != domain.mode.cend(); ++from) {
        for (auto to = domain.mode.cbegin(); to != domain.mode.cend(); ++to) {
            if (from != to && !edge.contains({from.key(), to.key()})) {
                unsupported(
                    input,
                    path + ".transition",
                    QString("Busy target changes need a transition from %1 to %2 in this template.")
                        .arg(from.key(), to.key()));
            }
        }
    }
}

} // namespace

QSocPrcmSequencePlanResult QSocPrcmSequencePlanner::build(const QSocPrcmInput &input)
{
    QSocPrcmSequencePlanResult result;
    try {
        if (input.domain.size() != 1)
            unsupported(input, "prcm.domain", "The action template supports one domain at present.");
        if (!input.chipMode.isEmpty())
            unsupported(input, "prcm.chip", "Chip policy needs a shared-resource action template.");
        const auto domain = input.domain.cbegin();
        const auto path   = "prcm.domain." + domain.key();
        if (!domain->service.isEmpty() || !domain->require.isEmpty())
            unsupported(input, path, "Service ownership needs a domain handshake action template.");
        return buildDomain(input, domain.key());
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    }
    return result;
}

QSocPrcmSequencePlanResult QSocPrcmSequencePlanner::buildDomain(
    const QSocPrcmInput &input, const QString &name)
{
    QSocPrcmSequencePlanResult result;
    try {
        const auto path   = "prcm.domain." + name;
        const auto domain = input.domain.constFind(name);
        if (domain == input.domain.cend())
            unsupported(input, path, "Unknown domain: " + name);
        const auto supply = input.supplyTable.constFind(domain->supply);
        if (supply == input.supplyTable.cend() || supply->alwaysOn)
            unsupported(input, path + ".supply", "This template needs a switched supply.");
        QSocPrcmSequencePlan plan;
        plan.domain = domain.key();
        selectMode(input, domain.value(), path, plan);
        checkTransition(input, domain.value(), path);
        result.plan = std::move(plan);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    }
    return result;
}
