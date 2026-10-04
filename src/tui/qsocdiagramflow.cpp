// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include <QRegularExpression>

namespace QSocDiagram::Detail {
namespace {
int flowNode(QString &text, Graph &graph)
{
    const auto        id = identifier(text);
    const QStringList reserved
        = {"end", "subgraph", "direction", "style", "class", "classDef", "linkStyle", "click"};
    require(!reserved.contains(id));
    for (const auto &prefix : {"[(", "[[", "[/", "[\\", "{{"})
        require(!text.startsWith(prefix));
    const int index = graph.node(id);
    Shape     shape = Shape::Rectangle;
    QString   closing;
    if (text.startsWith("([")) {
        text.remove(0, 2);
        closing = "])";
        shape   = Shape::Stadium;
    } else if (text.startsWith('[')) {
        text.remove(0, 1);
        closing = "]";
    } else if (text.startsWith('{')) {
        text.remove(0, 1);
        closing = "}";
        shape   = Shape::Decision;
    }
    if (closing.isEmpty())
        return index;
    const auto label = takeLabel(text, closing);
    auto      &node  = graph.nodes[index];
    require(!node.declared || (node.label == label && node.shape == shape));
    node.label    = label;
    node.shape    = shape;
    node.declared = true;
    return index;
}
QList<int> group(QString &text, Graph &graph)
{
    QList<int> result{flowNode(text, graph)};
    while (take(text, "&")) {
        require(result.size() < edgeLimit, Error::Limit);
        result.append(flowNode(text, graph));
    }
    return result;
}
Edge link(QString &text)
{
    Edge edge;
    bool matched = false;
    for (const QString &token : {"<-.->", "<-->", "-.->", "-.-", "-->", "---"}) {
        if (!take(text, token))
            continue;
        edge.dashed    = token.contains('.');
        edge.sourceTip = token.startsWith('<') ? Tip::Arrow : Tip::None;
        edge.targetTip = token.endsWith('>') ? Tip::Arrow : Tip::None;
        require(edge.targetTip != Tip::None || (!text.startsWith('o') && !text.startsWith('x')));
        if (take(text, "|"))
            edge.label = takeLabel(text, "|");
        matched = true;
        break;
    }
    if (matched)
        return edge;
    QString end;
    if (take(text, "--"))
        end = "--";
    else if (take(text, "-.")) {
        end         = ".-";
        edge.dashed = true;
    } else
        reject();
    const int position = text.indexOf(end);
    require(position > 1 && text.front().isSpace() && text[position - 1].isSpace());
    auto label = text.first(position).trimmed() + '|';
    edge.label = takeLabel(label, "|");
    text.remove(0, position + end.size());
    require(text.startsWith('>'));
    text.remove(0, 1);
    return edge;
}
} // namespace
Graph flowchart(const QString &header, const QStringList &body)
{
    const auto words = header.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    require(
        (words.size() == 1 || words.size() == 2)
        && (words[0] == "flowchart" || words[0] == "graph"));
    Graph graph;
    if (words.size() == 2)
        graph.orientation = direction(words[1]);
    for (QString text : body) {
        auto from = group(text, graph);
        while (!text.trimmed().isEmpty()) {
            auto       edge = link(text);
            const auto to   = group(text, graph);
            require(from.size() * to.size() <= edgeLimit - graph.edges.size(), Error::Limit);
            for (const int source : from)
                for (const int target : to) {
                    edge.from = source;
                    edge.to   = target;
                    graph.edge(edge);
                }
            from = to;
        }
    }
    require(!graph.nodes.isEmpty());
    return graph;
}
} // namespace QSocDiagram::Detail
