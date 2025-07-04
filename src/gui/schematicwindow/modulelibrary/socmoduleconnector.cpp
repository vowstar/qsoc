// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "socmoduleconnector.h"

#include <qschematic/items/label.hpp>

#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace ModuleLibrary;

#define SIZE (_settings.gridSize / 2)
#define RECT (QRectF(-SIZE, -SIZE, 2 * SIZE, 2 * SIZE))

// Color definitions for different port types
const QColor INPUT_COLOR_FILL    = QColor(100, 200, 100); // Green for inputs
const QColor INPUT_COLOR_BORDER  = QColor(80, 160, 80);
const QColor OUTPUT_COLOR_FILL   = QColor(200, 100, 100); // Red for outputs
const QColor OUTPUT_COLOR_BORDER = QColor(160, 80, 80);
const qreal  CONNECTOR_PEN_WIDTH = 1.5;

SocModuleConnector::SocModuleConnector(
    const QPoint &gridPoint, const QString &text, PortType portType, QGraphicsItem *parent)
    : QSchematic::Items::Connector(QSchematic::Items::Item::ConnectorType, gridPoint, text, parent)
    , m_portType(portType)
{
    label()->setVisible(true);
    setForceTextDirection(false);
}

std::shared_ptr<QSchematic::Items::Item> SocModuleConnector::deepCopy() const
{
    auto clone = std::make_shared<SocModuleConnector>(gridPos(), text(), m_portType, parentItem());
    copyAttributes(*clone);
    return clone;
}

QRectF SocModuleConnector::boundingRect() const
{
    qreal adj = 1.5;
    return RECT.adjusted(-adj, -adj, adj, adj);
}

void SocModuleConnector::paint(
    QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // Draw the bounding rect if debug mode is enabled
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    // Choose colors based on port type
    QColor fillColor, borderColor;
    if (m_portType == Input) {
        fillColor   = INPUT_COLOR_FILL;
        borderColor = INPUT_COLOR_BORDER;
    } else {
        fillColor   = OUTPUT_COLOR_FILL;
        borderColor = OUTPUT_COLOR_BORDER;
    }

    // Body pen
    QPen bodyPen;
    bodyPen.setWidthF(CONNECTOR_PEN_WIDTH);
    bodyPen.setStyle(Qt::SolidLine);
    bodyPen.setColor(borderColor);

    // Body brush
    QBrush bodyBrush;
    bodyBrush.setStyle(Qt::SolidPattern);
    bodyBrush.setColor(fillColor);

    // Draw the connector
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);

    if (m_portType == Input) {
        // Draw a square for input ports
        painter->drawRect(RECT);
    } else {
        // Draw a circle for output ports
        painter->drawEllipse(RECT);
    }
}
