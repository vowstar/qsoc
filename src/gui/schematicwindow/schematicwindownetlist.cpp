// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/customwire.h"
#include "gui/schematicwindow/modulelibrary/socmoduleconnector.h"
#include "gui/schematicwindow/modulelibrary/socmoduleitem.h"
#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"

#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSet>
#include <QStandardPaths>

#include <fstream>
#include <yaml-cpp/yaml.h>
#include <qschematic/commands/item_remove.hpp>
#include <qschematic/commands/wirenet_rename.hpp>
#include <qschematic/items/node.hpp>
#include <qschematic/items/wire.hpp>
#include <qschematic/items/wirenet.hpp>
#include <qschematic/netlist.hpp>
#include <qschematic/netlistgenerator.hpp>
#include <qschematic/scene.hpp>

#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/commands/instance_rename.h"

bool SchematicWindow::eventFilter(QObject *watched, QEvent *event)
{
    /* Fix: Prevent Delete key from being consumed by ShortcutOverride
     * Qt sends ShortcutOverride before KeyPress to allow widgets to override shortcuts.
     * If we don't accept it here, parent widgets' shortcuts will consume the Delete key,
     * preventing QSchematic::View's built-in Delete handling from working.
     */
    if (watched == ui->schematicView && event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            event->accept(); // Tell Qt we want this key
            return true;     // Stop propagation to prevent parent shortcuts from blocking
        }
    }

    if (watched == ui->schematicView->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        /* Handle double-click */
        {
            auto          *mouseEvent = static_cast<QMouseEvent *>(event);
            QPointF        scenePos   = ui->schematicView->mapToScene(mouseEvent->pos());
            QGraphicsItem *item       = scene.itemAt(scenePos, ui->schematicView->transform());

            if (!item) {
                return QMainWindow::eventFilter(watched, event);
            }

            /* Check if it's a SocModuleItem */
            auto *socItem = dynamic_cast<ModuleLibrary::SocModuleItem *>(item);
            if (socItem) {
                handleLabelDoubleClick(socItem);
                return true;
            }

            /* Check if it's a label */
            auto *label = qgraphicsitem_cast<QSchematic::Items::Label *>(item);
            if (label) {
                auto *parent = label->parentItem();

                /* Search up the parent hierarchy */
                while (parent) {
                    /* Check if parent is SocModuleItem (most common case) */
                    auto *socModuleItem = dynamic_cast<ModuleLibrary::SocModuleItem *>(parent);
                    if (socModuleItem) {
                        handleLabelDoubleClick(socModuleItem);
                        return true;
                    }

                    /* Check if parent is WireNet */
                    auto *wireNet = dynamic_cast<QSchematic::Items::WireNet *>(parent);
                    if (wireNet) {
                        handleWireDoubleClick(wireNet);
                        return true;
                    }

                    /* Check if parent is Wire */
                    auto *wire = qgraphicsitem_cast<QSchematic::Items::Wire *>(parent);
                    if (wire) {
                        auto net = wire->net();
                        if (net) {
                            auto *wireNet = dynamic_cast<QSchematic::Items::WireNet *>(net.get());
                            if (wireNet) {
                                handleWireDoubleClick(wireNet);
                                return true;
                            }
                        }
                    }

                    parent = parent->parentItem();
                }
            }

            /* Check if it's a wire */
            auto *wire = qgraphicsitem_cast<QSchematic::Items::Wire *>(item);
            if (wire) {
                auto net = wire->net();
                if (net) {
                    auto *wireNet = dynamic_cast<QSchematic::Items::WireNet *>(net.get());
                    if (wireNet) {
                        handleWireDoubleClick(wireNet);
                        return true;
                    }
                }
            }

            /* Default: event not handled, but don't crash */
            return false;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void SchematicWindow::on_actionExportNetlist_triggered()
{
    if (!projectManager) {
        QMessageBox::warning(this, tr("Export Error"), tr("No project manager available"));
        return;
    }

    /* Determine default path and filename */
    QString defaultPath = projectManager->getOutputPath();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    /* Generate default filename from current schematic file */
    QString defaultFileName;
    if (!m_currentFilePath.isEmpty()) {
        QFileInfo fileInfo(m_currentFilePath);
        QString   baseName = fileInfo.completeBaseName(); // Remove .soc_sch
        defaultFileName    = QDir(defaultPath).filePath(baseName + ".soc_net");
    } else {
        defaultFileName = defaultPath;
    }

    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Export Netlist"), defaultFileName, tr("SOC Netlist Files (*.soc_net)"));

    if (fileName.isEmpty()) {
        return;
    }

    if (!fileName.endsWith(".soc_net")) {
        fileName += ".soc_net";
    }

    if (exportNetlist(fileName)) {
        QMessageBox::information(
            this, tr("Export Success"), tr("Netlist exported successfully to %1").arg(fileName));
    } else {
        QMessageBox::critical(this, tr("Export Error"), tr("Failed to export netlist"));
    }
}

void SchematicWindow::handleLabelDoubleClick(ModuleLibrary::SocModuleItem *socItem)
{
    if (!socItem) {
        return;
    }

    QString currentName = socItem->instanceName();

    bool    ok;
    QString newName = QInputDialog::getText(
        this, tr("Rename Instance"), tr("Enter instance name:"), QLineEdit::Normal, currentName, &ok);

    if (ok && !newName.isEmpty() && newName != currentName) {
        /* Check if name already exists */
        for (const auto &node : scene.nodes()) {
            auto otherItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
            if (otherItem && otherItem.get() != socItem && otherItem->instanceName() == newName) {
                QMessageBox::warning(
                    this, tr("Rename Error"), tr("Instance name '%1' already exists").arg(newName));
                return;
            }
        }

        /* Use undo command for renamable operation */
        /* Find the shared_ptr for this socItem */
        for (const auto &node : scene.nodes()) {
            auto socItemShared = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
            if (socItemShared && socItemShared.get() == socItem) {
                scene.undoStack()->push(
                    new SchematicCommands::InstanceRename(socItemShared, newName));
                break;
            }
        }
    }
}

QPointF SchematicWindow::getWireStartPos(const QSchematic::Items::WireNet *wireNet) const
{
    if (!wireNet) {
        return QPointF();
    }

    for (const auto &wire : wireNet->wires()) {
        auto qsWire = std::dynamic_pointer_cast<QSchematic::Items::Wire>(wire);
        if (qsWire && qsWire->points_count() > 0) {
            return qsWire->scenePos() + qsWire->pointsRelative().first();
        }
    }

    return QPointF();
}

void SchematicWindow::autoNameWires()
{
    auto wm = scene.wire_manager();
    if (!wm) {
        return;
    }

    for (const auto &net : wm->nets()) {
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
        if (!wireNet) {
            continue;
        }

        /* Update bus flag for all wires in this net */
        bool        isBusNet  = false;
        const qreal tolerance = 5.0;

        for (const auto &wire : wireNet->wires()) {
            auto customWire = std::dynamic_pointer_cast<SchematicCustom::CustomWire>(wire);
            if (!customWire || customWire->points_count() < 2) {
                continue;
            }

            /* Check if any connector attached to this wire is a bus */
            for (const auto &node : scene.nodes()) {
                auto socItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
                if (!socItem) {
                    continue;
                }

                for (const auto &connector : node->connectors()) {
                    if (!connector) {
                        continue;
                    }

                    auto socConnector
                        = std::dynamic_pointer_cast<ModuleLibrary::SocModuleConnector>(connector);
                    if (!socConnector
                        || socConnector->portType() != ModuleLibrary::SocModuleConnector::Bus) {
                        continue;
                    }

                    /* Check if wire connects to this bus connector */
                    QPointF connectorPos = connector->scenePos();
                    QPointF wireStart    = customWire->scenePos()
                                        + customWire->pointsRelative().first();
                    QPointF wireEnd = customWire->scenePos() + customWire->pointsRelative().last();

                    if (QLineF(connectorPos, wireStart).length() < tolerance
                        || QLineF(connectorPos, wireEnd).length() < tolerance) {
                        isBusNet = true;
                        break;
                    }
                }
                if (isBusNet) {
                    break;
                }
            }
            if (isBusNet) {
                break;
            }
        }

        /* Set bus flag for all wires in this net */
        for (const auto &wire : wireNet->wires()) {
            auto customWire = std::dynamic_pointer_cast<SchematicCustom::CustomWire>(wire);
            if (customWire) {
                customWire->setBusWire(isBusNet);
            }
        }

        /* Auto-naming logic - only for unnamed nets */
        if (!wireNet->name().isEmpty()) {
            continue;
        }

        QString generatedName = autoGenerateWireName(wireNet.get());
        if (generatedName.isEmpty() || generatedName == "unnamed") {
            continue;
        }

        /* Set label position based on port direction (always horizontal) */
        ConnectionInfo connInfo = findStartConnection(wireNet.get());
        QPointF        startPos = getWireStartPos(wireNet.get());
        if (!startPos.isNull() && wireNet->label()) {
            auto label = wireNet->label();

            /* Temporarily set the name to calculate label dimensions */
            label->setText(generatedName);
            qreal labelWidth  = label->boundingRect().width();
            qreal labelHeight = label->boundingRect().height();

            QPointF labelPos;

            /* Use Position enum from SocModuleConnector */
            using Pos    = ModuleLibrary::SocModuleConnector::Position;
            auto portPos = static_cast<Pos>(connInfo.portPosition);

            /* All directions align to port position (startPos) */
            switch (portPos) {
            case Pos::Left:
                /* Port on left: label to the left, right-aligned, Y aligns with port */
                labelPos.setX(startPos.x() - labelWidth);
                labelPos.setY(startPos.y() - labelHeight / 2);
                break;

            case Pos::Right:
                /* Port on right: label to the right, left-aligned, Y aligns with port */
                labelPos.setX(startPos.x());
                labelPos.setY(startPos.y() - labelHeight / 2);
                break;

            case Pos::Top:
                /* Port on top: label above, X aligns with port, Y centered at port */
                labelPos.setX(startPos.x() - labelWidth / 2);
                labelPos.setY(startPos.y() - labelHeight / 2);
                break;

            case Pos::Bottom:
                /* Port on bottom: label below, X aligns with port, shifted down by labelHeight */
                labelPos.setX(startPos.x() - labelWidth / 2);
                labelPos.setY(startPos.y() + labelHeight);
                break;
            }

            label->setRotation(0.0);
            label->setPos(labelPos);
        }

        wireNet->set_name(generatedName);
    }
}

void SchematicWindow::handleWireDoubleClick(QSchematic::Items::WireNet *wireNet)
{
    if (!wireNet) {
        return;
    }

    QString currentName = wireNet->name();
    if (currentName.isEmpty()) {
        currentName = autoGenerateWireName(wireNet);
    }

    bool    ok;
    QString newName = QInputDialog::getText(
        this, tr("Rename Wire/Net"), tr("Enter net name:"), QLineEdit::Normal, currentName, &ok);

    if (ok && !newName.isEmpty() && newName != wireNet->name()) {
        /* Use undo command for renamable operation */
        auto wm = scene.wire_manager();
        if (wm) {
            /* Find the shared_ptr for this wireNet */
            for (const auto &net : wm->nets()) {
                auto wireNetShared = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
                if (wireNetShared && wireNetShared.get() == wireNet) {
                    scene.undoStack()->push(
                        new QSchematic::Commands::WirenetRename(wireNetShared, newName));
                    break;
                }
            }
        }
    }
}

QSet<QString> SchematicWindow::getExistingWireNames() const
{
    QSet<QString> existingNames;
    auto          wm = scene.wire_manager();
    if (!wm) {
        return existingNames;
    }

    for (const auto &net : wm->nets()) {
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
        if (wireNet && !wireNet->name().isEmpty()) {
            existingNames.insert(wireNet->name());
        }
    }
    return existingNames;
}

SchematicWindow::ConnectionInfo SchematicWindow::findStartConnection(
    const QSchematic::Items::WireNet *wireNet) const
{
    ConnectionInfo info;
    if (!wireNet) {
        return info;
    }

    auto wm = scene.wire_manager();
    if (!wm) {
        return info;
    }

    /* Get wire start position */
    QPointF startPos;
    for (const auto &wire : wireNet->wires()) {
        auto qsWire = std::dynamic_pointer_cast<QSchematic::Items::Wire>(wire);
        if (qsWire && qsWire->points_count() > 0) {
            startPos = qsWire->scenePos() + qsWire->pointsRelative().first();
            break;
        }
    }

    if (startPos.isNull()) {
        return info;
    }

    /* Find connector at start position */
    const qreal tolerance = 5.0; // Grid tolerance
    for (const auto &node : scene.nodes()) {
        auto socItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
        if (!socItem) {
            continue;
        }

        for (const auto &connector : node->connectors()) {
            if (!connector) {
                continue;
            }

            /* Check if connector is at wire start position */
            QPointF connectorPos = connector->scenePos();
            qreal   distance     = QLineF(connectorPos, startPos).length();

            if (distance < tolerance) {
                /* Found the start connector */
                info.instanceName = socItem->instanceName();
                info.portName     = connector->text();
                if (info.portName.isEmpty() && connector->label()) {
                    info.portName = connector->label()->text();
                }

                /* Get port position (Left/Right/Top/Bottom) */
                auto socConnector = std::dynamic_pointer_cast<ModuleLibrary::SocModuleConnector>(
                    connector);
                if (socConnector) {
                    info.portPosition = static_cast<int>(socConnector->modulePosition());
                } else {
                    info.portPosition = static_cast<int>(ModuleLibrary::SocModuleConnector::Right);
                }

                return info;
            }
        }
    }

    return info;
}

QString SchematicWindow::autoGenerateWireName(const QSchematic::Items::WireNet *wireNet) const
{
    if (!wireNet) {
        return QString();
    }

    /* Find start connection */
    ConnectionInfo connInfo = findStartConnection(wireNet);
    if (connInfo.instanceName.isEmpty()) {
        return QString("unnamed");
    }

    /* Generate base name */
    QString baseName = connInfo.portName.isEmpty()
                           ? connInfo.instanceName
                           : connInfo.instanceName + "_" + connInfo.portName;

    /* Make it unique */
    const QSet<QString> existingNames = getExistingWireNames();
    QString             finalName     = baseName;
    int                 suffix        = 0;

    while (existingNames.contains(finalName)) {
        finalName = QString("%1_%2").arg(baseName).arg(++suffix);
    }

    return finalName;
}

bool SchematicWindow::exportNetlist(const QString &filePath)
{
    /* Generate netlist from scene */
    QSchematic::Netlist<QSchematic::Items::Node *, QSchematic::Items::Connector *> netlist;
    if (!QSchematic::NetlistGenerator::generate(netlist, scene)) {
        qWarning() << "Failed to generate netlist from scene";
        return false;
    }

    /* Build instance map: instance_name -> { module_type, ports, buses } */
    struct PortConnection
    {
        QString portName;
        QString netName;
        bool    isBus; // true if this is a bus connection
    };

    struct InstanceInfo
    {
        QString               moduleName;
        QList<PortConnection> ports;
        QList<PortConnection> buses;
    };

    QMap<QString, InstanceInfo> instances;

    /* Process all nets */
    for (const auto &net : netlist.nets) {
        QString netName = net.name;
        if (netName.isEmpty()) {
            continue; /* Skip unnamed nets */
        }

        /* For each connector in this net, add port connection to its instance */
        for (const auto &connectorNodePair : net.connectorNodePairs) {
            auto connector = connectorNodePair.first;
            auto node      = connectorNodePair.second;

            if (!connector || !node) {
                continue;
            }

            /* Get real instance name and module name from SocModuleItem */
            auto    socItem = dynamic_cast<ModuleLibrary::SocModuleItem *>(node);
            QString instanceName;
            QString moduleName;

            if (socItem) {
                instanceName = socItem->instanceName();
                moduleName   = socItem->moduleName();
            } else {
                /* Fallback for non-SocModuleItem nodes */
                instanceName = QString("node_%1").arg(quintptr(node), 0, 16);
                moduleName   = QString("unknown");
            }

            /* Get port name from connector */
            QString portName = connector->text();
            if (portName.isEmpty() && connector->label()) {
                portName = connector->label()->text();
            }
            if (portName.isEmpty()) {
                continue; /* Skip connectors without port names */
            }

            /* Check if this is a bus connector */
            bool isBus        = false;
            auto socConnector = dynamic_cast<ModuleLibrary::SocModuleConnector *>(connector);
            if (socConnector) {
                isBus = (socConnector->portType() == ModuleLibrary::SocModuleConnector::Bus);
            }

            /* Add to instances map */
            if (!instances.contains(instanceName)) {
                instances[instanceName].moduleName = moduleName;
            }

            /* Add port or bus connection */
            PortConnection portConn;
            portConn.portName = portName;
            portConn.netName  = netName;
            portConn.isBus    = isBus;

            if (isBus) {
                instances[instanceName].buses.append(portConn);
            } else {
                instances[instanceName].ports.append(portConn);
            }
        }
    }

    /* Build YAML structure using yaml-cpp */
    YAML::Node root;

    for (auto it = instances.constBegin(); it != instances.constEnd(); ++it) {
        const QString      &instanceName = it.key();
        const InstanceInfo &info         = it.value();

        /* Set module name */
        root["instance"][instanceName.toStdString()]["module"] = info.moduleName.toStdString();

        /* Add port connections */
        if (!info.ports.isEmpty()) {
            for (const auto &portConn : info.ports) {
                root["instance"][instanceName.toStdString()]["port"]
                    [portConn.portName.toStdString()]["link"]
                    = portConn.netName.toStdString();
            }
        }

        /* Add bus connections */
        if (!info.buses.isEmpty()) {
            for (const auto &busConn : info.buses) {
                root["instance"][instanceName.toStdString()]["bus"][busConn.portName.toStdString()]
                    ["link"]
                    = busConn.netName.toStdString();
            }
        }
    }

    /* Write to file */
    std::ofstream fout(filePath.toStdString());
    if (!fout.is_open()) {
        qWarning() << "Failed to open file for writing:" << filePath;
        return false;
    }

    fout << root;
    fout.close();

    if (!fout) {
        qWarning() << "Failed to write netlist to file:" << filePath;
        return false;
    }

    return true;
}
