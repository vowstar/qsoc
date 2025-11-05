// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "socmoduleconnector.h"
#include "itemtypes.h"

#include <gpds/container.hpp>
#include <qschematic/items/label.hpp>

#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace ModuleLibrary;

#define SIZE (_settings.gridSize / 3) // Make ports smaller
#define RECT (QRectF(-SIZE, -SIZE, 2 * SIZE, 2 * SIZE))

// Color definitions for different port types (based on your color scheme)
const QColor INPUT_COLOR_FILL    = QColor(0, 132, 0); // Color4DConnEx: #008400
const QColor INPUT_COLOR_BORDER  = QColor(0, 132, 0);
const QColor OUTPUT_COLOR_FILL   = QColor(132, 0, 0); // Color4DBodyEx: #840000
const QColor OUTPUT_COLOR_BORDER = QColor(132, 0, 0);
const QColor INOUT_COLOR_FILL    = QColor(132, 132, 0); // Color4DHLabelEx: #848400
const QColor INOUT_COLOR_BORDER  = QColor(132, 132, 0);
const QColor BUS_COLOR_FILL      = QColor(0, 0, 132); // Color4DBusEx: #000084
const QColor BUS_COLOR_BORDER    = QColor(0, 0, 132);
const qreal  CONNECTOR_PEN_WIDTH = 1.5;
const qreal  BUS_PEN_WIDTH       = 3.0;

SocModuleConnector::SocModuleConnector(
    const QPoint  &gridPoint,
    const QString &text,
    PortType       portType,
    Position       position,
    QGraphicsItem *parent)
    : QSchematic::Items::Connector(ModuleLibrary::SocModuleConnectorType, gridPoint, text, parent)
    , m_portType(portType)
    , m_position(position)
{
    label()->setVisible(true);
    setForceTextDirection(false);
}

std::shared_ptr<QSchematic::Items::Item> SocModuleConnector::deepCopy() const
{
    auto clone = std::make_shared<SocModuleConnector>(
        gridPos(), text(), m_portType, m_position, parentItem());
    copyAttributes(*clone);
    return clone;
}

gpds::container SocModuleConnector::to_container() const
{
    // Root container
    gpds::container root;
    addItemTypeIdToContainer(root);

    // Save base Connector data
    root.add_value("connector", QSchematic::Items::Connector::to_container());

    // Save SocModuleConnector-specific data
    root.add_value("port_type", static_cast<int>(m_portType));
    root.add_value("position", static_cast<int>(m_position));

    return root;
}

void SocModuleConnector::from_container(const gpds::container &container)
{
    // Load base Connector data
    if (auto connectorContainer = container.get_value<gpds::container *>("connector")) {
        QSchematic::Items::Connector::from_container(**connectorContainer);
    }

    // Load SocModuleConnector-specific data
    if (auto portTypeOpt = container.get_value<int>("port_type")) {
        m_portType = static_cast<PortType>(*portTypeOpt);
    }

    if (auto positionOpt = container.get_value<int>("position")) {
        m_position = static_cast<Position>(*positionOpt);
    }
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

    // Update position based on current location before painting
    updatePositionFromLocation();

    // Draw the bounding rect if debug mode is enabled
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    // Choose colors based on port type
    QColor fillColor, borderColor;
    qreal  penWidth = CONNECTOR_PEN_WIDTH;

    switch (m_portType) {
    case Input:
        fillColor   = INPUT_COLOR_FILL;
        borderColor = INPUT_COLOR_BORDER;
        break;
    case Output:
        fillColor   = OUTPUT_COLOR_FILL;
        borderColor = OUTPUT_COLOR_BORDER;
        break;
    case InOut:
        fillColor   = INOUT_COLOR_FILL;
        borderColor = INOUT_COLOR_BORDER;
        break;
    case Bus:
        fillColor   = BUS_COLOR_FILL;
        borderColor = BUS_COLOR_BORDER;
        penWidth    = BUS_PEN_WIDTH;
        break;
    }

    // Body pen
    QPen bodyPen;
    bodyPen.setWidthF(penWidth);
    bodyPen.setStyle(Qt::SolidLine);
    bodyPen.setColor(borderColor);

    // Body brush
    QBrush bodyBrush;
    bodyBrush.setStyle(Qt::SolidPattern);
    bodyBrush.setColor(fillColor);

    // Draw the connector
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);

    // Create shapes based on port type and position
    QPolygonF shape;

    switch (m_portType) {
    case Input:
        // Rectangle with triangle pointing INTO module
        shape = createInputShape();
        break;
    case Output:
        // Rectangle with triangle pointing OUT OF module
        shape = createOutputShape();
        break;
    case InOut:
        // Rectangle with triangles on both module inner and outer sides
        shape = createInOutShape();
        break;
    case Bus:
        // Simple rectangle for bus ports
        shape = createBusShape();
        break;
    }

    painter->drawPolygon(shape);
}

