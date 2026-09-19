// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMGRAPH_P_H
#define QSOCPRCMGRAPH_P_H

#include <QList>
#include <QMap>
#include <QSet>
#include <QVector>

#include <functional>
#include <optional>
#include <stop_token>

namespace QSocPrcmGraph {

using Edge = QVector<QList<qsizetype>>;

inline QList<QList<qsizetype>> component(
    const Edge &edge, const QSet<qsizetype> &active, std::stop_token stop)
{
    QVector<qsizetype>             serial(edge.size(), -1), low(edge.size(), -1);
    QList<qsizetype>               stack;
    QSet<qsizetype>                onStack;
    QList<QList<qsizetype>>        result;
    qsizetype                      count = 0;
    std::function<void(qsizetype)> visit = [&](qsizetype index) {
        serial[index] = low[index] = count++;
        stack.append(index);
        onStack.insert(index);
        for (auto next : edge[index]) {
            if (stop.stop_requested())
                return;
            if (!active.contains(next))
                continue;
            if (serial[next] == -1) {
                visit(next);
                low[index] = qMin(low[index], low[next]);
            } else if (onStack.contains(next)) {
                low[index] = qMin(low[index], serial[next]);
            }
        }
        if (low[index] != serial[index])
            return;
        QList<qsizetype> group;
        qsizetype        member;
        do {
            member = stack.takeLast();
            onStack.remove(member);
            group.append(member);
        } while (member != index);
        result.append(group);
    };
    for (qsizetype index = 0; index < edge.size() && !stop.stop_requested(); ++index) {
        if (active.contains(index) && serial[index] == -1)
            visit(index);
    }
    return result;
}

inline std::optional<QList<qsizetype>> path(
    const Edge &edge, const QSet<qsizetype> &within, qsizetype from, qsizetype to)
{
    QMap<qsizetype, qsizetype> previous{{from, -1}};
    QList<qsizetype>           pending{from};
    for (qsizetype i = 0; i < pending.size() && !previous.contains(to); ++i) {
        for (auto next : edge[pending[i]]) {
            if (within.contains(next) && !previous.contains(next)) {
                previous.insert(next, pending[i]);
                pending.append(next);
            }
        }
    }
    if (!previous.contains(to))
        return std::nullopt;
    QList<qsizetype> result;
    for (auto at = to; at != from; at = previous[at])
        result.prepend(at);
    return result;
}

inline std::optional<QList<qsizetype>> fairLoop(
    const Edge &edge, const QList<qsizetype> &group, const QList<qsizetype> &witness)
{
    const QSet<qsizetype> within(group.cbegin(), group.cend());
    QList<qsizetype>      walk{group.first()};
    auto                  appendPath = [&](qsizetype target) {
        const auto segment = path(edge, within, walk.last(), target);
        if (!segment)
            return false;
        walk.append(*segment);
        return true;
    };
    for (auto target : witness) {
        if (!appendPath(target))
            return std::nullopt;
    }
    if (walk.size() == 1) {
        for (auto next : edge[walk.first()]) {
            if (within.contains(next)) {
                walk.append(next);
                break;
            }
        }
    }
    if (walk.size() == 1 || !appendPath(walk.first()))
        return std::nullopt;
    return walk;
}

} // namespace QSocPrcmGraph

#endif // QSOCPRCMGRAPH_P_H
