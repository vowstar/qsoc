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
    qreal                    scale      = 1;
    qreal                    frameWidth = 976;
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

QStringList wrapSignal(const QString &value, const QFont &face, qreal available)
{
    QStringList         result;
    QString             current;
    const QFontMetricsF metrics(face);
    const auto          parts = value.split('_');
    for (int i = 0; i < parts.size(); ++i) {
        const QString part = parts[i] + (i + 1 < parts.size() ? "_" : "");
        if (!current.isEmpty() && metrics.horizontalAdvance(current + part) > available) {
            result.append(current);
            current.clear();
        }
        const auto lines = wrap(part, face, available);
        if (lines.size() > 1) {
            result.append(lines.mid(0, lines.size() - 1));
            current = lines.last();
        } else {
            current += part;
        }
    }
    if (!current.isEmpty())
        result.append(current);
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
            const qreal maximum = graph["power"].toBool() && kind == "output" ? 192 : 160;
            const qreal width   = qBound<qreal>(
                96, QFontMetricsF(face).horizontalAdvance(node["label"].toString()) + 28, maximum);
            const int  lines = wrap(node["label"].toString(), face, width - 28, 2).size();
            const bool note  = !node["note"].toString().isEmpty() || node["reference"].toBool();
            sizes.append(
                {width, qMax<qreal>(32, lines * leading(face) + (note ? leading(small) : 0) + 8)});
        } else if (kind == "overview") {
            const qreal width = qBound<qreal>(
                244, QFontMetricsF(face).horizontalAdvance(node["label"].toString()) + 84, 400);
            const int lines = wrap(node["label"].toString(), face, width - 80, 2).size();
            sizes.append(
                {width,
                 qMax<qreal>(
                     qMax<qreal>(44, node["inputs"].toInt(1) * 10 + 8),
                     2 * (lines * leading(face) + 2))});
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
    if (kind == "overview") {
        const QString shape = data["shape"].toString();
        const QRectF  glyph(box.left() + 24, box.top() + 4, 32, box.height() - 8);
        const auto    inversions = data["inputInversions"].toArray();
        const auto    styles     = data["styles"].toArray();
        for (int i = 0; i < data["inputs"].toInt(1); ++i) {
            const auto pin = QSocReportLayout::port(node, true, i);
            QPen       wire(ink, 1.3);
            if (i < styles.size() && (styles[i] == "soft" || styles[i] == "test"))
                wire.setStyle(Qt::DashLine);
            painter.setPen(wire);
            painter.drawLine(pin, QPointF(glyph.left(), pin.y()));
            if (i < inversions.size() && inversions[i].toBool()) {
                painter.setPen(QPen(ink, 1.3));
                painter.drawEllipse(QPointF(glyph.left() - 3, pin.y()), 3, 3);
            }
        }
        painter.setPen(QPen(ink, 1.3));
        if (shape == "mux") {
            painter.drawPolygon(
                QPolygonF{
                    glyph.topLeft(),
                    {glyph.right(), glyph.center().y() - 6},
                    {glyph.right(), glyph.center().y() + 6},
                    glyph.bottomLeft()});
        } else if (shape == "and") {
            QPainterPath path;
            path.moveTo(glyph.topLeft());
            path.lineTo(glyph.center().x(), glyph.top());
            path.arcTo(glyph, 90, -180);
            path.lineTo(glyph.bottomLeft());
            path.closeSubpath();
            painter.drawPath(path);
        } else if (shape == "power") {
            painter.drawRect(glyph.adjusted(-8, 0, 8, 0));
            text(painter, glyph.adjusted(-8, 0, 8, 0), {"PWR"}, small);
        } else {
            painter.drawLine(
                QPointF(glyph.left(), box.center().y()), QPointF(glyph.right(), box.center().y()));
        }
        const QPointF output = QSocReportLayout::port(node, false, 0);
        painter.drawLine(QPointF(glyph.right(), output.y()), output);
        if (data["outputInverted"].toBool())
            painter.drawEllipse(QPointF(glyph.right() + 3, output.y()), 3, 3);
        text(
            painter,
            {box.left() + 76, box.top(), box.width() - 80, box.height() / 2 - 2},
            wrap(label, face, box.width() - 80, 2),
            face,
            Qt::AlignLeft | Qt::AlignTop);
        if (!note.isEmpty())
            text(
                painter,
                {box.left() + 76, box.center().y() + 4, box.width() - 80, leading(small)},
                wrap(note, small, box.width() - 80, 1),
                small,
                Qt::AlignLeft | Qt::AlignTop);
        return;
    }
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
        const QFont labelFace = data["continuation"].toBool() ? small : face;
        const qreal available = box.width() - 16;
        auto        lines     = wrapSignal(label, labelFace, available);
        if (lines.size() > 2)
            lines = {QFontMetricsF(labelFace).elidedText(label, Qt::ElideMiddle, available)};
        QString   displayNote          = note;
        const int destinationPageCount = data["destinationPageCount"].toInt();
        if (!input && destinationPageCount > 1
            && QFontMetricsF(small).horizontalAdvance(note) > available)
            displayNote = QString("%1 pages").arg(destinationPageCount);
        const auto  notes       = displayNote.isEmpty() ? QStringList()
                                                        : wrap(displayNote, small, available, 1);
        const qreal labelHeight = lines.size() * leading(labelFace);
        const qreal top  = box.center().y() - (labelHeight + notes.size() * leading(small)) / 2;
        const qreal left = box.left() + (input ? 0 : 16);
        text(
            painter,
            {left, top, available, labelHeight},
            lines,
            labelFace,
            Qt::AlignTop | (input ? Qt::AlignRight : Qt::AlignLeft));
        text(
            painter,
            {left, top + labelHeight, available, notes.size() * leading(small)},
            notes,
            small,
            Qt::AlignTop | (input ? Qt::AlignRight : Qt::AlignLeft),
            muted);
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
    QList<QPainterPath> arcs;
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
            arcs.append(arc);
        }
    }
    painter.setPen(QPen(ink, 1.3));
    for (const auto &arc : arcs)
        painter.drawPath(arc);
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