QPolygonF SocModuleConnector::createInputShape() const
{
    QPolygonF shape;
    qreal     tabSize = SIZE * 0.8; // Make triangular tab more prominent

    switch (m_position) {
    case Left:
        // Rectangle with triangle pointing right (into module)
        shape << QPointF(-SIZE, -SIZE)          // Top left
              << QPointF(SIZE - tabSize, -SIZE) // Top right
              << QPointF(SIZE, -SIZE / 2)       // Triangle top point
              << QPointF(SIZE, SIZE / 2)        // Triangle bottom point
              << QPointF(SIZE - tabSize, SIZE)  // Bottom right
              << QPointF(-SIZE, SIZE);          // Bottom left
        break;
    case Right:
        // Rectangle with triangle pointing left (into module)
        shape << QPointF(-SIZE + tabSize, -SIZE) // Top left
              << QPointF(SIZE, -SIZE)            // Top right
              << QPointF(SIZE, SIZE)             // Bottom right
              << QPointF(-SIZE + tabSize, SIZE)  // Bottom left
              << QPointF(-SIZE, SIZE / 2)        // Triangle bottom point
              << QPointF(-SIZE, -SIZE / 2);      // Triangle top point
        break;
    case Top:
        // Rectangle with triangle pointing down (into module)
        shape << QPointF(-SIZE, -SIZE)           // Top left
              << QPointF(SIZE, -SIZE)            // Top right
              << QPointF(SIZE, SIZE - tabSize)   // Bottom right
              << QPointF(SIZE / 2, SIZE)         // Triangle right point
              << QPointF(-SIZE / 2, SIZE)        // Triangle left point
              << QPointF(-SIZE, SIZE - tabSize); // Bottom left
        break;
    case Bottom:
        // Rectangle with triangle pointing up (into module)
        shape << QPointF(-SIZE / 2, -SIZE)        // Triangle left point
              << QPointF(SIZE / 2, -SIZE)         // Triangle right point
              << QPointF(SIZE, -SIZE + tabSize)   // Top right
              << QPointF(SIZE, SIZE)              // Bottom right
              << QPointF(-SIZE, SIZE)             // Bottom left
              << QPointF(-SIZE, -SIZE + tabSize); // Top left
        break;
    }

    return shape;
}

QPolygonF SocModuleConnector::createOutputShape() const
{
    QPolygonF shape;
    qreal     tabSize = SIZE * 0.8; // Make triangular tab more prominent

    switch (m_position) {
    case Left:
        // Rectangle with triangle pointing left (out of module)
        shape << QPointF(-SIZE, -SIZE / 2)       // Triangle top point
              << QPointF(-SIZE + tabSize, -SIZE) // Top left
              << QPointF(SIZE, -SIZE)            // Top right
              << QPointF(SIZE, SIZE)             // Bottom right
              << QPointF(-SIZE + tabSize, SIZE)  // Bottom left
              << QPointF(-SIZE, SIZE / 2);       // Triangle bottom point
        break;
    case Right:
        // Rectangle with triangle pointing right (out of module)
        shape << QPointF(-SIZE, -SIZE)          // Top left
              << QPointF(SIZE - tabSize, -SIZE) // Top right
              << QPointF(SIZE, -SIZE / 2)       // Triangle top point
              << QPointF(SIZE, SIZE / 2)        // Triangle bottom point
              << QPointF(SIZE - tabSize, SIZE)  // Bottom right
              << QPointF(-SIZE, SIZE);          // Bottom left
        break;
    case Top:
        // Rectangle with triangle pointing up (out of module)
        shape << QPointF(-SIZE / 2, -SIZE)        // Triangle left point
              << QPointF(SIZE / 2, -SIZE)         // Triangle right point
              << QPointF(SIZE, -SIZE + tabSize)   // Top right
              << QPointF(SIZE, SIZE)              // Bottom right
              << QPointF(-SIZE, SIZE)             // Bottom left
              << QPointF(-SIZE, -SIZE + tabSize); // Top left
        break;
    case Bottom:
        // Rectangle with triangle pointing down (out of module)
        shape << QPointF(-SIZE, -SIZE)           // Top left
              << QPointF(SIZE, -SIZE)            // Top right
              << QPointF(SIZE, SIZE - tabSize)   // Bottom right
              << QPointF(SIZE / 2, SIZE)         // Triangle right point
              << QPointF(-SIZE / 2, SIZE)        // Triangle left point
              << QPointF(-SIZE, SIZE - tabSize); // Bottom left
        break;
    }

    return shape;
}

