// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmservicecheck.h"
#include "common/qsocprcmgraph_p.h"

#include <QHash>

namespace {

using Core    = QSocPrcmSequence;
using Phase   = QSocPrcmPhase;
using Target  = QSocPrcmTarget;
using Service = QSocPrcmServicePhase;
using Node    = QSocPrcmServiceFrame;
using Local   = QSocPrcmSequenceFrame;
using Literal = QSocPrcmLiteral;

unsigned feedback(const QSocPrcmObservation &value)
{
    return unsigned(value.power) | (unsigned(value.reset) << 1) | (unsigned(value.isolation) << 2)
           | (unsigned(value.idle) << 3);
}

quint32 key(const Node &node)
{
    static_assert(static_cast<unsigned>(Phase::FaultPower) < 16);
    static_assert(static_cast<unsigned>(Service::Return) < 4);
    const auto local = [](const Local &value) {
        return quint32(value.state.phase) | (quint32(value.state.target) << 4)
               | (feedback(value.observation) << 6);
    };
    return local(node.provider) | (local(node.consumer) << 10) | (quint32(node.phase) << 20)
           | (quint32(node.grant) << 22) | (quint32(node.other) << 23);
}

bool request(const Node &node)
{
    return node.phase == Service::Wait || node.phase == Service::Hold;
}

bool ready(const Local &node, Target target)
{
    const auto &value = node.observation;
    switch (target) {
    case Target::Off:
        return node.state.phase == Phase::Off && !value.power && value.reset && value.isolation
               && value.idle;
    case Target::Reset:
        return node.state.phase == Phase::Reset && value.power && value.reset && value.isolation
               && value.idle;
    case Target::Run:
        return node.state.phase == Phase::Run && value.power && !value.reset && !value.isolation
               && !value.idle;
    }
    return false;
}

bool released(const Node &node)
{
    return ready(node.consumer, Target::Off) || ready(node.consumer, Target::Reset)
           || (node.consumer.state.phase == Phase::FaultOff
               && feedback(node.consumer.observation) == 14);
}

Node step(const Node &node, Target provider, Target consumer, bool other)
{
    Node       next      = node;
    const bool available = ready(node.provider, Target::Run);
    const bool failure   = request(node)
                           && (node.phase == Service::Hold ? !node.grant || !available
                                                           : Core::fault(node.provider.state));
    const bool need      = consumer == Target::Run;
    switch (node.phase) {
    case Service::Idle:
        if (need && !Core::fault(node.consumer.state))
            next.phase = Service::Wait;
        break;
    case Service::Wait:
        if (Core::fault(node.provider.state))
            next.phase = Service::Return;
        else if (node.grant)
            next.phase = Service::Hold;
        break;
    case Service::Hold:
        if ((!need || Core::fault(node.consumer.state) || failure) && released(node))
            next.phase = Service::Return;
        break;
    case Service::Return:
        if (!node.grant)
            next.phase = Service::Idle;
        break;
    }
    next.grant            = request(node) && available;
    const bool permission = node.phase == Service::Hold && node.grant && available
                            && !Core::fault(node.provider.state);
    if (need && (!permission || !other) && !Core::fault(node.consumer.state))
        consumer = Core::control(node.consumer.state).power ? Target::Reset : Target::Off;
    next.provider.state = Core::step(
                              node.provider.state,
                              request(node) ? Target::Run : provider,
                              node.provider.observation)
                              .state;
    next.consumer.state
        = Core::step(node.consumer.state, consumer, node.consumer.observation, failure).state;
    return next;
}

void respond(Local &node, unsigned follow)
{
    const auto control = Core::control(node.state);
    if ((follow & 1U) != 0)
        node.observation.power = control.power;
    if ((follow & 2U) != 0)
        node.observation.reset = control.reset;
    if ((follow & 4U) != 0)
        node.observation.isolation = control.isolation;
    if ((follow & 8U) != 0)
        node.observation.idle = control.quiesce;
}

void appendResponse(QList<Node> &node, QSet<quint32> &seen, const Node &next)
{
    for (unsigned follow = 0; follow < 256; ++follow) {
        auto value = next;
        respond(value.provider, follow & 15U);
        respond(value.consumer, follow >> 4);
        const auto id = key(value);
        if (seen.contains(id))
            continue;
        seen.insert(id);
        node.append(value);
    }
}

QList<Node> reachable(std::stop_token stop)
{
    QList<Node>   node{Node{}};
    QSet<quint32> seen{key(node.first())};
    const Target  target[]{Target::Off, Target::Reset, Target::Run};
    for (qsizetype i = 0; i < node.size() && !stop.stop_requested(); ++i) {
        const auto current = node[i];
        for (auto provider : target) {
            for (auto consumer : target) {
                appendResponse(node, seen, step(current, provider, consumer, false));
                appendResponse(node, seen, step(current, provider, consumer, true));
            }
        }
    }
    return node;
}

class Formula
{
public:
    QSocPrcmQuery       query;
    QSocPrcmRequirement model{"service.model", {}};
    QSocPrcmRequirement property{"service.safety", {}};

