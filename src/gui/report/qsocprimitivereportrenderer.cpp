// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/report/qsocprimitivereportrenderer.h"
#include "common/config.h"
#include "common/qsocprimitivereport.h"
#include "gui/report/qsocprimitivereportlayout.h"
#include "gui/report/qsocreportengine.h"

#include <algorithm>
#include <functional>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPageLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSet>
#include <QSvgGenerator>
#include <QSvgRenderer>
#include <QTextLayout>
#include <QUrl>
#include <QtMath>

namespace {
const QColor ink("#16191d"), blue("#245e9a"), muted("#56616c"), border("#9bb8d1");
struct Label
{
    QPointF     position;
    QStringList lines;
    int         size  = 14;
    QColor      color = ink;
};
struct Cell
{
    QRectF box;
    bool   header = false;
};
struct Diagram
{
    QString                  title;
    QList<int>               panels;
    int                      page = 0;
    QSocReportLayout::Result graph;
};
struct Page
{
    QString                        title;
    QSize                          size{1040, 720};
    QList<Label>                   labels;
    QList<Cell>                    cells;
    QList<QPair<Diagram, QPointF>> diagrams;
};

QString circuitLegend(const QString &kind)
{
    if (kind == "Clock")
        return "Dashed: test. STA: serial buffer. Dots join. Arcs cross. Repeated U: same device. "
               "Named ports cite continuation pages.";
    if (kind == "Reset")
        return "Bubbles invert. AND combines active-low resets. Triangle pin: clock. Dots join. "
               "Arcs cross. Named ports cite continuation pages.";
    return "Solid: hard dependency. Dashed: soft dependency. Dots join. Arcs cross. Named ports "
           "cite continuation pages.";
}

QByteArray resource(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QFont font(const QStringList &families, int size)
{
    QFont result;
    result.setFamilies(families);
    result.setStyleStrategy(QFont::PreferNoShaping);
    result.setPixelSize(size);
    return result;
}

qreal leading(const QFont &face)
{
    return QFontMetricsF(face).height() + 3;
}

QStringList wrap(const QString &text, const QFont &face, qreal available, int limit = 0)
{
    QStringList result;
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    for (const QString &paragraph : text.split('\n')) {
        QTextLayout layout(paragraph, face);
        layout.setTextOption(option);
        layout.beginLayout();
        for (;;) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(available);
            result.append(paragraph.mid(line.textStart(), line.textLength()).trimmed());
        }
        layout.endLayout();
        if (paragraph.isEmpty())
            result.append(QString());
    }
    if (limit > 0 && result.size() > limit) {
        result = result.mid(0, limit);
        result.last()
            = QFontMetricsF(face).elidedText(result.last() + "...", Qt::ElideRight, available);
    }
    return result;
}

void text(
    QPainter          &painter,
    const QRectF      &box,
    const QStringList &lines,
    const QFont       &face,
    Qt::Alignment      alignment = Qt::AlignCenter,
    const QColor      &color     = ink)
{
    const QFontMetricsF metrics(face);
    const qreal         top = alignment.testFlag(Qt::AlignVCenter)
                                  ? box.center().y() - lines.size() * leading(face) / 2
                                  : box.top();
    painter.setFont(face);
    painter.setPen(color);
    for (qsizetype i = 0; i < lines.size(); ++i) {
        qreal x = box.left();
        if (alignment.testFlag(Qt::AlignHCenter))
            x = box.center().x() - metrics.horizontalAdvance(lines[i]) / 2;
        else if (alignment.testFlag(Qt::AlignRight))
            x = box.right() - metrics.horizontalAdvance(lines[i]);
        painter.drawText(QPointF(x, top + metrics.ascent() + i * leading(face)), lines[i]);
    }
}

QList<QSizeF> measure(QJsonObject &graph, const QStringList &families)
{
    QList<QSizeF> sizes;
    const auto    face = font(families, 14), small = font(families, 12);
    auto          nodes = graph["nodes"].toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        auto       node = nodes[i].toObject();
        const auto kind = node["kind"].toString();
        if (kind == "input" || kind == "output" || kind == "net") {
            const qreal width = graph["power"].toBool() && kind == "output" ? 192 : 160;
            const int   lines = wrap(node["label"].toString(), face, width - 28, 2).size();
            const bool  note  = !node["note"].toString().isEmpty() || node["reference"].toBool();
            sizes.append(
                {width, qMax<qreal>(32, lines * leading(face) + (note ? leading(small) : 0) + 8)});
        } else if (kind == "mux") {
            int largest = node["inputs"].toInt(1) - 1;
            for (const auto &index : node["indices"].toArray())
                largest = qMax(largest, index.toInt());
            const qreal width = 32 + 24 * (QString::number(largest).size() - 1);
            const bool  test  = node["test"].toBool();
            sizes.append(
                {width, (node["inputs"].toInt(1) + 1) * (test ? 18.0 : 24.0) + (test ? 36 : 44)});
        } else if (kind == "buffer") {
            sizes.append({24, 44});
        } else if (kind == "gate") {
            sizes.append({48, 88});
        } else if (kind == "and") {
            sizes.append({48, (node["inputs"].toInt(1) + 1) * 24.0 + 28});
        } else if (kind == "divider") {
            const qreal width = qMax<qreal>(
                72,
                qMin<qreal>(
                    160, QFontMetricsF(face).horizontalAdvance(node["label"].toString()) + 12));
            const int lines = wrap(node["label"].toString(), face, width - 12, 2).size();
            sizes.append(
                {width,
                 qMax<qreal>(32, lines * leading(face) + 8)
                     + (node["control"].toString().isEmpty() ? 8 : 36)});
        } else if (kind == "inverter") {
            sizes.append({30, 28});
        } else if (kind == "reset") {
            const int lines = wrap(node["label"].toString(), small, 76, 2).size()
                              + wrap(node["note"].toString(), small, 76, 2).size();
            sizes.append({88, qMax<qreal>(56, lines * leading(small) + 20) + 36});
        } else {
            const int ports = qMax(node["inputs"].toInt(1), node["outputs"].toInt(1));
            sizes.append({180, qMax<qreal>(132, ports * 22 + 48)});
        }
        node["bodyWidth"]  = sizes.last().width();
        const auto control = kind == "mux"     ? node["select"].toString()
                             : kind == "reset" ? node["clock"].toString()
                                               : node["control"].toString();
        qreal annotation = control.isEmpty() ? 0
                                             : QFontMetricsF(small).horizontalAdvance(control) + 8;
        if (kind == "mux" && !node["test"].toBool() && !node["reference"].toString().isEmpty())
            annotation = qMax(
                annotation,
                QFontMetricsF(small).horizontalAdvance(
                    node["reference"].toString() + "/" + node["label"].toString())
                    + 8);
        sizes.last().setWidth(qMax(sizes.last().width(), qMin<qreal>(120, annotation)));
        nodes[i] = node;
    }
    graph["nodes"] = nodes;
    return sizes;
}