QList<Diagram> circuits(
    const QJsonArray &panels, const QStringList &families, QSocReportLayout::Budget &budget)
{
    QList<Diagram>           result;
    QSocReportLayout::Engine engine;
    QMap<QString, QString>   labels;
    for (const auto &value : panels) {
        const auto panel = value.toObject();
        labels.insert(
            panel["identity"].toString(panel["title"].toString()), panel["output"].toString());
    }
    std::function<bool(const QJsonArray &, const QList<int> &, const QString &, bool)> append;
    append = [&](const QJsonArray &members,
                 const QList<int> &owners,
                 const QString    &caption,
                 bool              fitOnly) {
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
        const auto sizes = measure(graph, families);
        if (fitOnly
            && (nodes.size() > 64 || graph["edges"].toArray().size() > 128
                || std::any_of(sizes.cbegin(), sizes.cend(), [](const QSizeF &size) {
                       return size.width() > 1110 || size.height() > 527;
                   })))
            return false;
        const auto drawing = QSocReportLayout::layout(graph, sizes, &engine, &budget);
        const bool fits    = drawing.size.width() <= 1110 && drawing.size.height() <= 527;
        if (fitOnly && (!drawing.error.isEmpty() || !fits))
            return false;
        if (drawing.error.isEmpty() && !fits && members.size() > 1) {
            const int middle = members.size() / 2;
            append(slice(members, 0, middle), owners.mid(0, middle), caption, false);
            append(slice(members, middle, members.size()), owners.mid(middle), caption, false);
            return true;
        }
        const auto sheets = fits || !drawing.error.isEmpty()
                                ? QList<QSocReportLayout::Result>{drawing}
                                : QSocReportLayout::paginate(
                                      graph, sizes, engine, result.size() + 1, &drawing, &budget);
        for (int i = 0; i < sheets.size(); ++i)
            result.append(
                {names.join(" / ") + caption
                     + (sheets.size() > 1
                            ? QString(" / sheet %1 of %2").arg(i + 1).arg(sheets.size())
                            : QString()),
                 owners,
                 0,
                 sheets[i]});
        return true;
    };
    QList<QList<int>>     groups;
    QMap<QByteArray, int> current;
    for (int index = 0; index < panels.size(); ++index) {
        const auto panel    = panels[index].toObject();
        const auto branches = panel["branches"].toArray();
        if (branches.size() > 4 || panel["exits"].toArray().size() > 4
            || panel["followers"].toArray().size() > 4) {
            if (append({panel}, {index}, {}, true))
                continue;
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
                                        : QString(),
                    false);
            }
            for (int start = 4; start < panel["exits"].toArray().size(); start += 4) {
                auto part         = panel;
                part["branches"]  = slice(branches, 0, 4);
                part["followers"] = QJsonArray();
                part["exits"]     = slice(panel["exits"].toArray(), start, 4);
                append({part}, {index}, " / remaining outputs", false);
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
                        .arg(qMin(start + 4, int(panel["followers"].toArray().size()))),
                    false);
            }
            continue;
        }
        QJsonArray signature;
        for (const auto &branch : branches)
            signature.append(branch.toObject()["source"]);
        signature.append(panel["merge"].toObject()["kind"]);
        const auto key     = QJsonDocument(signature).toJson(QJsonDocument::Compact);
        const int  maximum = panel["merge"].toObject()["kind"] == "mux" ? 4 : 3;
        if (!current.contains(key) || groups[current.value(key)].size() >= maximum) {
            current.insert(key, groups.size());
            groups.append(QList<int>());
        }
        groups[current.value(key)].append(index);
    }
    for (const auto &group : groups) {
        QJsonArray members;
        for (const int index : group)
            members.append(panels[index]);
        append(members, group, {}, false);
    }
    std::stable_sort(result.begin(), result.end(), [](const Diagram &a, const Diagram &b) {
        return a.panels.first() < b.panels.first();
    });
    return result;
}

