// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequencecheck.h"
#include "common/qsocprcmgraph_p.h"

#include <QSet>
#include <QVector>

namespace {

using Target  = QSocPrcmTarget;
using Literal = QSocPrcmLiteral;

QString targetName(Target target)
{
    switch (target) {
    case Target::Off:
        return "off";
    case Target::Reset:
        return "reset";
    case Target::Run:
        return "run";
    }
    return {};
}

using Node = QSocPrcmSequenceFrame;

int key(const Node &node)
{
    const auto &value = node.observation;
    const int state = static_cast<int>(node.state.phase) * 3 + static_cast<int>(node.state.target);
    return state * 16 + int(value.power) * 8 + int(value.reset) * 4 + int(value.isolation) * 2
           + int(value.idle);
}

Node response(const QSocPrcmSequenceState &state, const QSocPrcmObservation &old, unsigned follow)
{
    Node       node{state, old};
    const auto control = QSocPrcmSequence::control(state);
    if ((follow & 1U) != 0)
        node.observation.power = control.power;
    if ((follow & 2U) != 0)
        node.observation.reset = control.reset;
    if ((follow & 4U) != 0)
        node.observation.isolation = control.isolation;
    if ((follow & 8U) != 0)
        node.observation.idle = control.quiesce;
    return node;
}

QList<std::optional<Target>> requestFor(const QSocPrcmSequencePlan &plan, QString &error)
{
    QList<std::optional<Target>> request{std::nullopt};
    for (auto value : plan.mode) {
        if (targetName(value).isEmpty()) {
            error = "Unknown action target.";
            return {};
        }
        if (!request.contains(value))
            request.append(value);
    }
    if (!request.contains(Target::Off)) {
        error = "The action plan needs a power-off target.";
        return {};
    }
    return request;
}

QList<Node> reachable(const QList<std::optional<Target>> &request, std::stop_token stop)
{
    const Node  initial;
    QList<Node> node{initial};
    QSet<int>   seen{key(initial)};
    for (qsizetype index = 0; index < node.size() && !stop.stop_requested(); ++index) {
        const auto current = node[index];
        for (const auto &target : request) {
            const auto next = QSocPrcmSequence::step(current.state, target, current.observation);
            for (unsigned follow = 0; follow < 16; ++follow) {
                const auto successor = response(next.state, current.observation, follow);
                const auto id        = key(successor);
                if (!seen.contains(id)) {
                    seen.insert(id);
                    node.append(successor);
                }
            }
        }
    }
    return node;
}

using QSocPrcmGraph::component;
using QSocPrcmGraph::Edge;
using QSocPrcmGraph::fairLoop;

QList<qsizetype> fairWitness(const QList<Node> &node, const QList<qsizetype> &group)
{
    QList<qsizetype> witness{-1, -1, -1, -1};
    for (auto index : group) {
        const auto  control = QSocPrcmSequence::control(node[index].state);
        const auto &value   = node[index].observation;
        const bool  match[]{
            control.power == value.power,
            control.reset == value.reset,
            control.isolation == value.isolation,
            control.quiesce == value.idle};
        for (int i = 0; i < 4; ++i) {
            if (match[i])
                witness[i] = index;
        }
    }
    return witness.contains(-1) ? QList<qsizetype>{} : witness;
}

class Formula
{
public:
    QSocPrcmQuery       query;
    QSocPrcmRequirement model{"sequence.model", {}};
    QSocPrcmRequirement property{"sequence.safety", {}};

    Literal symbol(const QString &name, bool value = true)
    {
        if (!declared.contains(name)) {
            declared.insert(name);
            query.symbol.append(name);
        }
        return {name, value};
    }

    void imply(const QList<Literal> &condition, const QList<Literal> &value)
    {
        QList<Literal> prefix;
        for (const auto &item : condition)
            prefix.append({item.symbol, !item.value});
        for (const auto &item : value) {
            auto clause = prefix;
            clause.append(item);
            model.clause.append(clause);
        }
    }

    void violation(const QString &name, const QList<Literal> &condition)
    {
        const auto     flag = symbol("violation." + name);
        QList<Literal> reverse{flag};
        for (const auto &item : condition) {
            property.clause.append({{flag.symbol, false}, item});
            reverse.append({item.symbol, !item.value});
        }
        property.clause.append(reverse);
        failure.append(flag);
    }

    QList<Literal> control(const QString &prefix, const QSocPrcmControl &value)
    {
        return {
            symbol(prefix + ".power", value.power),
            symbol(prefix + ".clock", value.clock),
            symbol(prefix + ".reset", value.reset),
            symbol(prefix + ".isolation", value.isolation),
            symbol(prefix + ".quiesce", value.quiesce)};
    }