    Literal bit(const QString &name, bool value = true)
    {
        if (!declared.contains(name)) {
            declared.insert(name);
            query.symbol.append(name);
        }
        return {name, value};
    }

    void row(qsizetype index, const Node &node)
    {
        const auto selected = bit(QString("state.%1").arg(index));
        choice.append(selected);
        const auto           control = Core::control(node.consumer.state);
        const QList<Literal> value{
            bit("request", request(node)),
            bit("grant", node.grant),
            bit("hold", node.phase == Service::Hold),
            bit("provider.run", node.provider.state.phase == Phase::Run),
            bit("provider.power", node.provider.observation.power),
            bit("provider.reset", node.provider.observation.reset),
            bit("provider.isolation", node.provider.observation.isolation),
            bit("provider.idle", node.provider.observation.idle),
            bit("provider.quiesce", Core::control(node.provider.state).quiesce),
            bit("provider.fault", Core::fault(node.provider.state)),
            bit("consumer.fault", Core::fault(node.consumer.state)),
            bit("consumer.reset_request", control.reset),
            bit("consumer.reset", node.consumer.observation.reset)};
        for (const auto &item : value)
            model.clause.append({{selected.symbol, false}, item});
    }

    void violation(const QString &name, const QList<Literal> &condition)
    {
        const auto     flag = bit("violation." + name);
        QList<Literal> reverse{flag};
        for (const auto &item : condition) {
            property.clause.append({{flag.symbol, false}, item});
            reverse.append({item.symbol, !item.value});
        }
        property.clause.append(reverse);
        failure.append(flag);
    }

