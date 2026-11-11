// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/report/qsocprimitivereportlayout.h"
#include "gui/report/qsocreportengine.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <QJsonArray>
#include <QLineF>
#include <QMap>
#include <QSet>

namespace QSocReportLayout {
QList<Bridge> crossings(const QList<Edge> &edges)
{
    struct Vertical
    {
        qreal top;
        qreal bottom;
        int   edge;
    };
    QMap<qreal, QList<Vertical>> verticals;
    for (int i = 0; i < edges.size(); ++i) {
        const auto &path = edges[i].path;
        for (int j = 1; j < path.size(); ++j) {
            const auto a = path[j - 1], b = path[j];
            if (a.x() == b.x() && a.y() != b.y())
                verticals[a.x()].append({qMin(a.y(), b.y()), qMax(a.y(), b.y()), i});
        }
    }
    QList<Bridge> result;
    for (int i = 0; i < edges.size(); ++i) {
        const auto               &edge = edges[i];
        QSet<QPair<qreal, qreal>> seen;
        for (int j = 1; j < edge.path.size(); ++j) {
            const auto a = edge.path[j - 1], b = edge.path[j];
            if (a.y() != b.y() || a.x() == b.x())
                continue;
            for (auto it = verticals.lowerBound(qMin(a.x(), b.x()) + 4);
                 it != verticals.cend() && it.key() < qMax(a.x(), b.x()) - 4;
                 ++it) {
                for (const auto &vertical : it.value()) {
                    const auto &other = edges[vertical.edge];
                    if ((edge.from == other.from && edge.output == other.output)
                        || vertical.top > a.y() || vertical.bottom < a.y())
                        continue;
                    const QPointF             point(it.key(), a.y());
                    const QPair<qreal, qreal> key(point.x(), point.y());
                    if (!seen.contains(key)) {
                        seen.insert(key);
                        result.append({point, i});
                    }
                }
            }
        }
    }
    return result;
}

unsigned directionsAt(const QLineF &segment, const QPointF &point)
{
    const auto a = segment.p1(), b = segment.p2();
    if (a.y() == point.y() && b.y() == point.y() && point.x() >= qMin(a.x(), b.x())
        && point.x() <= qMax(a.x(), b.x()))
        return (qMin(a.x(), b.x()) < point.x() ? 1U : 0U)
               | (qMax(a.x(), b.x()) > point.x() ? 2U : 0U);
    if (a.x() == point.x() && b.x() == point.x() && point.y() >= qMin(a.y(), b.y())
        && point.y() <= qMax(a.y(), b.y()))
        return (qMin(a.y(), b.y()) < point.y() ? 4U : 0U)
               | (qMax(a.y(), b.y()) > point.y() ? 8U : 0U);
    return 0;
}

QList<QPointF> junctions(const QList<Edge> &edges)
{
    QMap<QPair<int, int>, QList<QLineF>> nets;
    for (const auto &edge : edges) {
        auto &segments = nets[{edge.from, edge.output}];
        for (int i = 1; i < edge.path.size(); ++i)
            segments.append(QLineF(edge.path[i - 1], edge.path[i]));
    }
    QList<QPointF> result;
    for (const auto &segments : nets) {
        QSet<QPair<qreal, qreal>> points;
        for (const auto &segment : segments) {
            points.insert({segment.x1(), segment.y1()});
            points.insert({segment.x2(), segment.y2()});
        }
        for (const auto &position : points) {
            const QPointF point(position.first, position.second);
            unsigned      directions = 0;
            for (const auto &segment : segments)
                directions |= directionsAt(segment, point);
            if (std::popcount(directions) >= 3 && !result.contains(point))
                result.append(point);
        }
    }
    return result;
}

QRectF body(const Node &node)
{
    const auto  kind = node.data["kind"].toString();
    const qreal margin
        = qMax<qreal>(0, (node.box.width() - node.data["bodyWidth"].toDouble(node.box.width())) / 2);
    const auto box = node.box.adjusted(margin, 0, -margin, 0);
    if (kind == "input" || kind == "output" || kind == "net" || kind == "overview")
        return box;
    if (kind == "mux")
        return node.data["test"].toBool() ? box.adjusted(0, 8, 0, -28)
                                          : box.adjusted(0, 24, 0, -20);
    if (kind == "buffer")
        return box.adjusted(0, 0, 0, -20);
    if (kind == "gate")
        return box.adjusted(0, 36, 0, -20);
    if (kind == "and")
        return box.adjusted(0, 8, 0, -20);
    if (kind == "divider")
        return box.adjusted(0, 0, 0, node.data["control"].toString().isEmpty() ? -8 : -36);
    if (kind == "inverter")
        return box;
    if (kind == "reset")
        return box.adjusted(0, 0, 0, -36);
    return box.adjusted(0, 16, 0, -16);
}

QPointF port(const Node &node, bool input, int index)
{
    const int   count  = qMax(1, node.data[input ? "inputs" : "outputs"].toInt(1));
    const auto  symbol = body(node);
    const qreal y      = symbol.top() + (index + 1) * symbol.height() / (count + 1);
    /* Fixed port sides anchor to the node boundary, including symbol leads. */
    return {input ? node.box.left() : node.box.right(), y};
}

QString validateGraph(const QJsonObject &graph, const QList<QSizeF> &sizes)
{
    const auto nodes = graph["nodes"].toArray(), edges = graph["edges"].toArray();
    const int  count = nodes.size();
    if (count == 0 || count != sizes.size() || count > 4096 || edges.size() > 8192)
        return QStringLiteral("Invalid or oversized report graph");
    for (const auto &size : sizes) {
        if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() <= 0
            || size.height() <= 0)
            return QStringLiteral("Invalid report symbol size");
    }
    QList<QList<int>>     outgoing(count);
    QList<int>            degree(count), queue;
    QSet<QPair<int, int>> sinks;
    for (const auto &value : edges) {
        const auto edge = value.toObject();
        const int  from = edge["from"].toInt(-1), to = edge["to"].toInt(-1);
        if (from < 0 || from >= count || to < 0 || to >= count)
            return QStringLiteral("Invalid report connection");
        const int input = edge["input"].toInt(), output = edge["output"].toInt();
        if (input < 0 || output < 0 || nodes[to].toObject()["kind"] == "input"
            || nodes[from].toObject()["kind"] == "output"
            || input >= qMax(1, nodes[to].toObject()["inputs"].toInt(1))
            || output >= qMax(1, nodes[from].toObject()["outputs"].toInt(1))
            || sinks.contains({to, input}))
            return QStringLiteral("Invalid or multiply driven report port");
        sinks.insert({to, input});
        outgoing[from].append(to);
        ++degree[to];
    }
    if (!graph["power"].toBool()) {
        for (int i = 0; i < count; ++i) {
            if (degree[i] == 0)
                queue.append(i);
        }
        for (int i = 0; i < queue.size(); ++i) {
            for (const int next : outgoing[queue[i]]) {
                if (--degree[next] == 0)
                    queue.append(next);
            }
        }
        if (queue.size() != count)
            return QStringLiteral("The report graph contains a cycle");
    }
    return {};
}

