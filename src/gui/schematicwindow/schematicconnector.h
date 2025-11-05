// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICCONNECTOR_H
#define SCHEMATICCONNECTOR_H

#include <gpds/container.hpp>
#include <qschematic/items/connector.hpp>

/**
 * @brief The SchematicConnector class.
 * @details Custom connector for SOC modules with better visual appearance.
 */
class SchematicConnector : public QSchematic::Items::Connector
{
public:
    enum PortType { Input, Output, InOut, Bus };
    enum Position { Left, Right, Top, Bottom };

    /**
     * @brief Constructor for SchematicConnector.
     * @param[in] gridPoint Grid position for the connector
     * @param[in] text Text label for the connector
     * @param[in] portType Type of port (Input/Output/InOut/Bus)
     * @param[in] position Position on module (Left/Right/Top/Bottom)
     * @param[in] parent Parent graphics item
     */
    explicit SchematicConnector(
        const QPoint  &gridPoint,
        const QString &text,
        PortType       portType = Input,
        Position       position = Left,
        QGraphicsItem *parent   = nullptr);

    /**
     * @brief Create a deep copy of this connector.
     * @return Deep copy of this connector
     */
    std::shared_ptr<QSchematic::Items::Item> deepCopy() const override;

    /**
     * @brief Serialize to container.
     * @return Serialized container
     */
    gpds::container to_container() const override;

    /**
     * @brief Deserialize from container.
     * @param[in] container Container with serialized data
     */
    void from_container(const gpds::container &container) override;

    /**
     * @brief Get the bounding rectangle.
     * @return Bounding rectangle
     */
    QRectF boundingRect() const override;

    /**
     * @brief Paint the connector.
     * @param[in] painter Painter object
     * @param[in] option Style option
     * @param[in] widget Widget being painted on
     */
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    /**
     * @brief Get the port type.
     * @return Port type
     */
    PortType portType() const { return m_portType; }

    /**
     * @brief Set the port type.
     * @param[in] portType Port type
     */
    void setPortType(PortType portType) { m_portType = portType; }

    /**
     * @brief Get the module position.
     * @return Module position
     */
    Position modulePosition() const { return m_position; }

    /**
     * @brief Set the module position.
     * @param[in] position Module position
     */
    void setModulePosition(Position position) { m_position = position; }

private:
    PortType m_portType;
    Position m_position;

    /**
     * @brief Create input port shape (rectangle with inward triangle tab)
     * @return QPolygonF shape
     */
    QPolygonF createInputShape() const;

    /**
     * @brief Create output port shape (rectangle with outward triangle tab)
     * @return QPolygonF shape
     */
    QPolygonF createOutputShape() const;

    /**
     * @brief Create inout port shape (rectangle with triangles on both sides)
     * @return QPolygonF shape
     */
    QPolygonF createInOutShape() const;

    /**
     * @brief Create bus port shape (simple rectangle)
     * @return QPolygonF shape
     */
    QPolygonF createBusShape() const;

    /**
     * @brief Update position based on current location relative to parent
     */
    void updatePositionFromLocation();
};

#endif // SCHEMATICCONNECTOR_H