void drawControl(
    QPainter          &painter,
    const QPointF     &pin,
    const QString     &name,
    qreal              end,
    bool               below,
    bool               activeLow,
    const QStringList &families,
    qreal              available)
{
    if (name.isEmpty())
        return;
    const auto face = font(families, 12);
    painter.setPen(QPen(ink, 1.3));
    painter.setBrush(Qt::white);
    if (activeLow) {
        const qreal center = pin.y() + (below ? 3 : -3);
        painter.drawEllipse(QPointF(pin.x(), center), 3, 3);
        painter.drawLine(QPointF(pin.x(), center + (below ? 3 : -3)), QPointF(pin.x(), end));
    } else {
        painter.drawLine(pin, QPointF(pin.x(), end));
    }
    text(
        painter,
        {pin.x() - available / 2,
         below ? end + 1 : end - leading(face) - 1,
         available,
         leading(face)},
        {QFontMetricsF(face).elidedText(name, Qt::ElideMiddle, available)},
        face);
}

void drawPinLeads(QPainter &painter, const QSocReportLayout::Node &node)
{
    const auto symbol = QSocReportLayout::body(node);
    if (symbol.width() == node.box.width())
        return;
    for (const bool input : {true, false}) {
        const int count = qMax(1, node.data[input ? "inputs" : "outputs"].toInt(1));
        for (int i = 0; i < count; ++i) {
            QPen pen(ink, 1.3);
            if (input && i == 1 && node.data["test"].toBool())
                pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            const auto pin = QSocReportLayout::port(node, input, i);
            painter.drawLine(pin, QPointF(input ? symbol.left() : symbol.right(), pin.y()));
        }
    }
    painter.setPen(QPen(ink, 1.3));
}

void drawNode(QPainter &painter, const QSocReportLayout::Node &node, const QStringList &families)
{
    const auto data = node.data;
    const auto kind = data["kind"].toString(), label = data["label"].toString(),
               note = data["note"].toString();
    const auto box = node.box, symbol = QSocReportLayout::body(node);
    const auto face = font(families, 14), small = font(families, 12);
    painter.setPen(QPen(ink, 1.3));
    painter.setBrush(Qt::white);
    if (kind == "input" || kind == "output" || kind == "net") {
        const qreal y = box.center().y();
        if (kind == "net") {
            painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
            text(
                painter,
                {box.left(), box.top(), box.width(), leading(face)},
                wrap(label, face, box.width(), 2),
                face);
            return;
        }
        const bool  input = kind == "input";
        const qreal x     = input ? box.right() : box.left() + 12;
        painter.drawPolyline(QPolygonF{{x - 7, y - 4}, {x, y}, {x - 7, y + 4}});
        painter.drawLine(QPointF(input ? box.right() - 16 : box.left(), y), QPointF(x, y));
        auto lines = wrap(label, face, box.width() - 28);
        if (lines.size() > 2)
            lines = {QFontMetricsF(face).elidedText(label, Qt::ElideMiddle, box.width() - 28)};
        if (!note.isEmpty())
            lines.append(wrap(note, small, box.width() - 28, 1));
        text(
            painter,
            box.adjusted(input ? 0 : 22, 0, input ? -22 : 0, 0),
            lines,
            face,
            Qt::AlignVCenter | (input ? Qt::AlignRight : Qt::AlignLeft));
        return;
    }
    drawPinLeads(painter, node);
    if (kind == "mux") {
        const qreal upper = symbol.center().y() - 9, lower = symbol.center().y() + 9;
        painter.drawPolygon(
            QPolygonF{
                symbol.topLeft(),
                {symbol.right(), upper},
                {symbol.right(), lower},
                symbol.bottomLeft()});
        const auto indices = data["indices"].toArray();
        for (int i = 0; i < data["inputs"].toInt(1); ++i) {
            const auto pin = QSocReportLayout::port(node, true, i);
            text(
                painter,
                {symbol.left() + 2, pin.y() - leading(small) / 2, symbol.width() - 4, leading(small)},
                {QString::number(i < indices.size() ? indices[i].toInt(i) : i)},
                small,
                Qt::AlignLeft | Qt::AlignVCenter);
        }
        const bool  test     = data["test"].toBool();
        const qreal controlY = test ? (symbol.bottom() + lower) / 2 : (symbol.top() + upper) / 2;
        drawControl(
            painter,
            {symbol.center().x(), controlY},
            data["select"].toString(),
            test ? box.bottom() - 18 : box.top() + 18,
            test,
            false,
            families,
            box.width());
        if (!test)
            text(
                painter,
                {symbol.left(), symbol.bottom() + 2, symbol.width(), leading(small)},
                {data["reference"].toString().isEmpty()
                     ? label
                     : data["reference"].toString() + "/" + label},
                small);
    } else if (kind == "buffer" || kind == "inverter") {
        const bool  inverted = kind == "inverter";
        const qreal tip      = symbol.right() - (inverted ? 10 : 6);
        painter.drawPolygon(
            QPolygonF{
                {symbol.left(), symbol.top() + 2},
                {tip, symbol.center().y()},
                {symbol.left(), symbol.bottom() - 2}});
        if (inverted) {
            painter.drawLine(
                QPointF(tip, symbol.center().y()), QPointF(symbol.right() - 6, symbol.center().y()));
            painter.drawEllipse(QPointF(symbol.right() - 3, symbol.center().y()), 3, 3);
        } else {
            painter.drawLine(
                QPointF(tip, symbol.center().y()), QPointF(symbol.right(), symbol.center().y()));
            text(
                painter,
                {symbol.left() - 4, symbol.bottom() + 2, symbol.width() + 8, leading(small)},
                {label},
                small);
        }
    } else if (kind == "gate" || kind == "and") {
        QPainterPath shape;
        shape.moveTo(symbol.topLeft());
        shape.lineTo(symbol.center().x(), symbol.top());
        shape.arcTo(symbol, 90, -180);
        shape.lineTo(symbol.bottomLeft());
        shape.closeSubpath();
        painter.drawPath(shape);
        if (kind == "gate") {
            drawControl(
                painter,
                {symbol.center().x(), symbol.top()},
                data["control"].toString(),
                symbol.top() - 14,
                false,
                data["activeLow"].toBool(),
                families,
                box.width());
            text(
                painter,
                {symbol.left(), symbol.bottom() + 2, symbol.width(), leading(small)},
                {"ICG"},
                small);
        } else if (!data["reference"].toString().isEmpty()) {
            text(
                painter,
                {symbol.left(), symbol.bottom() + 2, symbol.width(), leading(small)},
                {data["reference"].toString()},
                small);
        }
    } else if (kind == "divider") {
        painter.drawRoundedRect(symbol, 3, 3);
        text(painter, symbol.adjusted(4, 0, -4, 0), wrap(label, face, symbol.width() - 8, 2), face);
        drawControl(
            painter,
            {symbol.center().x(), symbol.bottom()},
            data["control"].toString(),
            symbol.bottom() + 14,
            true,
            false,
            families,
            box.width());
    } else if (kind == "reset") {
        painter.drawRect(symbol);
        const qreal x = symbol.center().x(), y = symbol.bottom();
        painter.drawPolyline(QPolygonF{{x - 4, y}, {x, y - 6}, {x + 4, y}});
        auto lines = wrap(label, small, symbol.width() - 12, 2);
        if (!note.isEmpty())
            lines.append(wrap(note, small, symbol.width() - 12, 2));
        text(painter, symbol.adjusted(6, 0, -6, -10), lines, small);
        drawControl(
            painter, {x, y}, data["clock"].toString(), y + 14, true, false, families, box.width());
    } else {
        painter.drawRect(symbol);
        const auto words = symbol.adjusted(6, 0, kind == "power" ? -36 : -6, 0);
        auto       lines = wrap(label, face, words.width() - 8, 2);
        if (!note.isEmpty())
            lines.append(wrap(note, small, words.width() - 8, 1));
        text(painter, words, lines, face);
        if (kind == "power") {
            const QStringList names{"RDY", "ICG", "RST", "SW", "FLT"};
            for (int i = 0; i < names.size(); ++i) {
                const auto pin = QSocReportLayout::port(node, false, i);
                text(
                    painter,
                    {symbol.right() - 30, pin.y() - leading(small) / 2, 26, leading(small)},
                    {names[i]},
                    small,
                    Qt::AlignRight | Qt::AlignVCenter);
            }
        }
    }
}

