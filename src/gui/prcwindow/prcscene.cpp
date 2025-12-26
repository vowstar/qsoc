// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcscene.h"

#include <qschematic/items/node.hpp>

#include <QFont>
#include <QGraphicsSceneContextMenuEvent>
#include <QMenu>
#include <QPen>

using namespace PrcLibrary;

PrcScene::PrcScene(QObject *parent)
    : QSchematic::Scene(parent)
{}

/* Clock Controller Management */

void PrcScene::setClockController(const QString &name, const ClockControllerDef &def)
{
    clockControllers[name] = def;
    update();
}

ClockControllerDef PrcScene::clockController(const QString &name) const
{
    return clockControllers.value(name, ClockControllerDef{name});
}

bool PrcScene::hasClockController(const QString &name) const
{
    return clockControllers.contains(name);
}

void PrcScene::removeClockController(const QString &name)
{
    clockControllers.remove(name);
    update();
}

QStringList PrcScene::clockControllerNames() const
{
    return clockControllers.keys();
}

/* Reset Controller Management */

void PrcScene::setResetController(const QString &name, const ResetControllerDef &def)
{
    resetControllers[name] = def;
    update();
}

ResetControllerDef PrcScene::resetController(const QString &name) const
{
    return resetControllers.value(name, ResetControllerDef{name});
}

bool PrcScene::hasResetController(const QString &name) const
{
    return resetControllers.contains(name);
}

void PrcScene::removeResetController(const QString &name)
{
    resetControllers.remove(name);
    update();
}

QStringList PrcScene::resetControllerNames() const
{
    return resetControllers.keys();
}

/* Power Controller Management */

void PrcScene::setPowerController(const QString &name, const PowerControllerDef &def)
{
    powerControllers[name] = def;
    update();
}

PowerControllerDef PrcScene::powerController(const QString &name) const
{
    return powerControllers.value(name, PowerControllerDef{name});
}

bool PrcScene::hasPowerController(const QString &name) const
{
    return powerControllers.contains(name);
}

void PrcScene::removePowerController(const QString &name)
{
    powerControllers.remove(name);
    update();
}

QStringList PrcScene::powerControllerNames() const
{
    return powerControllers.keys();
}

/* Session-level STA Guide Cell memory */

QString PrcScene::getLastStaGuideCell() const
{
    return lastStaGuideCell;
}

void PrcScene::setLastStaGuideCell(const QString &cell)
{
    if (!cell.isEmpty()) {
        lastStaGuideCell = cell;
    }
}

/* Serialization */

gpds::container PrcScene::to_container() const
{
    gpds::container c = QSchematic::Scene::to_container();

    /* Serialize clock controllers */
    c.add_value("clock_ctrl_count", static_cast<int>(clockControllers.size()));
    int idx = 0;
    for (auto it = clockControllers.begin(); it != clockControllers.end(); ++it, ++idx) {
        std::string prefix = "clock_ctrl_" + std::to_string(idx);
        c.add_value(prefix + "_name", it->name.toStdString());
        c.add_value(prefix + "_test_enable", it->test_enable.toStdString());
    }

    /* Serialize reset controllers */
    c.add_value("reset_ctrl_count", static_cast<int>(resetControllers.size()));
    idx = 0;
    for (auto it = resetControllers.begin(); it != resetControllers.end(); ++it, ++idx) {
        std::string prefix = "reset_ctrl_" + std::to_string(idx);
        c.add_value(prefix + "_name", it->name.toStdString());
        c.add_value(prefix + "_test_enable", it->test_enable.toStdString());
    }

    /* Serialize power controllers */
    c.add_value("power_ctrl_count", static_cast<int>(powerControllers.size()));
    idx = 0;
    for (auto it = powerControllers.begin(); it != powerControllers.end(); ++it, ++idx) {
        std::string prefix = "power_ctrl_" + std::to_string(idx);
        c.add_value(prefix + "_name", it->name.toStdString());
        c.add_value(prefix + "_host_clock", it->host_clock.toStdString());
        c.add_value(prefix + "_host_reset", it->host_reset.toStdString());
        c.add_value(prefix + "_test_enable", it->test_enable.toStdString());
    }

    return c;
}

