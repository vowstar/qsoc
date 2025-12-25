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

/* Primitive Types */
enum PrimitiveType {
    ClockInput  = 1,  /**< Clock input source */
    ClockTarget = 2,  /**< Clock target with operations */
    ResetSource = 11, /**< Reset source */
    ResetTarget = 12, /**< Reset target */
    PowerDomain = 21, /**< Power domain */
};

/* Controller Definition (stored at Scene level, referenced by primitives) */
struct ClockControllerDef
{
    QString name = "clock_ctrl"; /**< Controller instance name */
    QString test_enable;         /**< Test enable bypass signal */
};

struct ResetControllerDef
{
    QString name = "reset_ctrl"; /**< Controller instance name */
    QString test_enable;         /**< Test enable bypass signal */
};

struct PowerControllerDef
{
    QString name = "power_ctrl"; /**< Controller instance name */
    QString host_clock;          /**< AO host clock */
    QString host_reset;          /**< AO host reset */
    QString test_enable;         /**< Test enable bypass signal */
};

/* Shared Parameter Structures */
struct STAGuideParams
{
    bool    configured = false;
    QString cell;     /**< Buffer cell name (e.g., "BUF_X2") */
    QString in;       /**< Input pin name (e.g., "I") */
    QString out;      /**< Output pin name (e.g., "Z") */
    QString instance; /**< Instance name (e.g., "u_DONTTOUCH_xxx") */
};

/**
 * @brief ICG (Integrated Clock Gating) configuration
 */
struct ICGParams
{
    bool           configured = false;
    QString        enable;                 /**< Gate enable signal */
    QString        polarity;               /**< Enable polarity: "high" or "low" */
    QString        test_enable;            /**< Test enable signal (optional) */
    QString        reset;                  /**< Reset signal (optional) */
    bool           clock_on_reset = false; /**< Clock enabled during reset */
    STAGuideParams sta_guide;
};

/**
 * @brief DIV (Clock Divider) configuration
 */
struct DIVParams
{
    bool           configured    = false;
    int            default_value = 1;      /**< Default division value */
    QString        value;                  /**< Runtime divider control signal (optional) */
    int            width = 0;              /**< Divider bit width (0 = auto) */
    QString        reset;                  /**< Reset signal */
    bool           clock_on_reset = false; /**< Clock enabled during reset */
    STAGuideParams sta_guide;
};

/**
 * @brief MUX configuration
 */
struct MUXParams
{
    bool           configured = false;
    STAGuideParams sta_guide;
};

/**
 * @brief INV (Clock Inverter) configuration
 */
struct INVParams
{
    bool           configured = false;
    STAGuideParams sta_guide;
};

/* Link-level Parameters (for wires connecting inputs to targets) */
/**
 * @brief Link-level clock operations (applied per wire/connection)
 * @details In soc_net format, each link from input to target can have
 *          its own ICG, DIV, INV, and STA guide operations.
 *          Processing order: source -> icg -> div -> inv -> sta_guide -> target
 */
struct ClockLinkParams
{
    QString        sourceName; /**< Source input name this link connects from */
    ICGParams      icg;        /**< Link-level ICG */
    DIVParams      div;        /**< Link-level DIV */
    INVParams      inv;        /**< Link-level INV */
    STAGuideParams sta_guide;  /**< Link-level STA guide (at end of chain) */
};

/**
 * @brief Link-level reset operations (applied per wire/connection)
 */
struct ResetLinkParams
{
    QString sourceName; /**< Source name this link connects from */
};

/* Clock Domain Parameters */
struct ClockInputParams
{
    QString name;       /**< Input name (e.g., "clk_hse") */
    QString freq;       /**< Frequency string (e.g., "25MHz") */
    QString controller; /**< Controller name this belongs to */
};

struct ClockTargetParams
{
    QString name;       /**< Target name (e.g., "clk_noc") */
    QString freq;       /**< Target frequency (e.g., "400MHz") */
    QString controller; /**< Controller name this belongs to */

    /* Target-level operations */
    MUXParams mux; /**< Target-level MUX (auto when >1 link) */
    ICGParams icg; /**< Target-level ICG */
    DIVParams div; /**< Target-level DIV */
    INVParams inv; /**< Target-level INV */

    /* Selection and control */
    QString select;     /**< MUX select signal */
    QString reset;      /**< MUX reset signal (GF_MUX when present) */
    QString test_clock; /**< DFT test clock */
};

/* Reset Domain Parameters */

/**
 * @brief Reset source parameters
 */
struct ResetSourceParams
{
    QString name;       /**< Source name (e.g., "por_rst_n") */
    QString active;     /**< Active level: "high" or "low" */
    QString controller; /**< Controller name this belongs to */
};

/**
 * @brief Reset synchronizer configuration
 */
struct ResetSyncParams
{
    /* Async synchronizer (qsoc_rst_sync) */
    bool    async_configured = false;
    QString async_clock;
    int     async_stage = 4;