void drawGraph(
    QPainter                       &painter,
    const QSocReportLayout::Result &graph,
    const QPointF                  &origin,
    const QStringList              &families)
{
    painter.save();
    painter.translate(origin);
    for (const auto &edge : graph.edges) {
        QPen wire(ink, 1.3);
        if (edge.style == "soft" || edge.style == "test")
            wire.setStyle(Qt::DashLine);
        painter.setPen(wire);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(edge.path);
    }
    QMap<QPair<int, qreal>, QList<qreal>> bridgeRows;
    for (const auto &bridge : graph.bridges)
        bridgeRows[{bridge.edge, bridge.position.y()}].append(bridge.position.x());
    for (auto it = bridgeRows.begin(); it != bridgeRows.end(); ++it) {
        auto &positions = it.value();
        std::sort(positions.begin(), positions.end());
        for (int i = 0; i < positions.size(); ++i) {
            qreal radius = 4;
            if (i > 0)
                radius = qMin(radius, (positions[i] - positions[i - 1]) / 2.5);
            if (i + 1 < positions.size())
                radius = qMin(radius, (positions[i + 1] - positions[i]) / 2.5);
            const QPointF p(positions[i], it.key().second);
            painter.setPen(QPen(Qt::white, 4.4));
            painter.drawLine(QPointF(p.x() - radius, p.y()), QPointF(p.x() + radius, p.y()));
            QPainterPath arc;
            arc.moveTo(p.x() - radius, p.y());
            arc.cubicTo(
                p.x() - radius,
                p.y() - radius * 1.4,
                p.x() + radius,
                p.y() - radius * 1.4,
                p.x() + radius,
                p.y());
            painter.drawPath(arc);
            painter.setPen(QPen(ink, 1.3));
            painter.drawPath(arc);
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    for (const auto &point : graph.junctions)
        painter.drawEllipse(point, 2.5, 2.5);
    for (const auto &node : graph.nodes)
        drawNode(painter, node, families);
    painter.restore();
}

QJsonArray slice(const QJsonArray &array, int start, int count)
{
    QJsonArray result;
    for (int i = start; i < qMin(start + count, int(array.size())); ++i)
        result.append(array[i]);
    return result;
}

QList<Diagram> circuits(const QJsonArray &panels, const QStringList &families)
{
    QList<Diagram>           result;
    QSocReportLayout::Engine engine;
    QMap<QString, QString>   labels;
    for (const auto &value : panels) {
        const auto panel = value.toObject();
        labels.insert(
            panel["identity"].toString(panel["title"].toString()), panel["output"].toString());
    }
    std::function<void(const QJsonArray &, const QList<int> &, const QString &)> append;
    append = [&](const QJsonArray &members, const QList<int> &owners, const QString &caption) {
        auto graph     = QSocPrimitiveReport::graph(members);
        graph["power"] = members.first().toObject()["merge"].toObject()["kind"] == "power";
        auto nodes     = graph["nodes"].toArray();
        for (int i = 0; i < nodes.size(); ++i) {
            auto node = nodes[i].toObject();
            if (node["kind"] == "input" && labels.contains(node["label"].toString())) {
                node["label"]     = labels.value(node["label"].toString());
                node["reference"] = true;
            }
            nodes[i] = node;
        }
        graph["nodes"] = nodes;
        QStringList names;
        for (const auto &member : members)
            names.append(member.toObject()["title"].toString());
        const auto sizes   = measure(graph, families);
        const auto drawing = QSocReportLayout::layout(graph, sizes, &engine);
        const bool fits    = drawing.size.width() <= 944 && drawing.size.height() <= 448;
        if (drawing.error.isEmpty() && !fits && members.size() > 1) {
            const int middle = members.size() / 2;
            append(slice(members, 0, middle), owners.mid(0, middle), caption);
            append(slice(members, middle, members.size()), owners.mid(middle), caption);
            return;
        }
        const auto sheets
            = fits || !drawing.error.isEmpty()
                  ? QList<QSocReportLayout::Result>{drawing}
                  : QSocReportLayout::paginate(graph, sizes, engine, result.size() + 1);
        for (int i = 0; i < sheets.size(); ++i)
            result.append(
                {names.join(" / ") + caption
                     + (sheets.size() > 1
                            ? QString(" / sheet %1 of %2").arg(i + 1).arg(sheets.size())
                            : QString()),
                 owners,
                 0,
                 sheets[i]});
    };
    QList<QList<int>>     groups;
    QMap<QByteArray, int> current;
    for (int index = 0; index < panels.size(); ++index) {
        const auto panel    = panels[index].toObject();
        const auto branches = panel["branches"].toArray();
        if (branches.size() > 4 || panel["exits"].toArray().size() > 4
            || panel["followers"].toArray().size() > 4) {
            for (int start = 0; start < branches.size(); start += 4) {
                auto part        = panel;
                part["branches"] = slice(branches, start, 4);
                auto       merge = part["merge"].toObject();
                QJsonArray indices;
                for (int i = start; i < qMin(start + 4, int(branches.size())); ++i)
                    indices.append(i);
                merge["indices"]     = indices;
                merge["reference"]   = QString("U%1").arg(index + 1);
                merge["totalInputs"] = branches.size();
                part["merge"]        = merge;
                part["followers"]    = QJsonArray();
                part["exits"] = start == 0 ? slice(panel["exits"].toArray(), 0, 4) : QJsonArray();
                append(
                    {part},
                    {index},
                    branches.size() > 4 ? QString(" / U%3 inputs %1-%2 of %4")
                                              .arg(start)
                                              .arg(qMin(start + 3, int(branches.size()) - 1))
                                              .arg(index + 1)
                                              .arg(branches.size())
                                        : QString());
            }
            for (int start = 4; start < panel["exits"].toArray().size(); start += 4) {
                auto part         = panel;
                part["branches"]  = slice(branches, 0, 4);
                part["followers"] = QJsonArray();
                part["exits"]     = slice(panel["exits"].toArray(), start, 4);
                append({part}, {index}, " / remaining outputs");
            }
            for (int start = 0; start < panel["followers"].toArray().size(); start += 4) {
                auto part         = panel;
                part["branches"]  = slice(branches, 0, 4);
                part["exits"]     = QJsonArray();
                part["followers"] = slice(panel["followers"].toArray(), start, 4);
                append(
                    {part},
                    {index},
                    QString(" / reset followers %1-%2")
                        .arg(start + 1)
                        .arg(qMin(start + 4, int(panel["followers"].toArray().size()))));
            }
            continue;
        }
        QJsonArray signature;
        for (const auto &branch : branches)
            signature.append(branch.toObject()["source"]);
        signature.append(panel["merge"].toObject()["kind"]);
        const auto key = QJsonDocument(signature).toJson(QJsonDocument::Compact);
        if (!current.contains(key) || groups[current.value(key)].size() >= 3) {
            current.insert(key, groups.size());
            groups.append(QList<int>());
        }
        groups[current.value(key)].append(index);
    }
    for (const auto &group : groups) {
        QJsonArray members;
        for (const int index : group)
            members.append(panels[index]);
        append(members, group, {});
    }
    std::stable_sort(result.begin(), result.end(), [](const Diagram &a, const Diagram &b) {
        return a.panels.first() < b.panels.first();
    });
    return result;
}

QList<Page> circuitPages(QList<Diagram> &diagrams, int offset)
{
    QList<Page> pages;
    Page        page;
    page.title = "2. Signal paths";
    qreal y    = 144;
    for (int i = 0; i < diagrams.size(); ++i) {
        auto       &diagram = diagrams[i];
        const qreal height  = diagram.graph.size.height() + 56;
        if (y + height > 650 && !page.diagrams.isEmpty()) {
            pages.append(page);
            page       = Page();
            page.title = "2. Signal paths";
            y          = 144;
        }
        diagram.page = offset + pages.size() + 1;
        page.diagrams.append({diagram, QPointF(32, y)});
        y += height + 12;
    }
    if (!page.diagrams.isEmpty())
        pages.append(page);
    QMap<QString, int> references;
    for (const auto &diagram : diagrams) {
        for (const auto &node : diagram.graph.nodes) {
            if (node.data["kind"] == "output" && !references.contains(node.data["label"].toString()))
                references.insert(node.data["label"].toString(), diagram.page);
        }
    }
    QMap<QString, QList<int>> consumers;
    for (const auto &diagram : diagrams) {
        for (const auto &node : diagram.graph.nodes) {
            if (node.data["kind"] == "input" && node.data["continuation"].toBool()
                && !consumers[node.data["label"].toString()].contains(diagram.page))
                consumers[node.data["label"].toString()].append(diagram.page);
        }
    }
    QMap<int, QPair<int, int>> ranges;
    for (const auto &diagram : diagrams) {
        for (const int panel : diagram.panels) {
            auto &range = ranges[panel];
            if (range.first == 0)
                range.first = diagram.page;
            range.second = diagram.page;
        }
    }
    const auto annotate = [&](Diagram &diagram) {
        const auto range = ranges.value(diagram.panels.first());
        if (range.first != range.second)
            diagram.title += QString(" / pages %1-%2").arg(range.first).arg(range.second);
        for (auto &node : diagram.graph.nodes) {
            const auto label = node.data["label"].toString();
            if (node.data["kind"] == "input" && references.contains(label))
                node.data["note"] = QString("page %1").arg(references.value(label));
            if (node.data["kind"] == "output" && consumers.contains(label)) {
                QStringList targets;
                for (const int target : consumers.value(label))
                    targets.append(QString::number(target));
                node.data["note"] = "to p. " + targets.join(",");
            }
        }
    };
    for (auto &diagram : diagrams)
        annotate(diagram);
    for (auto &sheet : pages) {
        for (auto &entry : sheet.diagrams)
            annotate(entry.first);
    }
    return pages;
}

QList<Page> indexPages(
    const QJsonArray &panels, const QList<Diagram> &diagrams, const QStringList &families)
{
    QList<Page> pages;
    const QFont face = font(families, 14);
    for (int start = 0; start < panels.size(); start += 18) {
        Page page;
        page.title = "1. Target index";
        page.labels.append(
            {{32, 142},
             {QString("%1 targets. Circuit sheets retain the original input indices.")
                  .arg(panels.size())},
             14,
             muted});
        const QList<qreal> x{32, 272, 752, 908, 1008};
        const QStringList  headers{"Target", "Sources", "Note", "Page"};
        qreal              y = 174;
        for (int column = 0; column < 4; ++column) {
            page.cells.append({{x[column], y, x[column + 1] - x[column], 28}, true});
            page.labels.append({{x[column] + 8, y + 5}, {headers[column]}, 14, ink});
        }
        y += 28;
        for (int i = start; i < qMin(start + 18, int(panels.size())); ++i) {
            const auto  panel = panels[i].toObject();
            QStringList sources;
            for (const auto &branch : panel["branches"].toArray())
                sources.append(branch.toObject()["source"].toString().remove("domain:"));
            int first = 0, last = 0;
            for (const auto &diagram : diagrams) {
                if (!diagram.panels.contains(i))
                    continue;
                if (first == 0)
                    first = diagram.page;
                last = diagram.page;
            }
            const QStringList values{
                panel["title"].toString(),
                sources.join(", "),
                panel["outputNote"].toString(panel["subtitle"].toString()),
                first == last ? QString::number(first) : QString("%1-%2").arg(first).arg(last)};
            for (int column = 0; column < 4; ++column) {
                const qreal width = x[column + 1] - x[column];
                page.cells.append({{x[column], y, width, 24}, false});
                page.labels.append(
                    {{x[column] + 8, y + 3},
                     {QFontMetricsF(face).elidedText(values[column], Qt::ElideRight, width - 16)},
                     14,
                     ink});
            }
            y += 24;
        }
        pages.append(page);
    }
    return pages;
}

QJsonArray parameterSections(QJsonArray panels)
{
    if (panels.size() < 2)
        return panels;
    QJsonArray shared;
    const auto first = panels.first().toObject()["details"].toArray();
    for (const auto &row : first) {
        const bool common = std::all_of(panels.begin(), panels.end(), [&](const auto &panel) {
            return panel.toObject()["details"].toArray().contains(row);
        });
        if (common)
            shared.append(row);
    }
    if (shared.isEmpty())
        return panels;
    for (int i = 0; i < panels.size(); ++i) {
        auto       panel = panels[i].toObject();
        QJsonArray rows;
        for (const auto &row : panel["details"].toArray()) {
            if (!shared.contains(row))
                rows.append(row);
        }
        panel["details"] = rows;
        panels[i]        = panel;
    }
    panels.prepend(QJsonObject{{"title", "Shared by all targets"}, {"details", shared}});
    return panels;
}

QList<Page> parameters(const QJsonArray &panels, const QStringList &families)
{
    const QFont face = font(families, 14);
    const qreal line = leading(face);
    QList<Page> pages;
    Page        page;
    page.title         = "3. Parameters";
    qreal      y       = 144;
    const auto heading = [&](const QString &name) {
        page.labels.append({{32, y}, wrap(name, font(families, 18), 976, 1), 18, ink});
        y += 32;
        page.cells.append({{32, y, 248, 30}, true});
        page.cells.append({{280, y, 728, 30}, true});
        page.labels.append({{40, y + 5}, {"Parameter"}, 14, ink});
        page.labels.append({{288, y + 5}, {"Value"}, 14, ink});
        y += 30;
    };
    for (qsizetype index = 0; index < panels.size(); ++index) {
        const auto    panel         = panels[index].toObject();
        const QString title         = QString("3.%1 ").arg(index + 1) + panel["title"].toString();
        const auto    rows          = panel["details"].toArray();
        const auto    first         = rows.isEmpty() ? QJsonObject() : rows.first().toObject();
        const int     freshCapacity = static_cast<int>((650 - 144 - 62 - 16) / line);
        const int     firstLines    = qMax(
            wrap(first["name"].toString(), face, 232).size(),
            wrap(first["value"].toString(), face, 712).size());
        if (y + 62 + qMin(firstLines, freshCapacity) * line + 16 > 650) {
            pages.append(page);
            page       = Page();
            page.title = "3. Parameters";
            y          = 144;
        }
        heading(title);
        for (const auto &value : rows) {
            const auto  row    = value.toObject();
            QStringList keys   = wrap(row["name"].toString(), face, 232);
            QStringList values = wrap(row["value"].toString(), face, 712);
            while (!keys.isEmpty() || !values.isEmpty()) {
                int       capacity = static_cast<int>((650 - y - 16) / line);
                const int needed   = qMax(keys.size(), values.size());
                if (capacity < 1
                    || (needed > capacity && (needed <= freshCapacity || capacity < 3))) {
                    pages.append(page);
                    page       = Page();
                    page.title = "3. Parameters";
                    y          = 144;
                    heading(title + " (continued)");
                    capacity = static_cast<int>((650 - y - 16) / line);
                }
                const int   count     = qMin<qsizetype>(capacity, qMax(keys.size(), values.size()));
                const qreal rowHeight = count * line + 16;
                page.cells.append({{32, y, 248, rowHeight}, false});
                page.cells.append({{280, y, 728, rowHeight}, false});
                page.labels.append({{40, y + 8}, keys.mid(0, count), 14, ink});
                page.labels.append({{288, y + 8}, values.mid(0, count), 14, ink});
                keys   = keys.mid(qMin<qsizetype>(count, keys.size()));
                values = values.mid(qMin<qsizetype>(count, values.size()));
                y += rowHeight;
            }
        }
        y += 24;
    }
    if (!page.cells.isEmpty())
        pages.append(page);
    return pages;
}

void paint(
    QPainter          &painter,
    const Page        &page,
    const QStringList &families,
    const QString     &name,
    const QString     &kind,
    int                number,
    int                total)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(QPoint(), page.size), Qt::white);
    QSvgRenderer logo(QStringLiteral(":/primitive-report/logo.svg"));
    if (number == 1)
        logo.render(&painter, QRectF(32, 24, 136, 42));
    else
        text(
            painter,
            {32, 24, 136, 42},
            {"QSoC"},
            font(families, 22),
            Qt::AlignLeft | Qt::AlignVCenter);
    const QFont   small = font(families, 14);
    const QString shortName
        = QFontMetricsF(small).elidedText(name, Qt::ElideRight, page.size.width() - 240);
    text(
        painter,
        {200, 24, page.size.width() - 232.0, 42},
        {shortName, kind + " controller"},
        small,
        Qt::AlignRight | Qt::AlignVCenter);
    painter.setPen(QPen(ink, 1));
    painter.drawLine(32, 80, page.size.width() - 32, 80);
    text(
        painter,
        {32, 100, page.size.width() - 64.0, 32},
        {page.title},
        font(families, 20),
        Qt::AlignLeft | Qt::AlignTop);
    for (const auto &entry : page.diagrams) {
        const auto &diagram = entry.first;
        const auto  origin  = entry.second;
        text(
            painter,
            {origin, QSizeF(976, 24)},
            wrap(diagram.title, font(families, 16), 976, 1),
            font(families, 16),
            Qt::AlignLeft | Qt::AlignTop);
        QPen frame(border, .8);
        frame.setStyle(Qt::DashLine);
        painter.setPen(frame);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(origin.x(), origin.y() + 28, 976, diagram.graph.size.height() + 20));
        drawGraph(
            painter,
            diagram.graph,
            {origin.x() + (976 - diagram.graph.size.width()) / 2, origin.y() + 38},
            families);
    }
    if (!page.diagrams.isEmpty())
        text(
            painter,
            {32, 654, 976, 22},
            {circuitLegend(kind)},
            font(families, 12),
            Qt::AlignLeft | Qt::AlignTop,
            muted);
    for (const auto &cell : page.cells) {
        painter.setPen(QPen(QColor("#68727c"), .7));
        painter.setBrush(cell.header ? QColor("#eceff1") : QColor(Qt::white));
        painter.drawRect(cell.box);
    }
    for (const auto &label : page.labels)
        text(
            painter,
            {label.position,
             QSizeF(page.size.width(), label.lines.size() * leading(font(families, label.size)))},
            label.lines,
            font(families, label.size),
            Qt::AlignLeft | Qt::AlignTop,
            label.color);
    painter.setPen(QPen(ink, 1));
    painter.drawLine(32, page.size.height() - 40, page.size.width() - 32, page.size.height() - 40);
    text(
        painter,
        {32, page.size.height() - 30.0, 600, 24},
        {QStringLiteral("QSoC v") + QSOC_VERSION},
        small,
        Qt::AlignLeft | Qt::AlignTop);
    text(
        painter,
        {page.size.width() - 200.0, page.size.height() - 30.0, 168, 24},
        {QString("%1 / %2").arg(number).arg(total)},
        small,
        Qt::AlignRight | Qt::AlignTop);
}

