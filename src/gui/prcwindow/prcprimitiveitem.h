// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCPRIMITIVEITEM_H
#define PRCPRIMITIVEITEM_H

#include <qschematic/items/connector.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <gpds/container.hpp>
#include <QBrush>
#include <QColor>
#include <QGraphicsItem>
#include <QMap>
#include <QPen>
#include <QString>
#include <QVariant>

namespace PrcLibrary {

/**
 * @brief Power/Reset/Clock primitive types
 */
enum PrimitiveType {
    ClockSource = 0, /**< Clock signal source */
    ClockTarget = 1, /**< Clock signal target with optional gating */
    ResetSource = 2, /**< Reset signal source */
    ResetTarget = 3, /**< Reset signal target with optional synchronization */
    PowerDomain = 4  /**< Power domain with enable/ready/fault signals */
};

/**
 * @brief PRC primitive item for schematic editor
 * @details Represents clock/reset/power domain nodes with type-specific connectors.
 *          Each primitive type has its own connector layout and configuration options.
 */
class PrcPrimitiveItem : public QSchematic::Items::Node
{
    Q_OBJECT

public:
    /* QGraphicsItem type identifier */
    static constexpr int Type = QGraphicsItem::UserType + 100;

    /**
     * @brief Construct a PRC primitive item
     * @param[in] primitiveType Type of the primitive (clock/reset/power)
     * @param[in] name Display name, defaults to type name if empty
     * @param[in] parent Parent graphics item
     */
    explicit PrcPrimitiveItem(
        PrimitiveType  primitiveType,
        const QString &name   = QString(),
        QGraphicsItem *parent = nullptr);

    ~PrcPrimitiveItem() override = default;

    PrimitiveType primitiveType() const;

    /**
     * @brief Get human-readable type name
     * @return Type name string (e.g., "Clock Source")
     */
    QString primitiveTypeName() const;

    /**
     * @brief Get primitive display name
     * @return Current display name
     */
    QString primitiveName() const;

    /**
     * @brief Set primitive display name
     * @param[in] name New display name
     */
    void setPrimitiveName(const QString &name);

    /**
     * @brief Get configuration value
     * @param[in] key Configuration key
     * @param[in] defaultValue Default value if key not found
     * @return Configuration value or default
     */
    QVariant config(const QString &key, const QVariant &defaultValue = QVariant()) const;

    /**
     * @brief Set configuration value
     * @param[in] key Configuration key
     * @param[in] value Configuration value
     */
    void setConfig(const QString &key, const QVariant &value);

    /**
     * @brief Get all configuration values
     * @return Map of all configuration key-value pairs
     */
    QMap<QString, QVariant> configuration() const;

    /**
     * @brief Set all configuration values
     * @param[in] config Map of configuration key-value pairs
     */
    void setConfiguration(const QMap<QString, QVariant> &config);

    /**
     * @brief Create a deep copy of this item
     * @return Shared pointer to the copied item
     */
    std::shared_ptr<QSchematic::Items::Item> deepCopy() const override;

    /**
     * @brief Serialize item to GPDS container
     * @return GPDS container with serialized data
     */
    gpds::container to_container() const override;

    /**
     * @brief Deserialize item from GPDS container
     * @param[in] container GPDS container with serialized data
     */
    void from_container(const gpds::container &container) override;

    /**
     * @brief Paint the primitive item
     * @param[in] painter QPainter instance
     * @param[in] option Style options
     * @param[in] widget Widget being painted on
     */
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    /**
     * @brief Create connectors based on primitive type
     */
    void createConnectors();

    /**
     * @brief Update label position to center it
     */
    void updateLabelPosition();

    /**
     * @brief Get color based on primitive type
     * @return Color for this primitive type
     */
    QColor getTypeColor() const;

    PrimitiveType                                        m_primitiveType; /**< Primitive type */
    QString                                              m_primitiveName; /**< Display name */
    QMap<QString, QVariant>                              m_config;        /**< Configuration map */
    std::shared_ptr<QSchematic::Items::Label>            m_label;         /**< Name label */
    QList<std::shared_ptr<QSchematic::Items::Connector>> m_connectors;    /**< Connector list */

    static constexpr qreal WIDTH        = 120.0; /**< Primitive width */
    static constexpr qreal HEIGHT       = 80.0;  /**< Primitive height */
    static constexpr qreal LABEL_HEIGHT = 20.0;  /**< Label area height */
};

} // namespace PrcLibrary

#endif // PRCPRIMITIVEITEM_H
