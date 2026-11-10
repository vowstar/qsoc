// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/report/qsocprimitivereportlayout.h"
#include "gui/report/qsocreportengine.h"

#include <algorithm>
#include <QJsonArray>
#include <QMap>
#include <QSet>

namespace QSocReportLayout {
namespace {
class Sheets
{
public:
    Sheets(const QJsonObject &graph, const QList<QSizeF> &sizes, Engine &engine, int group)
        : graph(graph)
        , nodes(graph["nodes"].toArray())
        , edges(graph["edges"].toArray())
        , sizes(sizes)
        , engine(engine)
        , group(group)
    {}

    QList<Result> run()
    {
        auto initial = layout(graph, sizes, &engine);
        if (!initial.error.isEmpty())
            return {initial};
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
            initial.nodes[i].data["original"] = i;
        }
        for (const auto &value : edges) {
            const auto edge = value.toObject();
            if (nodes[edge["from"].toInt()].toObject()["kind"] == "input"
                && nodes[edge["to"].toInt()].toObject()["kind"] == "output")
                core.insert(edge["to"].toInt());
        }
        partition(core, initial);
        return result;
    }

private:
    Result subset(const QSet<int> &core)
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
            const QString signal = QString("C%1.%2.%3").arg(group).arg(from).arg(output);
            const QString key    = signal + (input ? ":i" : ":o");
            if (!ports.contains(key)) {
                ports.insert(key, localNodes.size());
                localNodes.append(
                    QJsonObject{
                        {"kind", input ? "input" : "output"},
                        {"label", signal},
                        {"continuation", true},
                        {"reference", true}});
                localSizes.append({96, 56});
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
        return layout(local, localSizes, &engine);
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
        QList<int> ordered(core.begin(), core.end());
        std::sort(ordered.begin(), ordered.end(), [&](int a, int b) {
            const auto pa = positions.value(a), pb = positions.value(b);
            const auto first = horizontal ? pa.x() : pa.y(), second = horizontal ? pb.x() : pb.y();
            return first == second ? a < b : first < second;
        });
        const auto      middle = ordered.cbegin() + ordered.size() / 2;
        const QSet<int> left(ordered.cbegin(), middle), right(middle, ordered.cend());
        partition(left, subset(left));
        partition(right, subset(right));
    }

    QJsonObject   graph;
    QJsonArray    nodes, edges;
    QList<QSizeF> sizes;
    Engine       &engine;
    int           group;
    QList<Result> result;
};
} // namespace

QList<Result> paginate(
    const QJsonObject &graph, const QList<QSizeF> &sizes, Engine &engine, int group)
{
    return Sheets(graph, sizes, engine, group).run();
}
} // namespace QSocReportLayout
