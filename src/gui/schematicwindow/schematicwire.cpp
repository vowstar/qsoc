// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematicwire.h"

#include <QPainter>

SchematicWire::SchematicWire(int type, QGraphicsItem *parent)
    : QSchematic::Items::Wire(type, parent)
{}

bool SchematicWire::isBusWire() const
{
    return m_isBusWire;
}

void SchematicWire::setBusWire(bool isBus)
{
    m_isBusWire = isBus;
}

gpds::container SchematicWire::to_container() const
{
    auto container = Wire::to_container();
    container.add_value("is_bus", m_isBusWire);
    return container;
}

void SchematicWire::from_container(const gpds::container &container)
{
    Wire::from_container(container);
    m_isBusWire = container.get_value<bool>("is_bus").value_or(false);
}

void SchematicWire::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    /* If this is a bus wire, draw thicker background line first */
    if (isBusWire()) {
        QPen busPen;
        busPen.setStyle(Qt::SolidLine);
        busPen.setCapStyle(Qt::RoundCap);
        busPen.setWidth(5);                          // Thicker than normal (normal is 1)
        busPen.setColor(QColor(100, 130, 200, 160)); // Soft deep blue, semi-transparent

        painter->setPen(busPen);
        painter->setBrush(Qt::NoBrush);

        /* Draw the background polyline */
        const auto &points = pointsRelative();
        painter->drawPolyline(points.constData(), points.count());
    }

    /* Call base class to draw normal wire on top */
    QSchematic::Items::Wire::paint(painter, option, widget);
}
