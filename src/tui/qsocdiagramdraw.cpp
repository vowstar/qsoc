// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include "tui/qtuiwidget.h"
#include <algorithm>

namespace QSocDiagram::Detail {
namespace {
struct Port
{
    int     node;
    QString label;
    Tip     tip;
    QPoint  position;
};
struct Placement
{
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};
int columns(const QString &text)
{
    return QTuiText::visualWidth(text);
}
char32_t marker(Tip tip, bool vertical)
{
    switch (tip) {
    case Tip::Arrow:
        return vertical ? U'◄' : U'▲';
    case Tip::Triangle:
        return vertical ? U'◁' : U'△';
    case Tip::Diamond:
        return U'◆';
    case Tip::OpenDiamond:
        return U'◇';
    case Tip::Cross:
        return U'×';
    case Tip::None:
        return vertical ? U'─' : U'│';
    }
    return U' ';
}
QString sourceLabel(const Edge &edge)
{
    const auto card = edge.sourceCard.isEmpty() ? QString{} : '(' + edge.sourceCard + ')';
    return card.isEmpty() ? edge.label : edge.label.isEmpty() ? card : card + ' ' + edge.label;
}
void drawNode(Canvas &canvas, const Node &node, const Placement &place)
{
    canvas.box(place.x, place.y, place.width, place.height, node.shape == Shape::Stadium);
    const auto label = (node.shape == Shape::Decision ? QStringLiteral("◇ ") : QString{})
                       + node.label;
    canvas.text(place.x + 2, place.y + 1, label, Role::Node);
    if (node.members.isEmpty())
        return;
    canvas.horizontal(place.x + 1, place.x + place.width - 2, place.y + 2, U'─', Role::Node);
    canvas.put(place.x, place.y + 2, U'├', Role::Node);
    canvas.put(place.x + place.width - 1, place.y + 2, U'┤', Role::Node);
    for (int i = 0; i < node.members.size(); ++i)
        canvas.text(place.x + 2, place.y + 3 + i, node.members[i], Role::Node);
}
} // namespace

Result drawGraph(const Graph &graph, int width)
{
    const bool vertical = graph.orientation == Direction::Down
                          || graph.orientation == Direction::Up;
    const bool reverse = graph.orientation == Direction::Up || graph.orientation == Direction::Left;
    QList<Port>       ports;
    QList<QList<int>> nodePorts(graph.nodes.size());
    int               longestLabel = 0;
    for (const auto &edge : graph.edges) {
        const auto targetLabel = edge.targetCard.isEmpty() ? QString{}
                                                           : '(' + edge.targetCard + ')';
        ports.append({.node = edge.from, .label = sourceLabel(edge), .tip = edge.sourceTip});
        nodePorts[edge.from].append(ports.size() - 1);
        ports.append({.node = edge.to, .label = targetLabel, .tip = edge.targetTip});
        nodePorts[edge.to].append(ports.size() - 1);
    }
    for (const auto &port : ports)
        longestLabel = std::max(longestLabel, columns(port.label));
    QList<Placement> places(graph.nodes.size());
    int              position = 0, crossSize = 0;
    for (int order = 0; order < graph.nodes.size(); ++order) {
        const int   index     = reverse ? graph.nodes.size() - order - 1 : order;
        const auto &node      = graph.nodes[index];
        auto       &place     = places[index];
        int         textWidth = columns(node.label) + (node.shape == Shape::Decision ? 2 : 0);
        for (const auto &member : node.members)
            textWidth = std::max(textWidth, columns(member));
        place.width  = textWidth + 4;
        place.height = node.members.isEmpty() ? 3 : node.members.size() + 4;
        if (vertical) {
            place.y      = position;
            place.height = std::max(place.height, int(nodePorts[index].size()) + 2);
            position += place.height + 2;
            crossSize = std::max(crossSize, place.width);
        } else {
            int portWidth = 2;
            for (const int port : nodePorts[index])
                portWidth += std::max(2, columns(ports[port].label) + 2);
            place.width = std::max(place.width, portWidth);
            place.x     = position;
            position += place.width + 3;
            crossSize = std::max(crossSize, place.height);
        }
    }
    for (int i = 0; i < places.size(); ++i) {
        auto &place = places[i];
        if (vertical)
            place.width = crossSize;
        else
            place.height = crossSize;
        int offset = 1;
        for (const int port : nodePorts[i]) {
            ports[port].position = vertical ? QPoint(place.width, place.y + offset)
                                            : QPoint(place.x + offset, place.height);
            offset += vertical ? 1 : std::max(2, columns(ports[port].label) + 2);
        }
    }
    const int lanes        = graph.edges.isEmpty()
                                 ? 0
                                 : 2 * graph.edges.size() + (vertical ? longestLabel + 3 : 3);
    const int canvasWidth  = vertical ? crossSize + lanes : position - 3;
    const int canvasHeight = vertical ? position - 2 : crossSize + lanes;
    Canvas    canvas(canvasWidth, canvasHeight, width);
    for (int i = 0; i < graph.nodes.size(); ++i)
        drawNode(canvas, graph.nodes[i], places[i]);
    for (int i = 0; i < graph.edges.size(); ++i) {
        const auto    &edge   = graph.edges[i];
        const auto    &source = ports[2 * i];
        const auto    &target = ports[2 * i + 1];
        const char32_t h      = edge.dashed ? U'┄' : U'─';
        const char32_t v      = edge.dashed ? U'┆' : U'│';
        if (vertical) {
            const int lane = crossSize + longestLabel + 3 + 2 * i;
            canvas.horizontal(source.position.x(), lane, source.position.y(), h);
            canvas.horizontal(target.position.x(), lane, target.position.y(), h);
            canvas.vertical(lane, source.position.y(), target.position.y(), v);
            canvas.put(lane, std::min(source.position.y(), target.position.y()), U'┐');
            canvas.put(lane, std::max(source.position.y(), target.position.y()), U'┘');
            for (const auto *port : {&source, &target}) {
                canvas.put(port->position.x(), port->position.y(), marker(port->tip, true));
                if (!port->label.isEmpty())
                    canvas.text(port->position.x() + 1, port->position.y(), port->label);
            }
        } else {
            const int lane = crossSize + 3 + 2 * i;
            canvas.vertical(source.position.x(), source.position.y(), lane, v);
            canvas.vertical(target.position.x(), target.position.y(), lane, v);
            canvas.horizontal(source.position.x(), target.position.x(), lane, h);
            canvas.put(std::min(source.position.x(), target.position.x()), lane, U'└');
            canvas.put(std::max(source.position.x(), target.position.x()), lane, U'┘');
            for (const auto *port : {&source, &target}) {
                canvas.put(port->position.x(), port->position.y(), marker(port->tip, false));
                if (!port->label.isEmpty())
                    canvas.text(port->position.x() + 1, port->position.y(), port->label);
            }
        }
    }
    return canvas.result();
}
} // namespace QSocDiagram::Detail
