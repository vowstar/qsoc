// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcconnector.h"

#include <QPainter>
#include <QStyleOptionGraphicsItem>

#define SIZE (_settings.gridSize / 3) // Make connectors smaller
#define RECT (QRectF(-SIZE, -SIZE, 2 * SIZE, 2 * SIZE))

/* Unified color scheme matching schematicwindow */
const QColor CONNECTOR_COLOR_FILL       = QColor(132, 0, 0);     /* Deep red fill */
const QColor CONNECTOR_COLOR_BORDER     = QColor(132, 0, 0);     /* Deep red border */
const QColor CONNECTOR_COLOR_AVAILABLE  = QColor(180, 180, 180); /* Gray for available */
const qreal  CONNECTOR_PEN_WIDTH        = 1.5;
const qreal  CONNECTOR_PEN_WIDTH_DASHED = 1.0;

using namespace PrcLibrary;

PrcConnector::PrcConnector(
    const QPoint  &gridPoint,
    const QString &text,
    PortType       portType,
    Position       position,
    QGraphicsItem *parent)
    : QSchematic::Items::Connector(PrcConnectorType, gridPoint, text, parent)
    , m_portType(portType)
    , m_position(position)
{
    label()->setVisible(true);
    label()->setOpacity(0.4); /* Default: unconnected (faded) */
    setForceTextDirection(false);
}

std::shared_ptr<QSchematic::Items::Item> PrcConnector::deepCopy() const
{
    auto clone
        = std::make_shared<PrcConnector>(gridPos(), text(), m_portType, m_position, parentItem());
    copyAttributes(*clone);
    return clone;
}

gpds::container PrcConnector::to_container() const
{
    // Root container
    gpds::container root;
    addItemTypeIdToContainer(root);

    // Save base Connector data
    root.add_value("connector", QSchematic::Items::Connector::to_container());

    // Save PrcConnector-specific data
    root.add_value("port_type", static_cast<int>(m_portType));
    root.add_value("position", static_cast<int>(m_position));

    return root;
}

void PrcConnector::from_container(const gpds::container &container)
{
    // Load base Connector data
    if (auto connectorContainer = container.get_value<gpds::container *>("connector")) {
        QSchematic::Items::Connector::from_container(**connectorContainer);
    }

    // Load PrcConnector-specific data
    if (auto portTypeOpt = container.get_value<int>("port_type")) {
        m_portType = static_cast<PortType>(*portTypeOpt);
    }

    if (auto positionOpt = container.get_value<int>("position")) {
        m_position = static_cast<Position>(*positionOpt);
    }
}

QRectF PrcConnector::boundingRect() const
{
    qreal adj = 1.5;
    return RECT.adjusted(-adj, -adj, adj, adj);
}

void PrcConnector::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    /* Update position based on current location before painting */
    updatePositionFromLocation();

    /* Draw the bounding rect if debug mode is enabled */
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    /* Use self-maintained connection state (more reliable than hasConnection after file load) */
    bool connected = m_isConnected;

    /* Choose colors and style based on connection state */
    QColor       fillColor;
    QColor       borderColor;
    qreal        penWidth;
    Qt::PenStyle penStyle;

    if (connected) {
        /* Connected: solid fill, solid border */
        fillColor   = CONNECTOR_COLOR_FILL;
        borderColor = CONNECTOR_COLOR_BORDER;
        penWidth    = CONNECTOR_PEN_WIDTH;
        penStyle    = Qt::SolidLine;
    } else {
        /* Available: no fill, dashed border (gray) */
        fillColor   = Qt::transparent;
        borderColor = CONNECTOR_COLOR_AVAILABLE;
        penWidth    = CONNECTOR_PEN_WIDTH_DASHED;
        penStyle    = Qt::DashLine;
    }

    /* Body pen */
    QPen bodyPen;
    bodyPen.setWidthF(penWidth);
    bodyPen.setStyle(penStyle);
    bodyPen.setColor(borderColor);

    /* Body brush */
    QBrush bodyBrush;
    if (connected) {
        bodyBrush.setStyle(Qt::SolidPattern);
        bodyBrush.setColor(fillColor);
    } else {
        bodyBrush.setStyle(Qt::NoBrush);
    }

    /* Draw the connector */
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);

    /* Create shape (simple rectangle for all PRC connectors) */
    QPolygonF shape = createShape();
    painter->drawPolygon(shape);
}

void PrcConnector::updatePositionFromLocation()
{
    if (!parentItem()) {
        return;
    }

    // Get the parent's bounding rect
    QRectF  parentRect   = parentItem()->boundingRect();
    QPointF connectorPos = pos();

    // Determine which edge the connector is closest to
    Position newPosition = Left; // Default

    qreal leftDist   = qAbs(connectorPos.x() - parentRect.left());
    qreal rightDist  = qAbs(connectorPos.x() - parentRect.right());
    qreal topDist    = qAbs(connectorPos.y() - parentRect.top());
    qreal bottomDist = qAbs(connectorPos.y() - parentRect.bottom());

    // Find the minimum distance to determine the edge
    qreal minDist = qMin(qMin(leftDist, rightDist), qMin(topDist, bottomDist));

    if (minDist == leftDist) {
        newPosition = Left;
    } else if (minDist == rightDist) {
        newPosition = Right;
    } else if (minDist == topDist) {
        newPosition = Top;
    } else if (minDist == bottomDist) {
        newPosition = Bottom;
    }

    // Update position if changed
    if (newPosition != m_position) {
        m_position = newPosition;
        update(); // Trigger a repaint
    }
}

QPolygonF PrcConnector::createShape() const
{
    // Simple rectangle for all PRC connectors (matching schematicwindow bus shape)
    QPolygonF shape;
    shape << QPointF(-SIZE, -SIZE) // Top left
          << QPointF(SIZE, -SIZE)  // Top right
          << QPointF(SIZE, SIZE)   // Bottom right
          << QPointF(-SIZE, SIZE); // Bottom left

    return shape;
}

void PrcConnector::setConnected(bool connected)
{
    if (m_isConnected != connected) {
        m_isConnected = connected;

        /* Fade label for unconnected ports */
        if (label()) {
            label()->setOpacity(connected ? 1.0 : 0.4);
        }

        update();
    }
}

bool PrcConnector::isConnected() const
{
    return m_isConnected;
}