    QList<Literal> target(const QString &prefix, Target value)
    {
        return {
            symbol(prefix + ".off", value == Target::Off),
            symbol(prefix + ".reset", value == Target::Reset),
            symbol(prefix + ".run", value == Target::Run)};
    }

    void finish()
    {
        property.clause.append(failure);
        query.requirement = {model, property};
    }

private:
    QSet<QString>  declared;
    QList<Literal> failure;
};

void encode(
    Formula                            &formula,
    const QList<Node>                  &node,
    const QList<std::optional<Target>> &request,
    std::stop_token                     stop)
{
    QList<Literal> choice;
    for (qsizetype i = 0; i < request.size(); ++i) {
        const QString name = request[i] ? targetName(*request[i]) : "invalid";
        choice.append(formula.symbol("request." + name));
        for (qsizetype j = 0; j < i; ++j)
            formula.model.clause.append({{choice[i].symbol, false}, {choice[j].symbol, false}});
    }
    formula.model.clause.append(choice);
    for (auto target : {Target::Off, Target::Reset, Target::Run}) {
        if (!request.contains(target))
            formula.model.clause.append({formula.symbol("request." + targetName(target), false)});
    }
    QList<Literal> selected;
    for (qsizetype i = 0; i < node.size() && !stop.stop_requested(); ++i) {
        const auto select = formula.symbol(QString("state.%1").arg(i));
        selected.append(select);
        const auto &current = node[i];
        const auto &value   = current.observation;
        auto        context = formula.control("before", QSocPrcmSequence::control(current.state));
        context.append(formula.target("target", current.state.target));
        context.append(
            {formula.symbol("feedback.power", value.power),
             formula.symbol("feedback.reset", value.reset),
             formula.symbol("feedback.isolation", value.isolation),
             formula.symbol("feedback.idle", value.idle)});
        formula.imply({select}, context);
        for (qsizetype j = 0; j < request.size(); ++j) {
            const auto next   = QSocPrcmSequence::step(current.state, request[j], value);
            auto       output = formula.control("after", QSocPrcmSequence::control(next.state));
            output.append(formula.target("next_target", next.state.target));
            output.append(formula.symbol("power_lost", next.powerLost));
            output.append(
                formula.symbol("done", QSocPrcmSequence::complete(next.state, request[j], value)));
            output.append(
                {formula.symbol("stable.off", next.state.phase == QSocPrcmPhase::Off),
                 formula.symbol("stable.reset", next.state.phase == QSocPrcmPhase::Reset),
                 formula.symbol("stable.run", next.state.phase == QSocPrcmPhase::Run)});
            formula.imply({select, choice[j]}, output);
        }
    }
    formula.model.clause.append(selected);
}

void safety(Formula &formula)
{
    const auto bit = [&formula](const QString &name, bool value = true) {
        return formula.symbol(name, value);
    };
    for (const auto &field : {"power", "isolation"}) {
        const QString name(field);
        formula.violation(
            name + "_reverse_low",
            {bit("before." + name, false), bit("after." + name), bit("feedback." + name)});
        formula.violation(
            name + "_reverse_high",
            {bit("before." + name), bit("after." + name, false), bit("feedback." + name, false)});
    }
    formula.violation(
        "quiesce_reverse_low",
        {bit("before.quiesce", false), bit("after.quiesce"), bit("feedback.idle")});
    formula.violation(
        "quiesce_reverse_high",
        {bit("before.quiesce"), bit("after.quiesce", false), bit("feedback.idle", false)});
    const QList<Literal> powerOff{bit("before.power"), bit("after.power", false)};
    const QList<Literal> clockOff{bit("before.clock"), bit("after.clock", false)};
    const QList<Literal> resetOff{bit("before.reset"), bit("after.reset", false)};
    const QList<Literal> isolationOff{bit("before.isolation"), bit("after.isolation", false)};
    for (const auto &field : {"reset", "isolation"}) {
        const QString name(field);
        formula.violation(
            "power_off_" + name, powerOff + QList<Literal>{bit("feedback." + name, false)});
        formula.violation(
            "clock_off_" + name, clockOff + QList<Literal>{bit("feedback." + name, false)});
    }
    formula.violation("power_off_clock", powerOff + QList<Literal>{bit("before.clock")});
    formula.violation("reset_release_power", resetOff + QList<Literal>{bit("feedback.power", false)});
    formula.violation("reset_release_clock", resetOff + QList<Literal>{bit("before.clock", false)});
    formula.violation(
        "reset_release_isolation", resetOff + QList<Literal>{bit("feedback.isolation", false)});
    formula.violation(
        "isolation_release_power", isolationOff + QList<Literal>{bit("feedback.power", false)});
    formula.violation(
        "isolation_release_clock", isolationOff + QList<Literal>{bit("before.clock", false)});
    formula
        .violation("isolation_release_reset", isolationOff + QList<Literal>{bit("feedback.reset")});
    for (const auto &condition : {bit("before.quiesce", false), bit("feedback.idle", false)})
        formula.violation(
            "isolation_before_" + condition.symbol,
            {bit("before.isolation", false), bit("after.isolation"), condition});
    for (const auto &condition :
         {bit("feedback.power", false), bit("feedback.reset"), bit("feedback.isolation")})
        formula.violation("admission_" + condition.symbol, {bit("after.quiesce", false), condition});
    formula.violation("normal_power_loss", {bit("power_lost")});
    formula.violation("invalid_done", {bit("request.invalid"), bit("done")});
    for (const auto &name : {"off", "reset", "run"}) {
        const QString target(name);
        formula.violation(
            "invalid_target_" + target,
            {bit("request.invalid"), bit("target." + target), bit("next_target." + target, false)});
        const QList<Literal> done{bit("done"), bit("request." + target)};
        formula
            .violation("done_phase_" + target, done + QList<Literal>{bit("stable." + target, false)});
        formula.violation(
            "done_target_" + target, done + QList<Literal>{bit("next_target." + target, false)});
        formula.violation(
            "done_power_" + target, done + QList<Literal>{bit("feedback.power", target == "off")});
        formula.violation(
            "done_reset_" + target, done + QList<Literal>{bit("feedback.reset", target == "run")});
        formula.violation(
            "done_isolation_" + target,
            done + QList<Literal>{bit("feedback.isolation", target == "run")});
    }
}

} // namespace