QString diagramSvg(
    const QSocReportLayout::Result &graph, const QStringList &families, const QString &title)
{
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QSvgGenerator generator;
    const QSize   size(qCeil(graph.size.width()) + 32, qCeil(graph.size.height()) + 32);
    generator.setOutputDevice(&buffer);
    generator.setTitle(title);
    generator.setDescription(
        "Circuit connections are listed in the connection table below the figure.");
    generator.setResolution(96);
    generator.setSize(size);
    generator.setViewBox(QRect(QPoint(), size));
    {
        QPainter painter(&generator);
        painter.fillRect(QRect(QPoint(), size), Qt::white);
        QPen frame(border, .8);
        frame.setStyle(Qt::DashLine);
        painter.setPen(frame);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(1, 1, size.width() - 2, size.height() - 2));
        drawGraph(painter, graph, {16, 16}, families);
    }
    const QString result = QString::fromUtf8(bytes);
    auto          svg    = result.mid(result.indexOf("<svg"));
    svg.insert(4, " role=\"img\" aria-label=\"" + title.toHtmlEscaped() + "\"");
    return svg;
}

QPageLayout paper()
{
    return {QPageSize(QPageSize::A4), QPageLayout::Landscape, QMarginsF(10, 10, 10, 10)};
}

QString escape(const QString &value)
{
    return value.toHtmlEscaped();
}
QString htmlParameters(const QJsonArray &input)
{
    const auto panels = parameterSections(input);
    QString    details;
    for (qsizetype i = 0; i < panels.size(); ++i) {
        const auto panel = panels[i].toObject();
        details += QString(
                       "<details class=\"parameters\" id=\"panel-%1\"><summary>3.%2 "
                       "%3</summary><table><caption>Configuration parameters</caption><tbody>")
                       .arg(i)
                       .arg(i + 1)
                       .arg(escape(panel["title"].toString()));
        for (const auto &value : panel["details"].toArray()) {
            const auto row = value.toObject();
            details += "<tr><th scope=\"row\">" + escape(row["name"].toString()) + "</th><td>"
                       + escape(row["value"].toString()) + "</td></tr>";
        }
        details += "</tbody></table></details>";
    }
    return details;
}