QList<Diagram> connectivity(
    const QJsonObject &data, const QStringList &families, QSocReportLayout::Budget &budget)
{
    const auto panels = data["panels"].toArray();
    const bool wide   = std::any_of(panels.begin(), panels.end(), [](const QJsonValue &value) {
        return value.toObject()["branches"].toArray().size() > 8;
    });
    if (panels.size() < 8 && !wide)
        return {};
    QJsonArray         nodes, edges;
    QMap<QString, int> drivers;
    QSet<QString>      identities;
    for (const auto &value : panels) {
        const auto panel = value.toObject();
        identities.insert(panel["identity"].toString(panel["title"].toString()));
    }
    const bool reset = data["kind"] == "Reset";
    for (int owner = 0; owner < panels.size(); ++owner) {
        const auto panel = panels[owner].toObject(), merge = panel["merge"].toObject();
        const auto branches = panel["branches"].toArray();
        QJsonArray sourceNodes;
        for (const auto &value : branches)
            sourceNodes.append(
                QJsonObject{{"kind", "input"}, {"label", value.toObject()["source"]}});
        QJsonObject sourceGraph{{"nodes", sourceNodes}};
        const auto  sourceSizes = measure(sourceGraph, families);
        for (int start = 0; start < branches.size();) {
            int   count  = 0;
            qreal height = 12;
            while (start + count < branches.size()) {
                const auto  source = branches[start + count].toObject()["source"].toString();
                const qreal next   = (identities.contains(source)
                                          ? QSocReportLayout::continuationPortSize.height()
                                          : sourceSizes[start + count].height())
                                     + (count ? 6 : 0);
                if (count && height + next > 448)
                    break;
                height += next;
                ++count;
            }
            QJsonArray sources, styles, inversions;
            for (const auto &value : slice(branches, start, count)) {
                const auto branch = value.toObject();
                sources.append(branch["source"]);
                styles.append(branch["style"]);
                const auto steps = branch["steps"].toArray();
                inversions.append(
                    reset && !steps.isEmpty() && steps.first().toObject()["kind"] == "inverter");
            }
            const QString output = panel["output"].toString();
            const bool    test   = !merge["testClock"].toString().isEmpty();
            const QString subset
                = count < branches.size()
                      ? QString(" / inputs %1-%2").arg(start).arg(start + sources.size() - 1)
                      : QString();
            const int main = nodes.size();
            nodes.append(
                QJsonObject{
                    {"kind", "overview"},
                    {"shape", merge["kind"]},
                    {"label", output + subset},
                    {"note", merge["kind"] == "power" ? panel["subtitle"] : merge["label"]},
                    {"signal", test ? QString() : output},
                    {"owner", owner},
                    {"inputs", int(sources.size())},
                    {"sources", sources},
                    {"styles", styles},
                    {"inputInversions", inversions}});
            int final = main;
            if (test) {
                final = nodes.size();
                nodes.append(
                    QJsonObject{
                        {"kind", "overview"},
                        {"shape", "mux"},
                        {"label", output + subset},
                        {"note", "TEST"},
                        {"signal", output},
                        {"owner", owner},
                        {"inputs", 2},
                        {"sources", QJsonArray{QJsonValue(), merge["testClock"]}},
                        {"styles", QJsonArray{QJsonValue(), "test"}}});
                edges.append(QJsonObject{{"from", main}, {"to", final}, {"input", 0}});
            }
            const auto tail = panel["tail"].toArray();
            if (reset && !tail.isEmpty() && tail.last().toObject()["kind"] == "inverter") {
                auto node              = nodes[final].toObject();
                node["outputInverted"] = true;
                nodes[final]           = node;
            }
            const QString identity = panel["identity"].toString(panel["title"].toString());
            if (!drivers.contains(identity))
                drivers.insert(identity, final);
            start += count;
        }
    }
    const int targets = nodes.size();
    for (int target = 0; target < targets; ++target) {
        const auto node    = nodes[target].toObject();
        const auto sources = node["sources"].toArray(), styles = node["styles"].toArray();
        for (int input = 0; input < sources.size(); ++input) {
            if (!sources[input].isString())
                continue;
            const QString source = sources[input].toString();
            if (!drivers.contains(source)) {
                drivers.insert(source, nodes.size());
                nodes.append(QJsonObject{{"kind", "input"}, {"label", source}});
            }
            edges.append(
                QJsonObject{
                    {"from", drivers.value(source)},
                    {"to", target},
                    {"input", input},
                    {"style", styles[input]}});
        }
    }
    if (nodes.size() > 4096 || edges.size() > 8192) {
        const auto polarity = [reset](const QJsonArray &steps) {
            QJsonArray result;
            for (const auto &step : steps) {
                if (reset && step.toObject()["kind"] == "inverter")
                    result.append(step);
            }
            return result;
        };
        QJsonArray parts;
        for (const auto &value : panels) {
            auto panel    = value.toObject();
            auto branches = panel["branches"].toArray();
            for (int i = 0; i < branches.size(); ++i) {
                auto branch     = branches[i].toObject();
                branch["steps"] = polarity(branch["steps"].toArray());
                branches[i]     = branch;
            }
            auto merge         = panel["merge"].toObject();
            merge["guide"]     = false;
            panel["merge"]     = merge;
            panel["branches"]  = branches;
            panel["tail"]      = polarity(panel["tail"].toArray());
            panel["exits"]     = QJsonArray();
            panel["followers"] = QJsonArray();
            parts.append(panel);
        }
        return circuits(parts, families, budget);
    }
    QList<QList<int>> outgoing(nodes.size());
    QList<int>        degree(nodes.size()), depth(nodes.size()), queue;
    for (const auto &value : edges) {
        const auto edge = value.toObject();
        outgoing[edge["from"].toInt()].append(edge["to"].toInt());
        ++degree[edge["to"].toInt()];
    }
    for (int i = 0; i < nodes.size(); ++i) {
        if (degree[i] == 0)
            queue.append(i);
    }
    for (int i = 0; i < queue.size(); ++i) {
        for (const int next : outgoing[queue[i]]) {
            depth[next] = qMax(depth[next], depth[queue[i]] + 1);
            if (--degree[next] == 0)
                queue.append(next);
        }
    }
    QMap<int, int> ownerDepth;
    for (int i = 0; i < targets; ++i) {
        const int owner   = nodes[i].toObject()["owner"].toInt();
        ownerDepth[owner] = ownerDepth.contains(owner) ? qMin(ownerDepth[owner], depth[i])
                                                       : depth[i];
    }
    QJsonObject graph{
        {"nodes", nodes}, {"edges", edges}, {"overview", true}, {"power", data["kind"] == "Power"}};
    const auto               sizes = measure(graph, families);
    QSocReportLayout::Engine engine;
    const auto     drawings = QSocReportLayout::paginate(graph, sizes, engine, 0, nullptr, &budget);
    QList<Diagram> result;
    for (const auto &drawing : drawings) {
        QList<int>  owners;
        QStringList names;
        for (const auto &node : drawing.nodes) {
            const int owner = node.data["owner"].toInt(-1);
            if (owner >= 0 && !owners.contains(owner)) {
                owners.append(owner);
                if (names.size() < 8)
                    names.append(panels[owner].toObject()["title"].toString());
            }
        }
        const QString more = owners.size() > names.size()
                                 ? QString(" / +%1").arg(owners.size() - names.size())
                                 : QString();
        result.append(
            {QString("%1 targets: ").arg(owners.size()) + names.join(" / ") + more,
             owners,
             0,
             drawing});
    }
    const auto level = [&](const Diagram &diagram) {
        int value = nodes.size();
        for (const int owner : diagram.panels)
            value = qMin(value, ownerDepth.value(owner));
        return value;
    };
    std::stable_sort(result.begin(), result.end(), [&](const Diagram &a, const Diagram &b) {
        return level(a) < level(b);
    });
    return result;
}

