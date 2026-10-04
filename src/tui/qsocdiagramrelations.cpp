// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include <QRegularExpression>

namespace QSocDiagram::Detail {
namespace {
QString description(QString &text, bool required = false)
{
    text = text.trimmed();
    if (text.isEmpty()) {
        require(!required);
        return {};
    }
    require(take(text, ":") && !text.startsWith("::"));
    const auto value = text.trimmed();
    checkLabel(value);
    text.clear();
    return value;
}
void stateTitle(Graph &graph, int node, const QString &text)
{
    checkLabel(text);
    if (graph.nodes[node].declared)
        graph.member(node, text);
    else {
        graph.nodes[node].label    = text;
        graph.nodes[node].declared = true;
    }
}
int stateNode(Graph &graph, QString &text, bool initial)
{
    if (!take(text, "[*]"))
        return graph.node(identifier(text));
    const int index          = graph.node(initial ? "[initial]" : "[final]");
    graph.nodes[index].label = initial ? QStringLiteral("● initial") : QStringLiteral("◎ final");
    return index;
}
void stateLine(Graph &graph, QString text)
{
    if (take(text, "state \"")) {
        text.prepend('"');
        const auto label = quoted(text);
        require(take(text, "as "));
        const int index = graph.node(identifier(text));
        require(text.trimmed().isEmpty());
        stateTitle(graph, index, label);
        return;
    }
    const QStringList reserved = {"state", "direction", "note", "end", "hide"};
    require(!reserved.contains(text));
    const int from     = stateNode(graph, text, true);
    text               = text.trimmed();
    const bool initial = graph.nodes[from].id == "[initial]";
    if (text.isEmpty()) {
        require(!initial);
        return;
    }
    if (text.startsWith(':')) {
        require(!initial);
        stateTitle(graph, from, description(text));
        return;
    }
    require(take(text, "-->"));
    const int to = stateNode(graph, text, false);
    graph.edge({.from = from, .to = to, .label = description(text)});
}
QString attribute(QString text)
{
    const auto type = identifier(text);
    require(!text.isEmpty() && text.front().isSpace());
    const auto name    = identifier(text);
    text               = text.trimmed();
    const int  quoteAt = text.indexOf('"');
    const auto keys    = (quoteAt < 0 ? text : text.first(quoteAt)).trimmed();
    for (const auto &key : keys.split(',', Qt::SkipEmptyParts))
        require(QStringList{"PK", "FK", "UK"}.contains(key.trimmed()));
    require(
        keys.isEmpty() || (!keys.startsWith(',') && !keys.endsWith(',') && !keys.contains(",,")));
    QString value = type + ' ' + name;
    if (!keys.isEmpty())
        value += ' ' + keys;
    if (quoteAt >= 0) {
        text.remove(0, quoteAt);
        value += QStringLiteral(" : ") + quoted(text);
        require(text.trimmed().isEmpty());
    }
    checkLabel(value);
    return value;
}
Tip endpoint(const QString &token)
{
    if (token.isEmpty())
        return Tip::None;
    if (token == "<" || token == ">")
        return Tip::Arrow;
    if (token == "<|" || token == "|>")
        return Tip::Triangle;
    if (token == "*")
        return Tip::Diamond;
    if (token == "o")
        return Tip::OpenDiamond;
    reject();
}
void relationship(Graph &graph, int from, QString text, bool er)
{
    Edge edge;
    edge.from      = from;
    edge.targetTip = Tip::None;
    text           = text.trimmed();
    if (!er && text.startsWith('"'))
        edge.sourceCard = quoted(text);
    text = text.trimmed();
    static const QRegularExpression stem(QStringLiteral("^(.*?)(--|\\.\\.)(.*)$"));
    const auto                      match = stem.match(text);
    require(match.hasMatch() && match.captured(1).size() <= 2);
    const auto left = match.captured(1);
    edge.dashed     = match.captured(2) == "..";
    text            = match.captured(3);
    if (er) {
        const QStringList leftTokens  = {"||", "|o", "}|", "}o"};
        const QStringList rightTokens = {"||", "o|", "|{", "o{"};
        const QStringList cards       = {"1", "0..1", "1..many", "0..many"};
        const int         source      = leftTokens.indexOf(left);
        const int         target      = rightTokens.indexOf(text.first(2));
        require(source >= 0 && target >= 0);
        edge.sourceCard = cards[source];
        edge.targetCard = cards[target];
        text.remove(0, 2);
    } else {
        edge.sourceTip = endpoint(left);
        for (const QString &token : {"|>", ">", "*", "o"}) {
            if (!text.startsWith(token))
                continue;
            if (token == "o" && text.size() > 1 && (text[1].isLetterOrNumber() || text[1] == '_'))
                continue;
            edge.targetTip = endpoint(token);
            text.remove(0, token.size());
            break;
        }
        text = text.trimmed();
        if (text.startsWith('"'))
            edge.targetCard = quoted(text);
    }
    edge.to    = graph.node(identifier(text));
    edge.label = description(text, er);
    require(!er || !edge.label.contains('"'));
    graph.edge(edge);
}
} // namespace

Graph relations(const QString &header, const QStringList &body)
{
    Graph                           graph;
    const bool                      state      = header.startsWith("stateDiagram");
    const bool                      er         = header == "erDiagram";
    bool                            directed   = false;
    int                             memberNode = -1;
    static const QRegularExpression metadata(
        QStringLiteral("^(?:accTitle\\s*:|accDescr\\s*[:{])"),
        QRegularExpression::CaseInsensitiveOption);
    for (QString text : body) {
        if (memberNode >= 0) {
            if (text == "}")
                memberNode = -1;
            else {
                require(!text.contains('{') && !text.contains('}'));
                graph.member(memberNode, er ? attribute(text) : text);
            }
            continue;
        }
        if (take(text, "direction ")) {
            require(!directed);
            graph.orientation = direction(text.trimmed());
            directed          = true;
            continue;
        }
        require(!text.contains(metadata));
        if (state) {
            stateLine(graph, text);
            continue;
        }
        const bool declaration = !er && take(text, "class ");
        const int  from        = graph.node(identifier(text));
        text                   = text.trimmed();
        if ((er || declaration) && text == "{") {
            memberNode = from;
            continue;
        }
        if (declaration || (er && text.isEmpty())) {
            require(text.isEmpty());
            continue;
        }
        if (!er && text.startsWith(':')) {
            graph.member(from, description(text));
            continue;
        }
        relationship(graph, from, text, er);
    }
    require(memberNode < 0 && !graph.nodes.isEmpty());
    return graph;
}
} // namespace QSocDiagram::Detail