QString htmlSheetLink(const QList<Diagram> &diagrams, int sheet)
{
    return "<a href=\"#path-" + QString::number(sheet) + "\">Sheet " + QString::number(sheet + 1)
           + " (PDF p. " + QString::number(diagrams[sheet].page) + ")</a>";
}

QString htmlIndex(const QJsonArray &panels, const QList<Diagram> &diagrams)
{
    QString               result = QString(
                                       "<p>%1 targets / %2 circuit sheets. Expand a target group to locate "
                                       "its signal paths.</p>")
                                       .arg(panels.size())
                                       .arg(diagrams.size());
    QMap<int, QList<int>> paths;
    QMap<QString, int>    references;
    for (int i = 0; i < diagrams.size(); ++i) {
        for (const int owner : diagrams[i].panels) {
            paths[owner].append(i);
            if (paths[owner].size() > 1)
                continue;
            const auto panel = panels[owner].toObject();
            for (const auto &key : {"title", "identity", "output"}) {
                if (!panel[key].toString().isEmpty())
                    references.insert(panel[key].toString(), i);
            }
        }
    }
    for (int start = 0; start < panels.size(); start += 16) {
        result += "<details class=\"index-group\"><summary>Targets " + QString::number(start + 1)
                  + " to " + QString::number(qMin(start + 16, int(panels.size())))
                  + "</summary><table><thead><tr><th>Target</th><th>Sources</th><th>PDF "
                    "page</th></tr></thead><tbody>";
        for (int i = start; i < qMin(start + 16, int(panels.size())); ++i) {
            const auto  panel = panels[i].toObject();
            QStringList sources;
            const auto  branches = panel["branches"].toArray();
            for (const auto &branch : branches) {
                const auto source = branch.toObject()["source"].toString();
                const auto label  = escape(QString(source).remove("domain:"));
                sources.append(
                    references.contains(source)
                        ? "<a href=\"#path-" + QString::number(references.value(source)) + "\">"
                              + label + "</a>"
                        : label);
            }
            const auto sheets = paths.value(i);
            const int  path = sheets.value(0), last = sheets.value(sheets.size() - 1, path);
            result += "<tr><td><a href=\"#path-" + QString::number(path) + "\">"
                      + escape(panel["title"].toString()) + "</a>" + "</td><td>"
                      + (branches.size() > 4 ? QString::number(branches.size()) + " inputs"
                                             : sources.join(", "))
                      + "</td><td>" + QString::number(diagrams[path].page)
                      + (diagrams[path].page == diagrams[last].page
                             ? QString()
                             : "-" + QString::number(diagrams[last].page));
            if (sheets.size() > 1) {
                result += "<details class=\"sheet-links\"><summary>All "
                          + QString::number(sheets.size()) + " sheets</summary><ol>";
                for (const int sheet : sheets)
                    result += "<li>" + htmlSheetLink(diagrams, sheet) + "</li>";
                result += "</ol></details>";
            }
            result += "</td></tr>";
        }
        result += "</tbody></table></details>";
    }
    return result;
}

