// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/report/qsocprimitivereportlayout.h"
#include "gui/report/qsocreportengine.h"

#include <algorithm>
#include <optional>
#include <QJsonArray>
#include <QMap>
#include <QSet>

namespace QSocReportLayout {
namespace {
class Sheets
{
public:
    Sheets(
        const QJsonObject   &graph,
        const QList<QSizeF> &sizes,
        Engine              &engine,
        int                  group,
        Budget              *budget)
        : graph(graph)
        , nodes(graph["nodes"].toArray())
        , edges(graph["edges"].toArray())
        , sizes(sizes)
        , engine(engine)
        , group(group)
        , budget(budget)
    {}

    QList<Result> run(const Result *initial)
    {
        Result checked;
        checked.error = validateGraph(graph, sizes);
        if (!checked.error.isEmpty())
            return {checked};
        QSet<int> core, connected;
        for (const auto &value : edges) {
            const auto edge = value.toObject();
            connected.insert(edge["from"].toInt());
            connected.insert(edge["to"].toInt());
        }
        for (int i = 0; i < nodes.size(); ++i) {
            const auto kind = nodes[i].toObject()["kind"].toString();
            if ((kind != "input" && kind != "output") || !connected.contains(i))
                core.insert(i);
        }
        for (const auto &value : edges) {
            const auto edge = value.toObject();
            if (nodes[edge["from"].toInt()].toObject()["kind"] == "input"
                && nodes[edge["to"].toInt()].toObject()["kind"] == "output")
                core.insert(edge["to"].toInt());
        }
        if (graph["overview"].toBool()) {
            QSet<int>  members;
            Result     last;
            const auto fits = [](const Result &drawing) {
                return drawing.size.width() <= 944 && drawing.size.height() <= 448;
            };
            for (const auto &unit : units(core)) {
                auto drawing = subset(members | unit, true);
                if (drawing && !drawing->error.isEmpty())
                    return {*drawing};
                if (!members.isEmpty() && (!drawing || !fits(*drawing))) {
                    result.append(last);
                    members.clear();
                    drawing = subset(unit, true);
                }
                if (!drawing) {
                    Result failure;
                    failure.error = "An overview unit exceeds its graph-work limit";
                    return {failure};
                }
                if (!drawing->error.isEmpty())
                    return {*drawing};
                if (fits(*drawing)) {
                    members |= unit;
                    last = *drawing;
                } else {
                    partition(unit, *drawing);
                }
            }
            if (!members.isEmpty())
                result.append(last);
            return result;
        }
        auto drawing = initial ? *initial : layout(graph, sizes, &engine, budget);
        if (!drawing.error.isEmpty())
            return {drawing};
        for (int i = 0; i < nodes.size(); ++i)
            drawing.nodes[i].data["original"] = i;
        partition(core, drawing);
        return result;
    }

private:
    QList<QSet<int>> units(const QSet<int> &core) const
    {
        QList<int> ordered(core.begin(), core.end());
        std::sort(ordered.begin(), ordered.end());
        QMap<int, int> roots;
        for (const int node : ordered)
            roots.insert(node, node);
        const auto root = [&](int node) {
            while (roots.value(node) != node)
                node = roots.value(node);
            return node;
        };
        if (graph["overview"].toBool()) {
            for (const auto &value : edges) {
                const auto edge = value.toObject();
                const int  from = edge["from"].toInt(), to = edge["to"].toInt();
                if (!core.contains(from) || !core.contains(to))
                    continue;
                const int owner = nodes[from].toObject()["owner"].toInt(-1);
                if (owner >= 0 && owner == nodes[to].toObject()["owner"].toInt(-1))
                    roots[root(to)] = root(from);
            }
        }
        QMap<int, QSet<int>> grouped;
        for (const int node : ordered)
            grouped[root(node)].insert(node);
        return grouped.values();
    }