Result layout(const QJsonObject &graph, const QList<QSizeF> &sizes, Engine *engine)
{
    Result     result;
    const auto fail = [&](const QString &message) {
        result.error = message;
        return result;
    };
    result.error = validateGraph(graph, sizes);
    if (!result.error.isEmpty())
        return result;
    const auto data = graph["nodes"].toArray(), links = graph["edges"].toArray();
    const int  count = data.size();
    QJsonArray nodes, edges;
    for (int i = 0; i < count; ++i) {
        const Node node{data[i].toObject(), QRectF(QPointF(), sizes[i])};
        result.nodes.append(node);
        QJsonArray pins;
        for (const bool input : {true, false}) {
            const int n = node.data["kind"] == (input ? "input" : "output")
                              ? 0
                              : qMax(1, node.data[input ? "inputs" : "outputs"].toInt(1));
            for (int j = 0; j < n; ++j) {
                const auto point = port(node, input, j);
                pins.append(
                    QJsonObject{
                        {"id", QString("n%1:%2%3").arg(i).arg(input ? "i" : "o").arg(j)},
                        {"x", point.x()},
                        {"y", point.y()},
                        {"width", 0},
                        {"height", 0},
                        {"layoutOptions", QJsonObject{{"elk.port.side", input ? "WEST" : "EAST"}}}});
            }
        }
        nodes.append(
            QJsonObject{
                {"id", QString("n%1").arg(i)},
                {"width", sizes[i].width()},
                {"height", sizes[i].height()},
                {"ports", pins},
                {"layoutOptions", QJsonObject{{"elk.portConstraints", "FIXED_POS"}}}});
    }
    for (int i = 0; i < links.size(); ++i) {
        const auto value = links[i].toObject();
        const int  from = value["from"].toInt(-1), to = value["to"].toInt(-1);
        const int  input = value["input"].toInt(), output = value["output"].toInt();
        result.edges.append({from, to, input, output, value["style"].toString(), {}});
        edges.append(
            QJsonObject{
                {"id", QString("e%1").arg(i)},
                {"sources", QJsonArray{QString("n%1:o%2").arg(from).arg(output)}},
                {"targets", QJsonArray{QString("n%1:i%2").arg(to).arg(input)}}});
    }
    QJsonObject request{
        {"id", "report"},
        {"children", nodes},
        {"edges", edges},
        {"layoutOptions",
         QJsonObject{
             {"elk.algorithm", "layered"},
             {"elk.direction", "RIGHT"},
             {"elk.edgeRouting", "ORTHOGONAL"},
             {"elk.spacing.nodeNode", graph["overview"].toBool() ? 6 : 16},
             {"elk.spacing.edgeNode", graph["overview"].toBool() ? 6 : 8},
             {"elk.layered.spacing.nodeNodeBetweenLayers", 20},
             {"elk.layered.considerModelOrder.strategy", "NODES_AND_EDGES"},
             {"elk.randomSeed", 1}}}};
    if (graph["overview"].toBool()) {
        auto options = request["layoutOptions"].toObject();
        options["elk.layered.crossingMinimization.forceNodeModelOrder"] = true;
        options["elk.layered.considerModelOrder.components"]            = "MODEL_ORDER";
        options["elk.padding"]                               = "[top=6,left=6,bottom=6,right=6]";
        options["elk.spacing.edgeEdge"]                      = 6;
        options["elk.layered.spacing.edgeEdgeBetweenLayers"] = 6;
        options["elk.layered.spacing.edgeNodeBetweenLayers"] = 6;
        request["layoutOptions"]                             = options;
    }
    Engine     local;
    auto      &backend = engine ? *engine : local;
    const auto output  = backend.layout(request, result.error);
    if (!result.error.isEmpty())
        return result;
    const auto children = output["children"].toArray();
    if (children.size() != count || output["edges"].toArray().size() != links.size())
        return fail("Incomplete report layout");
    for (const auto &value : children) {
        const auto  child = value.toObject();
        bool        valid = false;
        const int   index = child["id"].toString().mid(1).toInt(&valid);
        const qreal x = child["x"].toDouble(qQNaN()), y = child["y"].toDouble(qQNaN());
        if (!valid || index < 0 || index >= count || !std::isfinite(x) || !std::isfinite(y))
            return fail("Invalid report node position");
        result.nodes[index].box.moveTo(x, y);
    }
    for (const auto &value : output["edges"].toArray()) {
        const auto routed   = value.toObject();
        bool       valid    = false;
        const int  index    = routed["id"].toString().mid(1).toInt(&valid);
        const auto sections = routed["sections"].toArray();
        if (!valid || index < 0 || index >= result.edges.size() || sections.size() != 1)
            return fail("Invalid report wire route");
        const auto section = sections.first().toObject();
        QJsonArray points{section["startPoint"]};
        for (const auto &point : section["bendPoints"].toArray())
            points.append(point);
        points.append(section["endPoint"]);
        auto &edge = result.edges[index];
        for (const auto &point : points) {
            const auto p = point.toObject();
            QPointF    position(p["x"].toDouble(qQNaN()), p["y"].toDouble(qQNaN()));
            if (!std::isfinite(position.x()) || !std::isfinite(position.y()))
                return fail("Invalid report wire point");
            if (!edge.path.isEmpty()) {
                if (qAbs(edge.path.last().x() - position.x()) < 1e-6)
                    position.setX(edge.path.last().x());
                else if (qAbs(edge.path.last().y() - position.y()) < 1e-6)
                    position.setY(edge.path.last().y());
                else
                    return fail("A report wire is not orthogonal");
            }
            if (edge.path.isEmpty() || edge.path.last() != position)
                edge.path.append(position);
        }
        if (edge.path.size() < 2
            || (edge.path.first() - port(result.nodes[edge.from], false, edge.output))
                       .manhattanLength()
                   > .01
            || (edge.path.last() - port(result.nodes[edge.to], true, edge.input)).manhattanLength()
                   > .01)
            return fail("A report wire misses its pin");
    }
    result.size = {output["width"].toDouble(qQNaN()), output["height"].toDouble(qQNaN())};
    if (!std::isfinite(result.size.width()) || !std::isfinite(result.size.height())
        || result.size.width() <= 0 || result.size.height() <= 0)
        return fail("Invalid report drawing bounds");
    result.junctions = junctions(result.edges);
    result.bridges   = crossings(result.edges);
    return result;
}
} // namespace QSocReportLayout
