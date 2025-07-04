// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SOCMODULECONNECTOR_H
#define SOCMODULECONNECTOR_H

#include <qschematic/items/connector.hpp>

namespace ModuleLibrary {

/**
 * @brief The SocModuleConnector class.
 * @details Custom connector for SOC modules with better visual appearance.
 */
class SocModuleConnector : public QSchematic::Items::Connector
{
public:
    enum PortType { Input, Output };

    /**
     * @brief Constructor for SocModuleConnector.
     * @param[in] gridPoint Grid position for the connector
     * @param[in] text Text label for the connector
     * @param[in] portType Type of port (Input/Output)
     * @param[in] parent Parent graphics item
     */
    explicit SocModuleConnector(
        const QPoint  &gridPoint,
        const QString &text,
        PortType       portType = Input,
        QGraphicsItem *parent   = nullptr);

    /**
     * @brief Create a deep copy of this connector.
     * @return Deep copy of this connector
     */
    std::shared_ptr<QSchematic::Items::Item> deepCopy() const override;

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

private:
    PortType m_portType;
};

} // namespace ModuleLibrary

#endif // SOCMODULECONNECTOR_H
