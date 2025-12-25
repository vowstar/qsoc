// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCCONNECTOR_H
#define PRCCONNECTOR_H

#include "prcitemtypes.h"

#include <qschematic/items/connector.hpp>
#include <qschematic/items/label.hpp>

#include <gpds/container.hpp>
#include <QColor>
#include <QGraphicsItem>
#include <QPoint>
#include <QPolygonF>
#include <QString>

namespace PrcLibrary {

/**
 * @brief Custom connector for PRC primitives
 * @details Provides consistent visual styling matching schematicwindow connectors
 */
class PrcConnector : public QSchematic::Items::Connector
{
public:
    /**
     * @brief Connector type (port direction/function)
     */
    enum PortType {
        Signal = 0, /**< Regular signal port */
        Power  = 1, /**< Power/ground port */
        Clock  = 2, /**< Clock signal port */
        Reset  = 3  /**< Reset signal port */
    };

    /**
     * @brief Connector position on parent item
     */
    enum Position { Left = 0, Right = 1, Top = 2, Bottom = 3 };

    /**
     * @brief Construct a PRC connector
     * @param[in] gridPoint Grid position relative to parent
     * @param[in] text Connector label text
     * @param[in] portType Port type (signal/power/clock/reset)
     * @param[in] position Position on parent item edge
     * @param[in] parent Parent graphics item
     */
    explicit PrcConnector(
        const QPoint  &gridPoint = QPoint(0, 0),
        const QString &text      = QString(),
        PortType       portType  = Signal,
        Position       position  = Left,
        QGraphicsItem *parent    = nullptr);

    ~PrcConnector() override = default;

    /**
     * @brief Create a deep copy of this connector
     * @return Shared pointer to the copied connector
     */
    std::shared_ptr<QSchematic::Items::Item> deepCopy() const override;

    /**
     * @brief Serialize connector to GPDS container
     * @return GPDS container with serialized data
     */
    gpds::container to_container() const override;

    /**
     * @brief Deserialize connector from GPDS container
     * @param[in] container GPDS container with serialized data
     */
    void from_container(const gpds::container &container) override;

    /**
     * @brief Get bounding rectangle
     * @return Bounding rectangle
     */
    QRectF boundingRect() const override;

    /**
     * @brief Paint the connector
     * @param[in] painter QPainter instance
     * @param[in] option Style options
     * @param[in] widget Widget being painted on
     */
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    /**
     * @brief Set connection state (for visual rendering)
     * @param[in] connected true if wire is connected
     */
    void setConnected(bool connected);

    /**
     * @brief Check if connector is marked as connected
     * @return true if connected
     */
    bool isConnected() const;

private:
    /**
     * @brief Update position from current location
     */
    void updatePositionFromLocation();

    /**
     * @brief Create connector shape based on type and position
     * @return Shape polygon
     */
    QPolygonF createShape() const;

    PortType m_portType;      /**< Port type */
    Position m_position;      /**< Connector position */
    bool     m_isConnected{}; /**< Connection state for rendering */
};

} // namespace PrcLibrary

#endif // PRCCONNECTOR_H