QSocPrcmCheckResult QSocPrcmSequenceCheck::safety(
    const QSocPrcmSequencePlan &plan, const QSocPrcmCheckBudget &budget, std::stop_token stop)
{
    QString    error;
    const auto request = requestFor(plan, error);
    if (!error.isEmpty()) {
        QSocPrcmCheckResult result;
        result.reason = error;
        return result;
    }
    Formula formula;
    encode(formula, reachable(request, stop), request, stop);
    ::safety(formula);
    formula.finish();
    auto feasible = formula.query;
    feasible.requirement.removeLast();
    auto existence = QSocPrcmSolver::check(feasible, budget, stop);
    if (existence.status == QSocPrcmCheckStatus::Unsat) {
        existence.status = QSocPrcmCheckStatus::Error;
        existence.reason = "The action model has no valid step.";
    }
    if (existence.status != QSocPrcmCheckStatus::Sat)
        return existence;
    return QSocPrcmSolver::check(formula.query, budget, stop);
}

QSocPrcmProgressResult QSocPrcmSequenceCheck::progress(
    const QSocPrcmSequencePlan &plan, std::stop_token stop)
{
    QSocPrcmProgressResult result;
    const auto             request = requestFor(plan, result.reason);
    if (!result.reason.isEmpty())
        return result;
    const auto           node = reachable(request, stop);
    QMap<int, qsizetype> index;
    for (qsizetype i = 0; i < node.size(); ++i)
        index.insert(key(node[i]), i);
    for (const auto &target : request) {
        if (!target || stop.stop_requested())
            continue;
        Edge edge;
        edge.resize(node.size());
        QSet<qsizetype> active;
        for (qsizetype i = 0; i < node.size(); ++i) {
            const auto &current = node[i];
            if (!QSocPrcmSequence::complete(current.state, target, current.observation))
                active.insert(i);
            const auto next = QSocPrcmSequence::step(current.state, target, current.observation);
            for (unsigned follow = 0; follow < 16; ++follow) {
                const auto found = index.constFind(
                    key(response(next.state, current.observation, follow)));
                if (found == index.cend()) {
                    result.reason = "The action graph has an unresolved step.";
                    return result;
                }
                if (!edge[i].contains(*found))
                    edge[i].append(*found);
            }
        }
        const auto groupList = component(edge, active, stop);
        if (stop.stop_requested())
            break;
        for (const auto &group : groupList) {
            if (group.size() == 1 && !edge[group.first()].contains(group.first()))
                continue;
            const auto witness = fairWitness(node, group);
            if (witness.isEmpty())
                continue;
            const auto loop = fairLoop(edge, group, witness);
            if (!loop) {
                result.reason = "The action graph cannot reconstruct its cycle.";
                return result;
            }
            result.status = QSocPrcmCheckStatus::Sat;
            result.target = target;
            result.reason = "A fair cycle does not reach " + targetName(*target) + ".";
            for (auto member : *loop)
                result.loop.append(node[member]);
            return result;
        }
    }
    if (stop.stop_requested()) {
        result.status = QSocPrcmCheckStatus::Cancelled;
        result.reason = "Check cancelled.";
    } else {
        result.status = QSocPrcmCheckStatus::Unsat;
    }
    return result;
}