    void finish()
    {
        model.clause.append(choice);
        violation("provider_fault", {bit("provider.fault")});
        violation("consumer_fault", {bit("consumer.fault")});
        const QList<Literal> unavailable{
            bit("provider.run", false),
            bit("provider.power", false),
            bit("provider.reset"),
            bit("provider.isolation"),
            bit("provider.idle")};
        for (const auto &item : unavailable)
            violation("grant_" + item.symbol, {bit("grant"), item});
        for (const auto &active : {"consumer.reset_request", "consumer.reset"}) {
            for (const auto &item : unavailable)
                violation(QString(active) + '_' + item.symbol, {bit(active, false), item});
            for (const auto &item : {"request", "grant", "hold"})
                violation(QString(active) + '_' + item, {bit(active, false), bit(item, false)});
        }
        violation("accept", {bit("request"), bit("grant"), bit("provider.quiesce")});
        property.clause.append(failure);
        query.requirement = {model, property};
    }

private:
    QSet<QString>  declared;
    QList<Literal> choice, failure;
};

bool complete(const Node &node, Target provider, Target consumer)
{
    const bool running = consumer == Target::Run;
    return Core::complete(
               node.provider.state, running ? Target::Run : provider, node.provider.observation)
           && Core::complete(node.consumer.state, consumer, node.consumer.observation)
           && (running ? node.phase == Service::Hold && node.grant && node.other
                       : node.phase == Service::Idle && !node.grant);
}

QList<qsizetype> fairWitness(const QList<Node> &node, const QList<qsizetype> &group, Target consumer)
{
    QList<qsizetype> witness(9, -1);
    for (auto index : group) {
        const auto &value = node[index];
        const Local local[]{value.provider, value.consumer};
        for (int domain = 0; domain < 2; ++domain) {
            const auto control = Core::control(local[domain].state);
            const auto wanted  = unsigned(control.power) | (unsigned(control.reset) << 1)
                                 | (unsigned(control.isolation) << 2)
                                 | (unsigned(control.quiesce) << 3);
            const auto match   = wanted ^ feedback(local[domain].observation);
            for (int bit = 0; bit < 4; ++bit)
                if ((match & (1U << bit)) == 0)
                    witness[domain * 4 + bit] = index;
        }
        if (consumer != Target::Run || value.other)
            witness[8] = index;
    }
    return witness.contains(-1) ? QList<qsizetype>{} : witness;
}

QList<qsizetype> successor(
    const Node                      &current,
    const QHash<quint32, qsizetype> &index,
    Target                           provider,
    Target                           consumer,
    QString                         &error)
{
    QList<qsizetype> result;
    const auto       next = step(current, provider, consumer, current.other);
    for (unsigned follow = 0; follow < 256; ++follow) {
        for (bool other : {false, true}) {
            if (current.other && !other)
                continue;
            auto value  = next;
            value.other = other;
            respond(value.provider, follow & 15U);
            respond(value.consumer, follow >> 4);
            const auto found = index.constFind(key(value));
            if (found == index.cend()) {
                error = "The service graph has an unresolved step.";
                return {};
            }
            if (!result.contains(*found))
                result.append(*found);
        }
    }
    return result;
}

QSocPrcmServiceProgressResult fixed(
    const QList<Node>               &node,
    const QHash<quint32, qsizetype> &index,
    Target                           provider,
    Target                           consumer,
    std::stop_token                  stop)
{
    QSocPrcmServiceProgressResult result;
    result.provider = provider;
    result.consumer = consumer;
    QSocPrcmGraph::Edge edge(node.size());
    QSet<qsizetype>     active;
    for (qsizetype i = 0; i < node.size() && !stop.stop_requested(); ++i) {
        const auto &current = node[i];
        if (complete(current, provider, consumer))
            continue;
        active.insert(i);
        edge[i] = successor(current, index, provider, consumer, result.reason);
        if (!result.reason.isEmpty())
            return result;
    }
    for (const auto &group : QSocPrcmGraph::component(edge, active, stop)) {
        if (stop.stop_requested())
            break;
        if (group.size() == 1 && !edge[group.first()].contains(group.first()))
            continue;
        const auto witness = fairWitness(node, group, consumer);
        if (witness.isEmpty())
            continue;
        const auto loop = QSocPrcmGraph::fairLoop(edge, group, witness);
        if (!loop) {
            result.reason = "The service graph cannot reconstruct its cycle.";
            return result;
        }
        result.status = QSocPrcmCheckStatus::Sat;
        result.reason = "A fair service cycle does not reach the requested state.";
        for (auto member : *loop)
            result.loop.append(node[member]);
        return result;
    }
    result.status = stop.stop_requested() ? QSocPrcmCheckStatus::Cancelled
                                          : QSocPrcmCheckStatus::Unsat;
    if (stop.stop_requested())
        result.reason = "Check cancelled.";
    return result;
}

} // namespace

QSocPrcmCheckResult QSocPrcmServiceCheck::safety(
    const QSocPrcmCheckBudget &budget, std::stop_token stop)
{
    const auto node = reachable(stop);
    Formula    formula;
    for (qsizetype i = 0; i < node.size() && !stop.stop_requested(); ++i)
        formula.row(i, node[i]);
    formula.finish();
    auto feasible = formula.query;
    feasible.requirement.removeLast();
    auto existence = QSocPrcmSolver::check(feasible, budget, stop);
    if (existence.status == QSocPrcmCheckStatus::Unsat) {
        existence.status = QSocPrcmCheckStatus::Error;
        existence.reason = "The service model has no valid state.";
    }
    if (existence.status != QSocPrcmCheckStatus::Sat)
        return existence;
    return QSocPrcmSolver::check(formula.query, budget, stop);
}

QSocPrcmServiceProgressResult QSocPrcmServiceCheck::progress(std::stop_token stop)
{
    auto       node  = reachable(stop);
    const auto count = node.size();
    for (qsizetype i = 0; i < count; ++i) {
        auto value  = node[i];
        value.other = true;
        node.append(value);
    }
    QHash<quint32, qsizetype> index;
    for (qsizetype i = 0; i < node.size(); ++i)
        index.insert(key(node[i]), i);
    const Target target[]{Target::Off, Target::Reset, Target::Run};
    for (auto provider : target) {
        for (auto consumer : target) {
            const auto result = fixed(node, index, provider, consumer, stop);
            if (result.status != QSocPrcmCheckStatus::Unsat)
                return result;
        }
    }
    QSocPrcmServiceProgressResult result;
    result.status = QSocPrcmCheckStatus::Unsat;
    return result;
}