void PrcScene::from_container(const gpds::container &container)
{
    QSchematic::Scene::from_container(container);

    /* Clear existing controllers */
    clockControllers.clear();
    resetControllers.clear();
    powerControllers.clear();

    /* Deserialize clock controllers */
    int clockCount = container.get_value<int>("clock_ctrl_count").value_or(0);
    for (int i = 0; i < clockCount; ++i) {
        std::string        prefix = "clock_ctrl_" + std::to_string(i);
        ClockControllerDef def;
        def.name = QString::fromStdString(
            container.get_value<std::string>(prefix + "_name").value_or(""));
        def.test_enable = QString::fromStdString(
            container.get_value<std::string>(prefix + "_test_enable").value_or(""));
        if (!def.name.isEmpty()) {
            clockControllers[def.name] = def;
        }
    }

    /* Deserialize reset controllers */
    int resetCount = container.get_value<int>("reset_ctrl_count").value_or(0);
    for (int i = 0; i < resetCount; ++i) {
        std::string        prefix = "reset_ctrl_" + std::to_string(i);
        ResetControllerDef def;
        def.name = QString::fromStdString(
            container.get_value<std::string>(prefix + "_name").value_or(""));
        def.test_enable = QString::fromStdString(
            container.get_value<std::string>(prefix + "_test_enable").value_or(""));
        if (!def.name.isEmpty()) {
            resetControllers[def.name] = def;
        }
    }

    /* Deserialize power controllers */
    int powerCount = container.get_value<int>("power_ctrl_count").value_or(0);
    for (int i = 0; i < powerCount; ++i) {
        std::string        prefix = "power_ctrl_" + std::to_string(i);
        PowerControllerDef def;
        def.name = QString::fromStdString(
            container.get_value<std::string>(prefix + "_name").value_or(""));
        def.host_clock = QString::fromStdString(
            container.get_value<std::string>(prefix + "_host_clock").value_or(""));
        def.host_reset = QString::fromStdString(
            container.get_value<std::string>(prefix + "_host_reset").value_or(""));
        def.test_enable = QString::fromStdString(
            container.get_value<std::string>(prefix + "_test_enable").value_or(""));
        if (!def.name.isEmpty()) {
            powerControllers[def.name] = def;
        }
    }
}

/* Drawing */

void PrcScene::drawForeground(QPainter *painter, const QRectF &rect)
{
    QSchematic::Scene::drawForeground(painter, rect);

    painter->save();

    /* Collect unique controller names from elements */
    QSet<QString> clockCtrlNames;
    QSet<QString> resetCtrlNames;
    QSet<QString> powerCtrlNames;

    for (const auto &node : nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcPrimitiveItem>(node);
        if (!prcItem) {
            continue;
        }

        const auto &params = prcItem->params();

        if (std::holds_alternative<ClockInputParams>(params)) {
            const auto &p = std::get<ClockInputParams>(params);
            if (!p.controller.isEmpty()) {
                clockCtrlNames.insert(p.controller);
            }
        } else if (std::holds_alternative<ClockTargetParams>(params)) {
            const auto &p = std::get<ClockTargetParams>(params);
            if (!p.controller.isEmpty()) {
                clockCtrlNames.insert(p.controller);
            }
        } else if (std::holds_alternative<ResetSourceParams>(params)) {
            const auto &p = std::get<ResetSourceParams>(params);
            if (!p.controller.isEmpty()) {
                resetCtrlNames.insert(p.controller);
            }
        } else if (std::holds_alternative<ResetTargetParams>(params)) {
            const auto &p = std::get<ResetTargetParams>(params);
            if (!p.controller.isEmpty()) {
                resetCtrlNames.insert(p.controller);
            }
        } else if (std::holds_alternative<PowerDomainParams>(params)) {
            const auto &p = std::get<PowerDomainParams>(params);
            if (!p.controller.isEmpty()) {
                powerCtrlNames.insert(p.controller);
            }
        }
    }

    /* Draw clock controller frames */
    QSet<int> clockTypes = {ClockInput, ClockTarget};
    for (const QString &name : clockCtrlNames) {
        QRectF bounds = calculateControllerBounds(name, clockTypes);
        if (!bounds.isNull()) {
            drawControllerFrame(painter, bounds, name, controllerColor(true, false, false));
        }
    }

    /* Draw reset controller frames */
    QSet<int> resetTypes = {ResetSource, ResetTarget};
    for (const QString &name : resetCtrlNames) {
        QRectF bounds = calculateControllerBounds(name, resetTypes);
        if (!bounds.isNull()) {
            drawControllerFrame(painter, bounds, name, controllerColor(false, true, false));
        }
    }

    /* Draw power controller frames */
    QSet<int> powerTypes = {PowerDomain};
    for (const QString &name : powerCtrlNames) {
        QRectF bounds = calculateControllerBounds(name, powerTypes);
        if (!bounds.isNull()) {
            drawControllerFrame(painter, bounds, name, controllerColor(false, false, true));
        }
    }

    painter->restore();
}

QRectF PrcScene::calculateControllerBounds(
    const QString &controllerName, const QSet<int> &primitiveTypes) const
{
    QRectF bounds;
    bool   first = true;

    for (const auto &node : nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcPrimitiveItem>(node);
        if (!prcItem) {
            continue;
        }

        if (!primitiveTypes.contains(static_cast<int>(prcItem->primitiveType()))) {
            continue;
        }

        /* Get controller name from params */
        QString     itemController;
        const auto &params = prcItem->params();

        if (std::holds_alternative<ClockInputParams>(params)) {
            itemController = std::get<ClockInputParams>(params).controller;
        } else if (std::holds_alternative<ClockTargetParams>(params)) {
            itemController = std::get<ClockTargetParams>(params).controller;
        } else if (std::holds_alternative<ResetSourceParams>(params)) {
            itemController = std::get<ResetSourceParams>(params).controller;
        } else if (std::holds_alternative<ResetTargetParams>(params)) {
            itemController = std::get<ResetTargetParams>(params).controller;
        } else if (std::holds_alternative<PowerDomainParams>(params)) {
            itemController = std::get<PowerDomainParams>(params).controller;
        }

        if (itemController != controllerName) {
            continue;
        }

        /* Expand bounds to include this item */
        QRectF itemBounds = prcItem->sceneBoundingRect();
        if (first) {
            bounds = itemBounds;
            first  = false;
        } else {
            bounds = bounds.united(itemBounds);
        }
    }

    if (!bounds.isNull()) {
        bounds.adjust(-FRAME_PADDING, -FRAME_PADDING, FRAME_PADDING, FRAME_PADDING);
    }

    return bounds;
}