    /* Sync pipeline (qsoc_rst_pipe) */
    bool    sync_configured = false;
    QString sync_clock;
    int     sync_stage = 2;

    /* Counter-based release (qsoc_rst_count) */
    bool    count_configured = false;
    QString count_clock;
    int     count_value = 16;
};

/**
 * @brief Reset target parameters
 */
struct ResetTargetParams
{
    QString         name;       /**< Target name (e.g., "cpu_rst_n") */
    QString         active;     /**< Active level: "high" or "low" */
    QString         controller; /**< Controller name this belongs to */
    ResetSyncParams sync;       /**< Target-level synchronizer */
};

/* Power Domain Parameters */

/**
 * @brief Power domain dependency
 */
struct PowerDependency
{
    QString name; /**< Dependency domain name */
    QString type; /**< "hard" or "soft" */
};

/**
 * @brief Power domain follow entry (reset synchronizer)
 */
struct PowerFollow
{
    QString clock;     /**< Domain clock */
    QString reset;     /**< Synchronized reset output */
    int     stage = 4; /**< Synchronizer stages */
};

/**
 * @brief Power domain parameters
 */
struct PowerDomainParams
{
    QString                name;           /**< Domain name (e.g., "ao", "gpu") */
    QString                controller;     /**< Controller name this belongs to */
    int                    v_mv = 900;     /**< Voltage level in mV */
    QString                pgood;          /**< Power good input signal (optional) */
    int                    wait_dep   = 0; /**< Dependency wait cycles */
    int                    settle_on  = 0; /**< Power-on settle cycles */
    int                    settle_off = 0; /**< Power-off settle cycles */
    QList<PowerDependency> depend;         /**< Dependencies (empty = AO, [] = root) */
    QList<PowerFollow>     follow;         /**< Reset synchronizer entries */
};

/* Parameter Variant */

/**
 * @brief Type-safe union for all primitive parameters
 */
using PrcParams = std::
    variant<ClockInputParams, ClockTargetParams, ResetSourceParams, ResetTargetParams, PowerDomainParams>;

/* Primitive Item Class */

/**
 * @brief PRC primitive item for schematic editor
 * @details Represents clock/reset/power primitives matching soc_net format.
 *          Controllers act as containers, inputs/targets connect via wires.
 */
class PrcPrimitiveItem : public QSchematic::Items::Node
{
    Q_OBJECT

public:
    /* QGraphicsItem type identifier */
    static constexpr int Type = QGraphicsItem::UserType + 100;

    /**
     * @brief Construct a PRC primitive item
     * @param[in] primitiveType Type of the primitive
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
     * @brief Check if this primitive is a controller (container)
     * @return true if controller type
     */
    bool isController() const;

    /**
     * @brief Get human-readable type name
     * @return Type name string (e.g., "Clock Controller")
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
     * @brief Get mutable primitive parameters
     * @return Reference to parameter variant
     */
    PrcParams &params();

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
     * @brief Check if item needs initial configuration dialog
     * @return true if newly created via drag-drop and not yet configured
     */
    bool needsConfiguration() const;

    /**
     * @brief Set whether item needs initial configuration
     * @param[in] needs true if needs configuration dialog
     */
    void setNeedsConfiguration(bool needs);

    /**
     * @brief Update dynamic input ports based on connection state
     * @details For ClockTarget/ResetTarget, ensures there's always one
     *          available (unconnected) input port. Called when netlist changes.
     */
    void updateDynamicPorts();

    /**
     * @brief Get the number of input ports
     * @return Number of input connectors
     */
    int inputPortCount() const;

    /**
     * @brief Get the number of connected input ports
     * @return Number of input connectors with wire connections
     */
    int connectedInputPortCount() const;

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
     * @brief Get background color for primitive type
     * @return Background color
     */
    QColor getBackgroundColor() const;

    /**
     * @brief Get border color for primitive type
     * @return Border color
     */
    QColor getBorderColor() const;

    PrimitiveType                             m_primitiveType;        /**< Primitive type */
    QString                                   m_primitiveName;        /**< Display name */
    PrcParams                                 m_params;               /**< Type-safe parameters */
    std::shared_ptr<QSchematic::Items::Label> m_label;                /**< Name label */
    QList<std::shared_ptr<PrcConnector>>      m_connectors;           /**< Connector list */
    bool                                      m_needsConfiguration{}; /**< Needs config dialog */

    /* Dimension constants */
    static constexpr qreal CTRL_WIDTH   = 200.0; /**< Controller width */
    static constexpr qreal CTRL_HEIGHT  = 150.0; /**< Controller height */
    static constexpr qreal ITEM_WIDTH   = 100.0; /**< Input/target width */
    static constexpr qreal ITEM_HEIGHT  = 60.0;  /**< Input/target height */
    static constexpr qreal LABEL_HEIGHT = 20.0;  /**< Label area height */
};

} // namespace PrcLibrary

#endif // PRCPRIMITIVEITEM_H