QString htmlConnections(const QSocReportLayout::Result &graph)
{
    const auto describe = [&](int index) {
        const auto &node  = graph.nodes[index].data;
        QString     label = node["label"].toString();
        if (label.isEmpty())
            label = node["kind"].toString();
        QStringList
            facts{node["kind"].toString(), node["note"].toString(), node["reference"].toString()};
        if (node["totalInputs"].toInt() > 0)
            facts.append(QString("%1 total inputs").arg(node["totalInputs"].toInt()));
        if (node["activeLow"].toBool())
            facts.append("active-low control");
        facts.removeAll(QString());
        return escape(QString::number(index + 1) + ". " + label + " (" + facts.join(", ") + ")");
    };
    QString result
        = "<details class=\"connections\"><summary>Connections and control signals</summary>"
          "<table><thead><tr><th>From</th><th>To</th><th>Connection</th></tr></thead><tbody>";
    for (const auto &edge : graph.edges) {
        const auto node    = graph.nodes[edge.to].data;
        const auto indices = node["indices"].toArray();
        const int  input   = edge.input < indices.size() ? indices[edge.input].toInt() : edge.input;
        result += "<tr><td>" + describe(edge.from) + "</td><td>" + describe(edge.to)
                  + "</td><td>output " + QString::number(edge.output) + ", input "
                  + QString::number(input)
                  + (edge.style.isEmpty() ? QString() : ", " + escape(edge.style)) + "</td></tr>";
    }
    result += "</tbody></table><dl>";
    for (int i = 0; i < graph.nodes.size(); ++i) {
        const auto &node = graph.nodes[i].data;
        QStringList controls;
        for (const auto &key : {"select", "control", "clock"}) {
            if (!node[key].toString().isEmpty())
                controls.append(QString(key) + ": " + node[key].toString());
        }
        if (!controls.isEmpty())
            result += "<dt>" + describe(i) + "</dt><dd>" + escape(controls.join(", ")) + "</dd>";
    }
    return result + "</dl></details>";
}

