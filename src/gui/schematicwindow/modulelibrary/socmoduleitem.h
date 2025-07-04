// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SOCMODULEITEM_H
#define SOCMODULEITEM_H

#include <qschematic/items/connector.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <yaml-cpp/yaml.h>
#include <QBrush>
#include <QColor>
#include <QGraphicsItem>
#include <QPen>
#include <QString>

namespace ModuleLibrary {

/**
 * @brief The SocModuleItem class.
 * @details This class represents a SOC module item that can be placed on a schematic.
 *          It displays the module name and creates connectors for each port.
 */
class SocModuleItem : public QSchematic::Items::Node
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for SocModuleItem.
     * @details This constructor initializes the SOC module item.
     * @param[in] moduleName Name of the module
     * @param[in] moduleYaml YAML data containing module definition
     * @param[in] type Type identifier for the item
     * @param[in] parent Parent graphics item
     */
    explicit SocModuleItem(
        const QString    &moduleName,
        const YAML::Node &moduleYaml,
        int               type   = QSchematic::Items::Item::NodeType,
        QGraphicsItem    *parent = nullptr);

    /**
     * @brief Destructor for SocModuleItem.
     * @details This destructor will free the SOC module item.
     */
    ~SocModuleItem() override = default;

    /**
     * @brief Get the module name.
     * @details This function returns the name of the module.
     * @return Module name
     */
    QString moduleName() const;

    /**
     * @brief Set the module name.
     * @details This function sets the name of the module.
     * @param[in] name Module name
     */
    void setModuleName(const QString &name);

    /**
     * @brief Get the module YAML data.
     * @details This function returns the YAML data for the module.
     * @return Module YAML data
     */
    YAML::Node moduleYaml() const;

    /**
     * @brief Set the module YAML data.
     * @details This function sets the YAML data for the module and recreates ports.
     * @param[in] yaml Module YAML data
     */
    void setModuleYaml(const YAML::Node &yaml);

    /**
     * @brief Create a deep copy of this item.
     * @details This function creates a deep copy of this item.
     * @return Deep copy of this item
     */
    std::shared_ptr<QSchematic::Items::Item> deepCopy() const override;

    /**
     * @brief Paint the item.
     * @details This function paints the item on the graphics scene.
     * @param[in] painter Painter object
     * @param[in] option Style option
     * @param[in] widget Widget being painted on
     */
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    /**
     * @brief Create ports from YAML data.
     * @details This function creates connector ports based on the module YAML data.
     */
    void createPortsFromYaml();

    /**
     * @brief Calculate the required size for the module.
     * @details This function calculates the size needed to display all ports.
     * @return Required size
     */
    QSizeF calculateRequiredSize() const;

    /**
     * @brief Arrange ports on the module.
     * @details This function arranges the input and output ports on the module.
     */
    void arrangePorts();

    /**
     * @brief Update label position.
     * @details This function updates the position of the module name label.
     */
    void updateLabelPosition();

    QString                                              m_moduleName; /**< Module name */
    YAML::Node                                           m_moduleYaml; /**< Module YAML data */
    std::shared_ptr<QSchematic::Items::Label>            m_label;      /**< Module name label */
    QList<std::shared_ptr<QSchematic::Items::Connector>> m_ports; /**< List of port connectors */

    static constexpr qreal PORT_SPACING = 20.0; /**< Spacing between ports */
    static constexpr qreal MIN_WIDTH    = 80.0; /**< Minimum module width */
    static constexpr qreal MIN_HEIGHT   = 60.0; /**< Minimum module height */
    static constexpr qreal LABEL_HEIGHT = 20.0; /**< Height reserved for label */
};

} // namespace ModuleLibrary

#endif // SOCMODULEITEM_H