    std::optional<Result> subset(const QSet<int> &core, bool bounded = false)
    {
        QSet<int> included = core;
        for (const auto &value : edges) {
            const auto edge = value.toObject();
            const int  from = edge["from"].toInt(), to = edge["to"].toInt();
            if (core.contains(to) && nodes[from].toObject()["kind"] == "input")
                included.insert(from);
            if (core.contains(from) && nodes[to].toObject()["kind"] == "output")
                included.insert(to);
        }
        QJsonArray     localNodes, localEdges;
        QList<QSizeF>  localSizes;
        QMap<int, int> indices;
        for (int i = 0; i < nodes.size(); ++i) {
            if (!included.contains(i))
                continue;
            indices.insert(i, localNodes.size());
            auto node        = nodes[i].toObject();
            node["original"] = i;
            localNodes.append(node);
            localSizes.append(sizes[i]);
        }
        QMap<QString, int> ports;
        const auto         continuation = [&](const QJsonObject &edge, bool input) {
            const int     from = edge["from"].toInt(), output = edge["output"].toInt();
            const auto    source = nodes[from].toObject();
            const QString named  = source["kind"] == "overview" ? source["signal"].toString()
                                   : source["kind"] == "net"    ? source["label"].toString()
                                                                : QString();
            const QString signal = named.isEmpty()
                                       ? QString("C%1.%2.%3").arg(group).arg(from).arg(output)
                                       : named;
            const QString key    = signal + (input ? ":i" : ":o");
            if (!ports.contains(key)) {
                ports.insert(key, localNodes.size());
                localNodes.append(
                    QJsonObject{
                        {"kind", input ? "input" : "output"},
                        {"label", signal},
                        {"continuation", true},
                        {"reference", true}});
                localSizes.append(continuationPortSize);
            }
            return ports.value(key);
        };
        for (const auto &value : edges) {
            auto      edge = value.toObject();
            const int from = edge["from"].toInt(), to = edge["to"].toInt();
            if (!included.contains(from) && !included.contains(to))
                continue;
            if (!included.contains(to) && nodes[from].toObject()["kind"] == "input")
                continue;
            edge["from"] = included.contains(from) ? indices.value(from) : continuation(edge, true);
            edge["to"]   = included.contains(to) ? indices.value(to)
                                                 : continuation(value.toObject(), false);
            if (!included.contains(from))
                edge["output"] = 0;
            if (!included.contains(to)) {
                edge["input"] = 0;
                edge["style"] = "";
            }
            if (!localEdges.contains(edge))
                localEdges.append(edge);
        }
        auto local     = graph;
        local["nodes"] = localNodes;
        local["edges"] = localEdges;
        if (bounded && (localNodes.size() > 64 || localEdges.size() > 128))
            return {};
        return layout(local, localSizes, &engine, budget);
    }

    void partition(const QSet<int> &core, Result drawing)
    {
        if (!drawing.error.isEmpty()
            || (drawing.size.width() <= 944 && drawing.size.height() <= 448)) {
            result.append(drawing);
            return;
        }
        if (core.size() < 2) {
            drawing.error = "A report symbol exceeds the printable area";
            result.append(drawing);
            return;
        }
        const bool         horizontal = drawing.size.width() / 944 > drawing.size.height() / 448;
        QMap<int, QPointF> positions;
        for (const auto &node : drawing.nodes)
            positions.insert(node.data["original"].toInt(-1), node.box.center());
        auto grouped = units(core);
        if (grouped.size() < 2) {
            grouped.clear();
            for (const int node : core)
                grouped.append(QSet<int>{node});
        }
        const auto center = [&](const QSet<int> &members) {
            QPointF value;
            for (const int node : members)
                value += positions.value(node);
            return value / members.size();
        };
        std::sort(grouped.begin(), grouped.end(), [&](const QSet<int> &a, const QSet<int> &b) {
            const auto pa = center(a), pb = center(b);
            const auto first = horizontal ? pa.x() : pa.y(), second = horizontal ? pb.x() : pb.y();
            return first == second ? *std::min_element(a.cbegin(), a.cend())
                                         < *std::min_element(b.cbegin(), b.cend())
                                   : first < second;
        });
        QSet<int> left, right;
        for (int i = 0; i < grouped.size(); ++i)
            (i < grouped.size() / 2 ? left : right) |= grouped[i];
        partition(left, *subset(left));
        partition(right, *subset(right));
    }

    QJsonObject   graph;
    QJsonArray    nodes, edges;
    QList<QSizeF> sizes;
    Engine       &engine;
    int           group;
    Budget       *budget;
    QList<Result> result;
};
} // namespace

QList<Result> paginate(
    const QJsonObject   &graph,
    const QList<QSizeF> &sizes,
    Engine              &engine,
    int                  group,
    const Result        *initial,
    Budget              *budget)
{
    return Sheets(graph, sizes, engine, group, budget).run(initial);
}
} // namespace QSocReportLayout