QString htmlSheetNavigation(int sheet, int count)
{
    QString result = "<nav class=\"sheet-navigation\" aria-label=\"Circuit sheet navigation\">"
                     "<a href=\"#overview\">Target index</a>";
    if (sheet > 0)
        result += "<a rel=\"prev\" href=\"#path-" + QString::number(sheet - 1)
                  + "\">Previous sheet</a>";
    result += "<span>Sheet " + QString::number(sheet + 1) + " of " + QString::number(count)
              + "</span>";
    if (sheet + 1 < count)
        result += "<a rel=\"next\" href=\"#path-" + QString::number(sheet + 1)
                  + "\">Next sheet</a>";
    return result + "</nav>";
}

QString htmlContinuations(
    const QList<Diagram>            &diagrams,
    int                              current,
    const QMap<QString, QList<int>> &producers,
    const QMap<QString, QList<int>> &consumers)
{
    QString rows;
    for (const auto &node : diagrams[current].graph.nodes) {
        const auto  kind = node.data["kind"].toString(), label = node.data["label"].toString();
        const auto  sheets = kind == "input"    ? producers.value(label)
                             : kind == "output" ? consumers.value(label)
                                                : QList<int>();
        QStringList links;
        for (const int sheet : sheets) {
            if (sheet != current)
                links.append(htmlSheetLink(diagrams, sheet));
        }
        if (!links.isEmpty())
            rows += "<tr><td>" + escape(label) + "</td><td>" + (kind == "input" ? "From" : "To")
                    + "</td><td>" + links.join(", ") + "</td></tr>";
    }
    if (rows.isEmpty())
        return {};
    return "<details class=\"signal-continuations\"><summary>Signal continuations"
           "</summary><table><thead><tr><th>Signal</th><th>Direction</th><th>Circuit "
           "sheets</th></tr></thead><tbody>"
           + rows + "</tbody></table></details>";
}

QString htmlPaths(
    const QList<Diagram> &diagrams,
    const QStringList    &families,
    const QString        &pdfName,
    const QString        &kind)
{
    QString result = "<section id=\"paths\"><h2>2. Circuit sheets</h2><p>" + circuitLegend(kind)
                     + " MUX indices keep their original values. Pin-subset views repeat shared "
                       "downstream processing.</p>";
    QMap<QString, QList<int>> producers, consumers;
    for (int i = 0; i < diagrams.size(); ++i) {
        for (const auto &node : diagrams[i].graph.nodes) {
            const auto nodeKind = node.data["kind"].toString();
            if (nodeKind != "input" && nodeKind != "output")
                continue;
            auto &sheets = nodeKind == "input" ? consumers[node.data["label"].toString()]
                                               : producers[node.data["label"].toString()];
            if (!sheets.contains(i))
                sheets.append(i);
        }
    }
    for (int i = 0; i < diagrams.size(); ++i) {
        const auto &diagram = diagrams[i];
        const auto  title   = "2." + QString::number(i + 1) + " " + diagram.title;
        result
            += "<article class=\"sheet\" id=\"path-" + QString::number(i) + "\"><h3>"
               + escape(title)
               + "</h3><figure><div class=\"diagram\" tabindex=\"0\" role=\"region\" aria-label=\""
               + escape(title) + "\" style=\"--drawing-width:"
               + QString::number(qCeil(diagram.graph.size.width()) + 32) + "px\">"
               + diagramSvg(diagram.graph, families, title) + "</div><figcaption>PDF page "
               + QString::number(diagram.page) + " / <a href=\""
               + QString::fromLatin1(QUrl::toPercentEncoding(pdfName))
               + "#page=" + QString::number(diagram.page)
               + "\" target=\"_blank\" rel=\"noopener\">Open PDF</a></figcaption></figure>"
               + htmlSheetNavigation(i, diagrams.size())
               + htmlContinuations(diagrams, i, producers, consumers)
               + htmlConnections(diagram.graph) + "</article>";
    }
    return result + "</section>";
}

QJsonArray sourceSpecifications(const QJsonObject &data)
{
    const auto    graph = data["graph"].toObject();
    const auto    nodes = graph["nodes"].toArray();
    QSet<QString> used;
    for (const auto &value : graph["edges"].toArray()) {
        const int from = value.toObject()["from"].toInt(-1);
        if (from >= 0 && from < nodes.size())
            used.insert(nodes[from].toObject()["label"].toString());
    }
    QJsonArray rows;
    for (const auto &value : data["sources"].toArray()) {
        const auto source = value.toObject();
        const auto signal = source["signal"].toString();
        rows.append(
            QJsonObject{
                {"name", signal + (used.contains(signal) ? QString() : " (unused)")},
                {"value", source["note"]}});
    }
    return rows;
}

QString htmlOverview(const QJsonObject &data, const QList<Diagram> &diagrams)
{
    QString    result  = "<section id=\"overview\"><h2>1. Sources and targets</h2>";
    const auto sources = sourceSpecifications(data);
    if (!sources.isEmpty()) {
        result += "<details><summary>Source "
                  "specifications</summary><table><thead><tr><th>Source</th><th>Specification</"
                  "th></tr></thead><tbody>";
        for (const auto &value : sources) {
            const auto source = value.toObject();
            result += "<tr><td>" + escape(source["name"].toString()) + "</td><td>"
                      + escape(source["value"].toString()) + "</td></tr>";
        }
        result += "</tbody></table></details>";
    }
    return result + htmlIndex(data["panels"].toArray(), diagrams) + "</section>";
}