QList<Page> circuitPages(
    QList<Diagram> &diagrams, int offset, const QString &title = "2. Signal paths")
{
    QList<Page> pages;
    Page        page;
    page.title = title;
    qreal x = 32, y = 134, rowHeight = 0;
    for (int i = 0; i < diagrams.size(); ++i) {
        auto &diagram = diagrams[i];
        diagram.scale = qMin<qreal>(
            1, qMin(944 / diagram.graph.size.width(), 448 / diagram.graph.size.height()));
        if (diagram.graph.size.height() * diagram.scale + 28 > 254
            && diagram.graph.size.height() * .85 + 28 <= 254)
            diagram.scale = .85;
        const qreal height = diagram.graph.size.height() * diagram.scale + 28;
        diagram.frameWidth
            = qMin<qreal>(976, qMax<qreal>(244, diagram.graph.size.width() * diagram.scale + 32));
        if (x + diagram.frameWidth > 1008) {
            x = 32;
            y += rowHeight + 8;
            rowHeight = 0;
        }
        if (y + height > 650 && !page.diagrams.isEmpty()) {
            pages.append(page);
            page       = Page();
            page.title = title;
            y          = 134;
            x          = 32;
            rowHeight  = 0;
        }
        diagram.page = offset + pages.size() + 1;
        page.diagrams.append({diagram, QPointF(x, y)});
        x += diagram.frameWidth + 8;
        rowHeight = qMax(rowHeight, height);
    }
    if (!page.diagrams.isEmpty())
        pages.append(page);
    QMap<QString, int> references;
    for (const auto &diagram : diagrams) {
        for (const auto &node : diagram.graph.nodes) {
            const auto kind  = node.data["kind"].toString();
            const auto label = kind == "overview" ? node.data["signal"].toString()
                                                  : node.data["label"].toString();
            if (!label.isEmpty() && (kind == "output" || kind == "net" || kind == "overview")
                && !references.contains(label))
                references.insert(label, diagram.page);
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
        const auto range = diagram.panels.isEmpty() ? QPair<int, int>()
                                                    : ranges.value(diagram.panels.first());
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
                node.data["note"]                 = "to p. " + targets.join(",");
                node.data["destinationPageCount"] = targets.size();
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
    const bool large = panels.size() >= 8;
    const bool templateTargets
        = large && std::none_of(panels.begin(), panels.end(), [](const auto &value) {
              const auto rows = value.toObject()["details"].toArray();
              return std::any_of(rows.begin(), rows.end(), [](const auto &row) {
                  return row.toObject()["value"].toString().contains("{target}");
              });
          });
    const auto normalized = [templateTargets](QJsonObject row, const QString &target) {
        const QString value = row["value"].toString();
        if (templateTargets && !target.isEmpty()) {
            QString pattern = value;
            pattern.replace(target, "{target}");
            QString expanded = pattern;
            expanded.replace("{target}", target);
            if (expanded == value)
                row["value"] = pattern;
        }
        return row;
    };
    QList<QJsonArray> candidates;
    for (const auto &value : panels) {
        const auto panel = value.toObject();
        QJsonArray rows;
        for (const auto &row : panel["details"].toArray())
            rows.append(normalized(row.toObject(), panel["title"].toString()));
        candidates.append(rows);
    }
    QJsonArray                 shared;
    QMap<QString, QJsonObject> defaults;
    QSet<QString>              seen;
    bool                       templated = false, overrides = false;
    const auto                 first = panels.first().toObject()["details"].toArray();
    for (const auto &value : first) {
        const QString name = value.toObject()["name"].toString();
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen.insert(name);
        QMap<QString, int> votes;
        for (const auto &rows : candidates) {
            const auto matches = [&](const QJsonValue &row) {
                return row.toObject()["name"] == name;
            };
            if (std::count_if(rows.begin(), rows.end(), matches) != 1)
                break;
            const auto row = *std::find_if(rows.begin(), rows.end(), matches);
            ++votes[row.toObject()["value"].toString()];
        }
        int voters = 0;
        for (const int count : votes)
            voters += count;
        if (voters != panels.size())
            continue;
        const int required = large ? panels.size() / 2 + 1 : panels.size();
        for (auto vote = votes.cbegin(); vote != votes.cend(); ++vote) {
            if (vote.value() < required)
                continue;
            const QJsonObject row{{"name", name}, {"value", vote.key()}};
            defaults.insert(name, row);
            shared.append(row);
            templated |= templateTargets && vote.key().contains("{target}");
            overrides |= vote.value() != panels.size();
        }
    }
    if (shared.isEmpty())
        return panels;
    for (int i = 0; i < panels.size(); ++i) {
        auto       panel = panels[i].toObject();
        QJsonArray rows;
        for (const auto &value : panel["details"].toArray()) {
            const auto    row  = value.toObject();
            const QString name = row["name"].toString();
            if (!defaults.contains(name)
                || normalized(row, panel["title"].toString()) != defaults.value(name))
                rows.append(row);
        }
        panel["details"] = rows;
        panels[i]        = panel;
    }
    if (templated)
        shared.prepend(
            QJsonObject{
                {"name", "Target substitution"},
                {"value", "{target} is the complete name in the Target column of this table."}});
    if (overrides)
        shared.prepend(
            QJsonObject{
                {"name", "Override rule"}, {"value", "Per-target rows override these defaults."}});
    panels.prepend(
        QJsonObject{
            {"title", overrides ? "Target defaults" : "Shared by all targets"},
            {"details", shared}});
    return panels;
}

QList<Page> compactParameters(const QJsonArray &panels, const QStringList &families)
{
    struct Table
    {
        QStringList        headers;
        QList<qreal>       columns;
        QList<QStringList> rows;
    };
    const QFont           face = font(families, 14);
    const qreal           line = leading(face);
    const QStringList     headings{"Target", "Parameter", "Value"};
    const QList<qreal>    vertical{32, 240, 472, 1008};
    QList<Table>          tables;
    Table                 plain{headings, vertical, {}};
    QList<QJsonArray>     groups;
    QMap<QByteArray, int> schemas;
    for (const auto &value : panels) {
        const auto    panel = value.toObject();
        QJsonArray    schema;
        QSet<QString> names;
        bool          unique = panel.contains("branches") && !panel["details"].toArray().isEmpty();
        for (const auto &entry : panel["details"].toArray()) {
            const auto name = entry.toObject()["name"].toString();
            unique &= !name.isEmpty() && !names.contains(name);
            names.insert(name);
            schema.append(name);
        }
        if (!unique) {
            groups.append(QJsonArray{panel});
            continue;
        }
        const auto key = QJsonDocument(schema).toJson(QJsonDocument::Compact);
        if (!schemas.contains(key)) {
            schemas.insert(key, groups.size());
            groups.append(QJsonArray());
        }
        groups[schemas.value(key)].append(panel);
    }
    const auto rowHeight = [&](const QStringList &row, const QList<qreal> &columns) {
        int lines = 1;
        for (int i = 0; i < row.size(); ++i)
            lines = qMax(lines, int(wrap(row[i], face, columns[i + 1] - columns[i] - 16).size()));
        return lines * line + 8;
    };
    for (const auto &group : groups) {
        const auto first  = group.first().toObject()["details"].toArray();
        const int  stride = group.size() >= 8 ? 3 : 1;
        for (int start = 0; start < qMax<qsizetype>(1, first.size()); start += stride) {
            const int count = qMin(stride, int(first.size()) - start);
            Table     matrix{{"Target"}, {32, 240}, {}};
            for (int i = 0; i < count; ++i) {
                matrix.headers.append(first[start + i].toObject()["name"].toString());
                matrix.columns.append(240 + 768.0 * (i + 1) / count);
            }
            QList<QStringList> rows;
            qreal              matrixHeight = 0, verticalHeight = 0;
            bool               shortRows = true;
            for (const auto &value : group) {
                const auto  panel   = value.toObject();
                const auto  details = panel["details"].toArray();
                QStringList horizontal{panel["title"].toString()};
                if (details.isEmpty())
                    rows.append(
                        {panel["title"].toString(), "Parameters", "No per-target overrides."});
                for (int i = start; i < start + count; ++i) {
                    const auto row = details[i].toObject();
                    rows.append(
                        {panel["title"].toString(),
                         row["name"].toString(),
                         row["value"].toString()});
                    horizontal.append(row["value"].toString());
                }
                const qreal height = count ? rowHeight(horizontal, matrix.columns) : 0;
                shortRows &= height <= 3 * line + 8;
                matrixHeight += height;
                matrix.rows.append(horizontal);
            }
            for (const auto &row : rows)
                verticalHeight += rowHeight(row, vertical);
            const qreal headerHeight = count ? rowHeight(matrix.headers, matrix.columns) : 0;
            matrixHeight += qCeil(matrixHeight / qMax(line, 506 - headerHeight)) * headerHeight;
            if (group.size() < 8 || count < 1 || !shortRows || headerHeight > 2 * line + 8
                || matrixHeight >= verticalHeight) {
                plain.rows.append(rows);
                continue;
            }
            if (!plain.rows.isEmpty()) {
                tables.append(plain);
                plain.rows.clear();
            }
            tables.append(matrix);
        }
    }
    if (!plain.rows.isEmpty())
        tables.append(plain);
    QList<Page>  pages;
    Page         page;
    qreal        y     = 0;
    const Table *table = nullptr;
    const auto   fresh = [&] {
        if (!page.cells.isEmpty())
            pages.append(page);
        page                = Page();
        page.title          = "3. Parameters";
        y                   = 144;
        const auto &columns = table->columns;
        const qreal height  = rowHeight(table->headers, columns);
        for (int i = 0; i < table->headers.size(); ++i) {
            page.cells.append({{columns[i], y, columns[i + 1] - columns[i], height}, true});
            page.labels.append(
                {{columns[i] + 8, y + 4},
                 wrap(table->headers[i], face, columns[i + 1] - columns[i] - 16),
                 14,
                 ink});
        }
        y += height;
    };
    for (const auto &current : tables) {
        table = &current;
        fresh();
        const auto &columns = table->columns;
        for (const auto &row : table->rows) {
            QList<QStringList> texts;
            for (int i = 0; i < row.size(); ++i)
                texts.append(wrap(row[i], face, columns[i + 1] - columns[i] - 16));
            while (std::any_of(texts.cbegin(), texts.cend(), [](const QStringList &text) {
                return !text.isEmpty();
            })) {
                int capacity = static_cast<int>((650 - y - 8) / line);
                int needed   = 0;
                for (const auto &text : texts)
                    needed = qMax(needed, int(text.size()));
                if (capacity < 1 || (needed > capacity && needed <= 16)) {
                    fresh();
                    capacity = static_cast<int>((650 - y - 8) / line);
                }
                const int   count  = qMin(capacity, needed);
                const qreal height = count * line + 8;
                for (int i = 0; i < texts.size(); ++i) {
                    page.cells.append({{columns[i], y, columns[i + 1] - columns[i], height}, false});
                    page.labels.append({{columns[i] + 8, y + 4}, texts[i].mid(0, count), 14, ink});
                    texts[i] = texts[i].mid(qMin<qsizetype>(count, texts[i].size()));
                }
                y += height;
            }
        }
    }
    if (!page.cells.isEmpty())
        pages.append(page);
    return pages;
}

QList<Page> parameters(const QJsonArray &panels, const QStringList &families)
{
    if (panels.size() >= 8)
        return compactParameters(panels, families);
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
            {origin, QSizeF(diagram.frameWidth, 20)},
            wrap(diagram.title, font(families, 14), diagram.frameWidth, 1),
            font(families, 14),
            Qt::AlignLeft | Qt::AlignTop);
        QPen frame(border, .8);
        frame.setStyle(Qt::DashLine);
        painter.setPen(frame);
        painter.setBrush(Qt::NoBrush);
        const auto width  = diagram.graph.size.width() * diagram.scale;
        const auto height = diagram.graph.size.height() * diagram.scale;
        painter.drawRect(QRectF(origin.x(), origin.y() + 20, diagram.frameWidth, height + 8));
        painter.save();
        painter.translate(origin.x() + (diagram.frameWidth - width) / 2, origin.y() + 24);
        painter.scale(diagram.scale, diagram.scale);
        drawGraph(painter, diagram.graph, {0, 0}, families);
        painter.restore();
    }
    if (!page.diagrams.isEmpty())
        text(
            painter,
            {32, 654, 976, 22},
            {page.title.contains("Connectivity")
                 ? "Connectivity only. Polarity bubbles remain. Processing and controls are on "
                   "the circuit sheets. Repeated targets show input subsets."
                 : circuitLegend(kind)},
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

QByteArray pageSvg(
    const Page        &page,
    const QStringList &families,
    const QString     &name,
    const QString     &kind,
    int                number,
    int                total)
{
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QSvgGenerator generator;
    generator.setOutputDevice(&buffer);
    generator.setTitle(page.title);
    generator.setResolution(96);
    generator.setSize(page.size);
    generator.setViewBox(QRect(QPoint(), page.size));
    {
        QPainter painter(&generator);
        if (!painter.isActive())
            return {};
        paint(painter, page, families, name, kind, number, total);
    }
    const auto start = bytes.indexOf("<svg");
    return start < 0 ? QByteArray() : bytes.mid(start);
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

QString htmlPaths(const QList<Diagram> &diagrams, const QStringList &families, const QString &kind)
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
               + QString::number(diagram.page)
               + " / <a href=\"#document-preview\" data-document-page=\""
               + QString::number(diagram.page) + "\">Preview page</a></figcaption></figure>"
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

QString htmlOverview(
    const QJsonObject    &data,
    const QList<Diagram> &diagrams,
    const QList<Diagram> &overviews,
    const QStringList    &families)
{
    QString result = "<section id=\"overview\"><h2>1. Sources and targets</h2>";
    if (!overviews.isEmpty()) {
        result += "<h3>Connectivity overview</h3><p>These views retain source nets and target "
                  "dependencies. Polarity bubbles remain. Processing and control pins are on "
                  "the detailed circuit sheets. Repeated targets show input subsets.</p>";
        for (int i = 0; i < overviews.size(); ++i) {
            const auto &diagram = overviews[i];
            const auto  title   = QString("Connectivity %1 / ").arg(i + 1) + diagram.title;
            result += "<details" + QString(i == 0 ? " open" : "") + "><summary>" + escape(title)
                      + "</summary><figure><div class=\"diagram\" "
                        "tabindex=\"0\" role=\"region\" aria-label=\""
                      + escape(title) + "\" style=\"--drawing-width:"
                      + QString::number(qCeil(diagram.graph.size.width()) + 32) + "px\">"
                      + diagramSvg(diagram.graph, families, title) + "</div><figcaption>PDF page "
                      + QString::number(diagram.page)
                      + " / <a href=\"#paths\">Detailed circuits</a></figcaption></figure>";
            result += htmlConnections(diagram.graph) + "</details>";
        }
    }
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

QString htmlPagePreview(
    const QList<Page> &pages, const QStringList &families, const QString &name, const QString &kind)
{
    const QString unavailable
        = "<details id=\"document-preview\" class=\"document-preview\">"
          "<summary>Document page preview unavailable</summary>"
          "<p role=\"status\">The preview could not be prepared within its size limits. "
          "The PDF and static circuit sheets remain available.</p></details>";
    QJsonArray compressed;
    qsizetype  total = 0;
    for (qsizetype i = 0; i < pages.size(); ++i) {
        const auto svg = pageSvg(pages[i], families, name, kind, i + 1, pages.size());
        if (svg.isEmpty() || svg.size() > 4 * 1024 * 1024)
            return unavailable;
        const auto bytes = qCompress(svg).toBase64();
        total += bytes.size();
        if (bytes.isEmpty() || total > 64 * 1024 * 1024)
            return unavailable;
        compressed.append(QString::fromLatin1(bytes));
    }
    return QString(
               "<details id=\"document-preview\" class=\"document-preview\">"
               "<summary>Complete document preview (%1 pages)</summary>"
               "<p>All pages use the PDF page layout, including its index and parameters.</p>"
               "<div class=\"document-toolbar\">"
               "<button type=\"button\" data-page-step=\"-1\" disabled>Previous</button>"
               "<label>Page <input type=\"number\" min=\"1\" max=\"%1\" value=\"1\" "
               "disabled></label>"
               "<span>of %1</span>"
               "<button type=\"button\" data-page-step=\"1\" disabled>Next</button>"
               "<label>Zoom <select disabled><option value=\"fit\">Fit width</option>"
               "<option value=\"1\">100%</option><option value=\"1.25\">125%</option>"
               "<option value=\"1.5\">150%</option><option value=\"2\">200%</option>"
               "</select></label></div>"
               "<p role=\"status\">Enable JavaScript to use the paginated preview. "
               "Circuit sheets below remain available.</p>"
               "<div class=\"document-page fit\" tabindex=\"0\" role=\"region\" "
               "aria-label=\"Document page viewport\"></div>"
               "<script type=\"application/json\" id=\"document-page-data\">%2</script>"
               "</details>")
        .arg(pages.size())
        .arg(QString::fromUtf8(QJsonDocument(compressed).toJson(QJsonDocument::Compact)));
}

QString htmlReport(
    const QJsonObject    &data,
    const QList<Diagram> &diagrams,
    const QList<Page>    &pages,
    const QStringList    &families,
    const QString        &outputBase,
    const QList<Diagram> &overviews)
{
    const auto    panels = data["panels"].toArray();
    const QString name = data["name"].toString(), kind = data["kind"].toString();
    const auto    latin            = resource(":/primitive-report/latin.woff2"),
                  cjk              = resource(":/primitive-report/cjk.woff2");
    const auto    css              = resource(":/primitive-report/report.css"),
                  license          = resource(":/license/droid-fonts.txt");
    auto          navigationScript = resource(":/primitive-report/preview.js");
    /* HTML normalizes line endings before checking script hashes. */
    navigationScript.replace("\r\n", "\n");
    navigationScript.replace('\r', '\n');
    const auto preview = htmlPagePreview(pages, families, name, kind);
    if (latin.isEmpty() || cjk.isEmpty() || css.isEmpty() || license.isEmpty()
        || navigationScript.isEmpty())
        return {};
    const auto scriptHash
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
          + " controller</p></div></header><nav><a href=\"#overview\">Overview</a>"
            "<a href=\"#document-preview\" data-document-page=\"1\">Document pages</a><a "
            "href=\"#paths\">Signal paths</a><a href=\"#parameters\">Parameters</a>"
            "<a href=\""
          + QString::fromLatin1(QUrl::toPercentEncoding(pdfName))
          + "\" target=\"_blank\" rel=\"noopener\">Download PDF</a></nav>"
          + htmlOverview(data, diagrams, overviews, families) + preview
          + htmlPaths(diagrams, families, kind)
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
    QSocReportLayout::Budget budget;
    auto                     diagrams = circuits(panels, families, budget);
    if (diagrams.isEmpty())
        return fail("The report contains no circuit sheets");
    for (const auto &diagram : diagrams) {
        if (!diagram.graph.error.isEmpty())
            return fail(diagram.graph.error);
        if (diagram.graph.size.width() > 1110 || diagram.graph.size.height() > 527)
            return fail("A circuit sheet exceeds the printable area: " + diagram.title);
    }
    auto overviews = connectivity(data, families, budget);
    for (const auto &diagram : overviews) {
        if (!diagram.graph.error.isEmpty())
            return fail(diagram.graph.error);
    }
    QList<Page> pages      = circuitPages(overviews, 0, "1. Connectivity overview");
    const int   indexCount = (panels.size() + 17) / 18;
    const auto  paths      = circuitPages(diagrams, pages.size() + indexCount);
    pages.append(indexPages(panels, diagrams, families));
    pages.append(paths);
    auto       specifications = parameterSections(panels);
    const auto sources        = sourceSpecifications(data);
    if (!sources.isEmpty())
        specifications.append(QJsonObject{{"title", "Source specifications"}, {"details", sources}});
    pages.append(parameters(specifications, families));
    const auto html = htmlReport(data, diagrams, pages, families, outputBase, overviews);
    if (html.isEmpty())
        return fail("Cannot prepare primitive report HTML or page preview");
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
