// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRIMITIVEREPORTLAYOUT_H
#define QSOCPRIMITIVEREPORTLAYOUT_H

#include <QJsonObject>
#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>

namespace QSocReportLayout {
class Engine;
inline constexpr QSizeF continuationPortSize{96, 56};
struct Node
{
    QJsonObject data;
    QRectF      box;
};
struct Edge
{
    int       from   = 0;
    int       to     = 0;
    int       input  = 0;
    int       output = 0;
    QString   style;
    QPolygonF path;
};
struct Bridge
{
    QPointF position;
    int     edge = 0;
};
struct Result
{
    QList<Node>    nodes;
    QList<Edge>    edges;
    QList<QPointF> junctions;
    QList<Bridge>  bridges;
    QSizeF         size;
    QString        error;
};
struct Budget
{
    int    calls = 8192;
    qint64 nodes = 1024 * 1024;
    qint64 edges = 2 * 1024 * 1024;
};
QPointF port(const Node &node, bool input, int index);
QRectF  body(const Node &node);
QString validateGraph(const QJsonObject &graph, const QList<QSizeF> &sizes);
Result  layout(
    const QJsonObject   &graph,
    const QList<QSizeF> &sizes,
    Engine              *engine = nullptr,
    Budget              *budget = nullptr);
QList<Result> paginate(
    const QJsonObject   &graph,
    const QList<QSizeF> &sizes,
    Engine              &engine,
    int                  group,
    const Result        *initial = nullptr,
    Budget              *budget  = nullptr);
QList<Bridge> crossings(const QList<Edge> &edges);
} // namespace QSocReportLayout

#endif