QPolygonF SocModuleConnector::createInOutShape() const
{
    QPolygonF shape;
    qreal     tabSize = SIZE * 0.8; // Make triangular tabs more prominent

    switch (m_position) {
    case Left:
        // Rectangle with triangles on both left and right
        shape << QPointF(-SIZE, -SIZE / 2)       // Left triangle top
              << QPointF(-SIZE + tabSize, -SIZE) // Top left
              << QPointF(SIZE - tabSize, -SIZE)  // Top right
              << QPointF(SIZE, -SIZE / 2)        // Right triangle top
              << QPointF(SIZE, SIZE / 2)         // Right triangle bottom
              << QPointF(SIZE - tabSize, SIZE)   // Bottom right
              << QPointF(-SIZE + tabSize, SIZE)  // Bottom left
              << QPointF(-SIZE, SIZE / 2);       // Left triangle bottom
        break;
    case Right:
        // Rectangle with triangles on both left and right
        shape << QPointF(-SIZE, -SIZE / 2)       // Left triangle top
              << QPointF(-SIZE + tabSize, -SIZE) // Top left
              << QPointF(SIZE - tabSize, -SIZE)  // Top right
              << QPointF(SIZE, -SIZE / 2)        // Right triangle top
              << QPointF(SIZE, SIZE / 2)         // Right triangle bottom
              << QPointF(SIZE - tabSize, SIZE)   // Bottom right
              << QPointF(-SIZE + tabSize, SIZE)  // Bottom left
              << QPointF(-SIZE, SIZE / 2);       // Left triangle bottom
        break;
    case Top:
        // Rectangle with triangles on both top and bottom
        shape << QPointF(-SIZE / 2, -SIZE)        // Top triangle left
              << QPointF(SIZE / 2, -SIZE)         // Top triangle right
              << QPointF(SIZE, -SIZE + tabSize)   // Top right
              << QPointF(SIZE, SIZE - tabSize)    // Bottom right
              << QPointF(SIZE / 2, SIZE)          // Bottom triangle right
              << QPointF(-SIZE / 2, SIZE)         // Bottom triangle left
              << QPointF(-SIZE, SIZE - tabSize)   // Bottom left
              << QPointF(-SIZE, -SIZE + tabSize); // Top left
        break;
    case Bottom:
        // Rectangle with triangles on both top and bottom
        shape << QPointF(-SIZE / 2, -SIZE)        // Top triangle left
              << QPointF(SIZE / 2, -SIZE)         // Top triangle right
              << QPointF(SIZE, -SIZE + tabSize)   // Top right
              << QPointF(SIZE, SIZE - tabSize)    // Bottom right
              << QPointF(SIZE / 2, SIZE)          // Bottom triangle right
              << QPointF(-SIZE / 2, SIZE)         // Bottom triangle left
              << QPointF(-SIZE, SIZE - tabSize)   // Bottom left
              << QPointF(-SIZE, -SIZE + tabSize); // Top left
        break;
    }

    return shape;
}

QPolygonF SocModuleConnector::createBusShape() const
{
    // Simple rectangle for bus ports
    QPolygonF shape;
    shape << QPointF(-SIZE, -SIZE) // Top left
          << QPointF(SIZE, -SIZE)  // Top right
          << QPointF(SIZE, SIZE)   // Bottom right
          << QPointF(-SIZE, SIZE); // Bottom left

    return shape;
}

void SocModuleConnector::updatePositionFromLocation()
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
