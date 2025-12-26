// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCSCENE_H
#define PRCSCENE_H

#include "prcprimitiveitem.h"

#include <qschematic/scene.hpp>

#include <QColor>
#include <QMap>
#include <QPainter>
#include <QRectF>
#include <QString>

namespace PrcLibrary {

/**
 * @brief Extended scene for PRC (Power/Reset/Clock) editing
 * @details Provides Scene-level controller definition storage and
 *          auto-drawing of controller frames around grouped elements.
 */
class PrcScene : public QSchematic::Scene
{
    Q_OBJECT

public:
    /**
     * @brief Controller type for context menu
     */
    enum ControllerType { ClockCtrl, ResetCtrl, PowerCtrl };

    /**
     * @brief Construct PRC scene
     * @param[in] parent Parent QObject
     */
    explicit PrcScene(QObject *parent = nullptr);

    ~PrcScene() override = default;

signals:
    /**
     * @brief Signal emitted when user requests to edit a controller
     * @param[in] type Controller type
     * @param[in] name Controller name
     */
    void editControllerRequested(int type, const QString &name);

public:
    /* Clock Controller Management */

    /**
     * @brief Add or update a clock controller definition
     * @param[in] name Controller name
     * @param[in] def Controller definition
     */
    void setClockController(const QString &name, const ClockControllerDef &def);

    /**
     * @brief Get clock controller definition
     * @param[in] name Controller name
     * @return Controller definition (default if not found)
     */
    ClockControllerDef clockController(const QString &name) const;

    /**
     * @brief Check if clock controller exists
     * @param[in] name Controller name
     * @return true if exists
     */
    bool hasClockController(const QString &name) const;

    /**
     * @brief Remove clock controller definition
     * @param[in] name Controller name
     */
    void removeClockController(const QString &name);

    /**
     * @brief Get all clock controller names
     * @return List of controller names
     */
    QStringList clockControllerNames() const;

    /* Reset Controller Management */

    /**
     * @brief Add or update a reset controller definition
     * @param[in] name Controller name
     * @param[in] def Controller definition
     */
    void setResetController(const QString &name, const ResetControllerDef &def);

    /**
     * @brief Get reset controller definition
     * @param[in] name Controller name
     * @return Controller definition (default if not found)
     */
    ResetControllerDef resetController(const QString &name) const;

    /**
     * @brief Check if reset controller exists
     * @param[in] name Controller name
     * @return true if exists
     */
    bool hasResetController(const QString &name) const;

    /**
     * @brief Remove reset controller definition
     * @param[in] name Controller name
     */
    void removeResetController(const QString &name);

    /**
     * @brief Get all reset controller names
     * @return List of controller names
     */
    QStringList resetControllerNames() const;

    /* Power Controller Management */

    /**
     * @brief Add or update a power controller definition
     * @param[in] name Controller name
     * @param[in] def Controller definition
     */
    void setPowerController(const QString &name, const PowerControllerDef &def);

    /**
     * @brief Get power controller definition
     * @param[in] name Controller name
     * @return Controller definition (default if not found)
     */
    PowerControllerDef powerController(const QString &name) const;

    /**
     * @brief Check if power controller exists
     * @param[in] name Controller name
     * @return true if exists
     */
    bool hasPowerController(const QString &name) const;

    /**
     * @brief Remove power controller definition
     * @param[in] name Controller name
     */
    void removePowerController(const QString &name);

    /**
     * @brief Get all power controller names
     * @return List of controller names
     */
    QStringList powerControllerNames() const;

    /* Serialization */

    /**
     * @brief Serialize scene to GPDS container
     * @return GPDS container with serialized data
     */
    gpds::container to_container() const override;

    /**
     * @brief Deserialize scene from GPDS container
     * @param[in] container GPDS container with serialized data
     */
    void from_container(const gpds::container &container) override;

protected:
    /**
     * @brief Draw foreground with controller frames
     * @param[in] painter QPainter instance
     * @param[in] rect Visible rect
     */
    void drawForeground(QPainter *painter, const QRectF &rect) override;

    /**
     * @brief Handle context menu event for controller frames
     * @param[in] event Context menu event
     */
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;

private:
    /**
     * @brief Calculate bounding rect for elements with given controller
     * @param[in] controllerName Controller name to match
     * @param[in] primitiveTypes Types to include (for filtering)
     * @return Bounding rectangle (null if no elements)
     */
    QRectF calculateControllerBounds(
        const QString &controllerName, const QSet<int> &primitiveTypes) const;

    /**
     * @brief Find controller at scene position
     * @param[in] pos Scene position
     * @param[out] type Controller type
     * @param[out] name Controller name
     * @return true if controller found at position
     */
    bool findControllerAtPos(const QPointF &pos, ControllerType &type, QString &name) const;

    /**
     * @brief Get controller color by type
     * @param[in] isClockController true for clock
     * @param[in] isResetController true for reset
     * @param[in] isPowerController true for power
     * @return Frame color
     */
    QColor controllerColor(
        bool isClockController, bool isResetController, bool isPowerController) const;

    /**
     * @brief Draw a single controller frame
     * @param[in] painter QPainter instance
     * @param[in] bounds Bounding rectangle
     * @param[in] name Controller name
     * @param[in] color Frame color
     */
    void drawControllerFrame(
        QPainter *painter, const QRectF &bounds, const QString &name, const QColor &color) const;

    /* Controller definition storage */
    QMap<QString, ClockControllerDef> clockControllers_;
    QMap<QString, ResetControllerDef> resetControllers_;
    QMap<QString, PowerControllerDef> powerControllers_;

    /* Drawing constants */
    static constexpr qreal FRAME_PADDING = 20.0; /**< Padding around elements */
    static constexpr qreal FRAME_CORNER  = 8.0;  /**< Corner radius */
    static constexpr qreal LABEL_OFFSET  = 5.0;  /**< Label offset from corner */
};

} // namespace PrcLibrary

#endif // PRCSCENE_H