QString htmlReport(
    const QJsonObject    &data,
    const QList<Diagram> &diagrams,
    const QStringList    &families,
    const QString        &outputBase)
{
    const auto    panels = data["panels"].toArray();
    const QString name = data["name"].toString(), kind = data["kind"].toString();
    const auto    latin   = resource(":/primitive-report/latin.woff2"),
                  cjk     = resource(":/primitive-report/cjk.woff2");
    const auto    css     = resource(":/primitive-report/report.css"),
                  license = resource(":/license/droid-fonts.txt");
    if (latin.isEmpty() || cjk.isEmpty() || css.isEmpty() || license.isEmpty())
        return {};
    const QByteArray navigationScript = R"JS(document.addEventListener('click', function(event) {
    if (event.defaultPrevented || event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey)
        return;
    const link = event.target instanceof Element ? event.target.closest('a[href^="#"]') : null;
    if (!link)
        return;
    const target = document.getElementById(link.getAttribute('href').slice(1));
    if (!target)
        return;
    event.preventDefault();
    for (let parent = target.parentElement; parent; parent = parent.parentElement) {
        if (parent.tagName === 'DETAILS')
            parent.open = true;
    }
    target.scrollIntoView({block: 'start'});
});)JS";
    const auto       scriptHash
        = QCryptographicHash::hash(navigationScript, QCryptographicHash::Sha256).toBase64();
    const QString pdfName = QFileInfo(outputBase).fileName() + ".pdf";
    const QString html
        = "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src "
          "'unsafe-inline'; script-src 'sha256-"
          + QString::fromLatin1(scriptHash)
          + "'; font-src data:; img-src data:; base-uri 'none'; form-action 'none'\">"
            "<title>"
          + escape(name + " / " + kind)
          + "</title><style>"
            "@font-face{font-family:'Droid Sans Mono';src:url(data:font/woff2;base64,"
          + QString::fromLatin1(latin.toBase64())
          + ") format('woff2');font-weight:400;font-style:normal;font-display:block;}"
            "@font-face{font-family:'Droid Sans Fallback';src:url(data:font/woff2;base64,"
          + QString::fromLatin1(cjk.toBase64())
          + ") format('woff2');font-weight:400;font-style:normal;font-display:block;}"
          + QString::fromUtf8(css)
          + "</style><main><header><img alt=\"QSoC\" src=\"data:image/svg+xml;base64,"
          + QString::fromLatin1(resource(":/primitive-report/logo.svg").toBase64())
          + "\"><div class=\"identity\"><h1>" + escape(name) + "</h1><p>" + escape(kind)
          + " controller</p></div></header><nav><a href=\"#overview\">Overview</a><a "
            "href=\"#paths\">Signal paths</a><a href=\"#parameters\">Parameters</a>"
            "<a href=\""
          + QString::fromLatin1(QUrl::toPercentEncoding(pdfName))
          + "\" target=\"_blank\" rel=\"noopener\">Vector PDF</a></nav>"
          + htmlOverview(data, diagrams) + htmlPaths(diagrams, families, pdfName, kind)
          + "<section id=\"parameters\"><h2>3. Parameters</h2>" + htmlParameters(panels)
          + "</section><footer>QSoC v" + QSOC_VERSION
          + "<details><summary>Report style: Apache-2.0</summary><pre>"
          + escape(QString::fromUtf8(resource(":/license/apache-2.0.txt")))
          + "</pre></details>"
            "<details><summary>Fonts: Droid Sans Mono and Droid Sans Fallback, "
            "Apache-2.0</summary><pre>"
          + escape(QString::fromUtf8(license)) + "</pre></details></footer></main><script>"
          + QString::fromUtf8(navigationScript) + "</script></html>";
    return html;
}
} // namespace

bool renderPrimitiveReport(const QString &request, const QString &outputBase, QString *error)
{
    const auto fail = [&](const QString &message) {
        *error = message;
        return false;
    };
    QFile source(request);
    if (!source.open(QIODevice::ReadOnly) || source.size() > 32 * 1024 * 1024)
        return fail("Cannot read primitive report request");
    QJsonParseError parseError;
    const auto      data = QJsonDocument::fromJson(source.readAll(), &parseError).object();
    const QString   name = data["name"].toString(), kind = data["kind"].toString();
    const auto      panels = data["panels"].toArray();
    if (parseError.error != QJsonParseError::NoError || data["version"].toInt() != 1
        || name.isEmpty() || !QStringList{"Clock", "Reset", "Power"}.contains(kind)
        || panels.isEmpty())
        return fail("Invalid primitive report request");
    QStringList families;
    for (const QString &path :
         {QString(":/primitive-report/latin.ttf"), QString(":/primitive-report/cjk.ttf")}) {
        const int  id     = QFontDatabase::addApplicationFont(path);
        const auto loaded = QFontDatabase::applicationFontFamilies(id);
        if (loaded.isEmpty())
            return fail("Missing report font resource");
        families.append(loaded.first());
    }
    const auto license = resource(":/license/droid-fonts.txt");
    if (license.isEmpty())
        return fail("Missing report font license resource");
    auto diagrams = circuits(panels, families);
    if (diagrams.isEmpty())
        return fail("The report contains no circuit sheets");
    for (const auto &diagram : diagrams) {
        if (!diagram.graph.error.isEmpty())
            return fail(diagram.graph.error);
        if (diagram.graph.size.width() > 976 || diagram.graph.size.height() > 450)
            return fail("A circuit sheet exceeds the printable area: " + diagram.title);
    }
    const int   indexCount = (panels.size() + 17) / 18;
    const auto  paths      = circuitPages(diagrams, indexCount);
    QList<Page> pages      = indexPages(panels, diagrams, families);
    pages.append(paths);
    auto       specifications = parameterSections(panels);
    const auto sources        = sourceSpecifications(data);
    if (!sources.isEmpty())
        specifications.append(QJsonObject{{"title", "Source specifications"}, {"details", sources}});
    pages.append(parameters(specifications, families));
    const auto html = htmlReport(data, diagrams, families, outputBase);
    if (html.isEmpty())
        return fail("Missing report font, style or license resource");
    QSaveFile pdf(outputBase + ".pdf");
    if (!pdf.open(QIODevice::WriteOnly))
        return fail(pdf.errorString());
    {
        QPdfWriter writer(&pdf);
        writer.setTitle(name + " / " + kind);
        writer.setCreator("QSoC");
        writer.setResolution(96);
        writer.addFileAttachment("fonts-LICENSE.txt", license, "text/plain");
        writer.setPageLayout(paper());
        QPainter painter(&writer);
        if (!painter.isActive())
            return fail("Cannot initialize PDF painter");
        for (qsizetype i = 0; i < pages.size(); ++i) {
            if (i > 0) {
                writer.setPageLayout(paper());
                if (!writer.newPage())
                    return fail("Cannot create PDF page");
            }
            painter.setWindow(QRect(QPoint(), pages[i].size));
            paint(painter, pages[i], families, name, kind, i + 1, pages.size());
        }
    }
    QSaveFile  output(outputBase + ".html");
    const auto bytes = html.toUtf8();
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()
        || !output.commit())
        return fail(output.errorString());
    if (!pdf.commit())
        return fail(pdf.errorString());
    return true;
}
