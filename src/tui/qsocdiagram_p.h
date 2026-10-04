// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCDIAGRAM_P_H
#define QSOCDIAGRAM_P_H

#include "tui/qsocdiagram.h"
#include <QPoint>
#include <QStringList>

namespace QSocDiagram::Detail {
inline constexpr int sourceLimit = 16384;
inline constexpr int nodeLimit   = 16;
inline constexpr int edgeLimit   = 24;
inline constexpr int labelLimit  = 40;
inline constexpr int canvasLimit = 65536;
struct Failure
{
    Error error;
};
[[noreturn]] void reject(Error error = Error::Unsupported);
void              require(bool condition, Error error = Error::Unsupported);
void              checkLabel(const QString &text);
QStringList       statements(const QString &source);
QString           identifier(QString &text);
QString           quoted(QString &text);
QString           takeLabel(QString &text, const QString &closing);
bool              take(QString &text, const QString &prefix);

enum class Direction { Down, Up, Right, Left };
enum class Shape { Rectangle, Decision, Stadium };
enum class Tip { None, Arrow, Triangle, Diamond, OpenDiamond, Cross };
Direction direction(const QString &text);
struct Node
{
    QString     id;
    QString     label;
    QStringList members;
    Shape       shape    = Shape::Rectangle;
    bool        declared = false;
};
struct Edge
{
    int     from = 0;
    int     to   = 0;
    QString label;
    QString sourceCard;
    QString targetCard;
    Tip     sourceTip = Tip::None;
    Tip     targetTip = Tip::Arrow;
    bool    dashed    = false;
};
struct Graph
{
    QList<Node> nodes;
    QList<Edge> edges;
    Direction   orientation = Direction::Down;
    int         node(const QString &id);
    void        edge(const Edge &value);
    void        member(int index, const QString &value);
};
Graph  flowchart(const QString &header, const QStringList &body);
Graph  relations(const QString &header, const QStringList &body);
Result drawGraph(const Graph &graph, int width);
Result sequence(const QStringList &body, int width);

struct Cell
{
    char32_t scalar = U' ';
    Role     role   = Role::Edge;
};
class Canvas
{
public:
    Canvas(int columns, int lines, int available);
    void     put(int x, int y, char32_t scalar, Role role = Role::Edge);
    char32_t at(int x, int y) const;
    void     text(int x, int y, const QString &value, Role role = Role::Text);
    void     horizontal(int x1, int x2, int y, char32_t scalar, Role role = Role::Edge);
    void     vertical(int x, int y1, int y2, char32_t scalar, Role role = Role::Edge);
    void     box(int x, int y, int columns, int lines, bool rounded = false);
    Result   result() const;

private:
    int         width;
    int         height;
    int         availableWidth;
    QList<Cell> cells;
};
} // namespace QSocDiagram::Detail

#endif // QSOCDIAGRAM_P_H