QColor PrcScene::controllerColor(
    bool isClockController, bool isResetController, bool isPowerController) const
{
    if (isClockController) {
        return QColor(70, 130, 180); /* Steel blue for clock */
    } else if (isResetController) {
        return QColor(220, 20, 60); /* Crimson for reset */
    } else if (isPowerController) {
        return QColor(34, 139, 34); /* Forest green for power */
    }
    return QColor(128, 128, 128); /* Gray default */
}

void PrcScene::drawControllerFrame(
    QPainter *painter, const QRectF &bounds, const QString &name, const QColor &color) const
{
    /* Draw dashed frame */
    QPen pen(color);
    pen.setWidth(2);
    pen.setStyle(Qt::DashLine);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(bounds, FRAME_CORNER, FRAME_CORNER);

    /* Draw label background */
    QFont font = painter->font();
    font.setBold(true);
    painter->setFont(font);

    QFontMetricsF fm(font);
    QRectF        labelRect = fm.boundingRect(name);
    labelRect.adjust(-4, -2, 4, 2);
    labelRect.moveTopLeft(bounds.topLeft() + QPointF(LABEL_OFFSET, LABEL_OFFSET));

    /* Semi-transparent background */
    QColor bgColor = color.lighter(180);
    bgColor.setAlpha(200);
    painter->fillRect(labelRect, bgColor);

    /* Draw border around label */
    QPen labelPen(color);
    labelPen.setWidth(1);
    labelPen.setStyle(Qt::SolidLine);
    painter->setPen(labelPen);
    painter->drawRect(labelRect);

    /* Draw label text */
    painter->setPen(color.darker(150));
    painter->drawText(labelRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter, name);
}

bool PrcScene::findControllerAtPos(const QPointF &pos, ControllerType &type, QString &name) const
{
    /* Check clock controllers */
    QSet<int> clockTypes = {ClockInput, ClockTarget};
    for (const QString &ctrlName : clockControllers.keys()) {
        QRectF bounds = calculateControllerBounds(ctrlName, clockTypes);
        if (!bounds.isNull() && bounds.contains(pos)) {
            type = ClockCtrl;
            name = ctrlName;
            return true;
        }
    }

    /* Check reset controllers */
    QSet<int> resetTypes = {ResetSource, ResetTarget};
    for (const QString &ctrlName : resetControllers.keys()) {
        QRectF bounds = calculateControllerBounds(ctrlName, resetTypes);
        if (!bounds.isNull() && bounds.contains(pos)) {
            type = ResetCtrl;
            name = ctrlName;
            return true;
        }
    }

    /* Check power controllers */
    QSet<int> powerTypes = {PowerDomain};
    for (const QString &ctrlName : powerControllers.keys()) {
        QRectF bounds = calculateControllerBounds(ctrlName, powerTypes);
        if (!bounds.isNull() && bounds.contains(pos)) {
            type = PowerCtrl;
            name = ctrlName;
            return true;
        }
    }

    return false;
}

void PrcScene::contextMenuEvent(QGraphicsSceneContextMenuEvent *event)
{
    /* Check if click is on a controller frame (not on an item) */
    QGraphicsItem *itemUnderMouse = itemAt(event->scenePos(), QTransform());

    /* If there's an item under mouse, let default handling proceed */
    if (itemUnderMouse) {
        QSchematic::Scene::contextMenuEvent(event);
        return;
    }

    /* Check if we're in a controller frame */
    ControllerType type;
    QString        name;
    if (findControllerAtPos(event->scenePos(), type, name)) {
        QMenu menu;

        QString typeStr;
        switch (type) {
        case ClockCtrl:
            typeStr = tr("Clock");
            break;
        case ResetCtrl:
            typeStr = tr("Reset");
            break;
        case PowerCtrl:
            typeStr = tr("Power");
            break;
        }

        auto *editAction = menu.addAction(tr("Edit %1 Controller '%2'...").arg(typeStr, name));
        connect(editAction, &QAction::triggered, [this, type, name]() {
            emit editControllerRequested(static_cast<int>(type), name);
        });

        menu.exec(event->screenPos());
        event->accept();
        return;
    }

    /* Let default handling proceed */
    QSchematic::Scene::contextMenuEvent(event);
}
