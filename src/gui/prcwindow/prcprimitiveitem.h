// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCPRIMITIVEITEM_H
#define PRCPRIMITIVEITEM_H

#include "prcconnector.h"

#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <gpds/container.hpp>
#include <QBrush>
#include <QColor>
#include <QGraphicsItem>
#include <QPen>
#include <QString>

#include <variant>

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
 * @brief Clock source configuration
 */
struct ClockSourceParams
{
    double frequency_mhz = 100.0; /**< Frequency in MHz */
    double phase_deg     = 0.0;   /**< Phase offset in degrees */
};

/**
 * @brief Clock target configuration
 */
struct ClockTargetParams
{
    int  divider     = 1;     /**< Clock divider ratio */
    bool enable_gate = false; /**< Enable clock gating */
};

/**
 * @brief Reset source configuration
 */
struct ResetSourceParams
{
    bool   active_low  = true; /**< Active low reset signal */
    double duration_us = 10.0; /**< Reset duration in microseconds */
};

/**
 * @brief Reset target configuration
 */
struct ResetTargetParams
{
    bool synchronous = true; /**< Synchronous reset */
    int  stages      = 2;    /**< Synchronizer stages */
};

/**
 * @brief Power domain configuration
 */
struct PowerDomainParams
{
    double voltage   = 1.0;   /**< Operating voltage */
    bool   isolation = true;  /**< Enable isolation */
    bool   retention = false; /**< Enable retention */
};

/**
 * @brief Type-safe union for primitive parameters
 */
using PrcParams = std::variant<
    ClockSourceParams,
    ClockTargetParams,
    ResetSourceParams,
    ResetTargetParams,
    PowerDomainParams>;

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
     * @brief Get primitive parameters (type-safe)
     * @return Reference to parameter variant
     */
    const PrcParams &params() const;

    /**
     * @brief Set primitive parameters (type-safe)
     * @param[in] params Parameter variant
     */
    void setParams(const PrcParams &params);

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

    PrimitiveType                             m_primitiveType; /**< Primitive type */
    QString                                   m_primitiveName; /**< Display name */
    PrcParams                                 m_params;        /**< Type-safe parameters */
    std::shared_ptr<QSchematic::Items::Label> m_label;         /**< Name label */
    QList<std::shared_ptr<PrcConnector>>      m_connectors;    /**< Connector list */

    static constexpr qreal WIDTH        = 120.0; /**< Primitive width */
    static constexpr qreal HEIGHT       = 80.0;  /**< Primitive height */
    static constexpr qreal LABEL_HEIGHT = 20.0;  /**< Label area height */
};

} // namespace PrcLibrary

#endif // PRCPRIMITIVEITEM_H
