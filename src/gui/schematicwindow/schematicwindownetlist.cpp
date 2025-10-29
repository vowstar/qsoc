// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

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
#include <QMessageBox>
#include <QMouseEvent>
#include <QSet>
#include <QStandardPaths>

#include <fstream>
#include <yaml-cpp/yaml.h>
#include <qschematic/items/node.hpp>
#include <qschematic/items/wire.hpp>
#include <qschematic/items/wirenet.hpp>
#include <qschematic/netlist.hpp>
#include <qschematic/netlistgenerator.hpp>
#include <qschematic/scene.hpp>

#include "common/qsocprojectmanager.h"

bool SchematicWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->schematicView->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            /* Get item at mouse position */
            QPointF        scenePos = ui->schematicView->mapToScene(mouseEvent->pos());
            QGraphicsItem *item     = scene.itemAt(scenePos, ui->schematicView->transform());

            /* Check if it's a SocModuleItem (module body) */
            auto *socItem = dynamic_cast<ModuleLibrary::SocModuleItem *>(item);
            if (socItem) {
                handleLabelDoubleClick(socItem);
                return true;
            }

            /* Check if it's a label (instance name) */
            auto *label = qgraphicsitem_cast<QSchematic::Items::Label *>(item);
            if (label) {
                /* Find parent SocModuleItem */
                auto *parent = label->parentItem();
                while (parent) {
                    auto *parentSocItem = dynamic_cast<ModuleLibrary::SocModuleItem *>(parent);
                    if (parentSocItem) {
                        handleLabelDoubleClick(parentSocItem);
                        return true;
                    }
                    parent = parent->parentItem();
                }
            }

            /* Check if it's a wire */
            auto *wire = qgraphicsitem_cast<QSchematic::Items::Wire *>(item);
            if (wire && wire->net()) {
                auto *wireNet = dynamic_cast<QSchematic::Items::WireNet *>(wire->net().get());
                if (wireNet) {
                    handleWireDoubleClick(wireNet);
                    return true; /* Event handled */
                }
            }
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

        socItem->setInstanceName(newName);
        qDebug() << "Instance renamed to:" << newName;
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
        if (!wireNet || !wireNet->name().isEmpty()) {
            continue;
        }

        QString generatedName = autoGenerateWireName(wireNet.get());
        if (generatedName.isEmpty() || generatedName == "unnamed") {
            continue;
        }

        /* Set label position at wire start before naming */
        QPointF startPos = getWireStartPos(wireNet.get());
        if (!startPos.isNull()) {
            wireNet->label()->setPos(startPos);
        }

        wireNet->set_name(generatedName);
        qDebug() << "Auto-named wire:" << generatedName;
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

    if (ok && !newName.isEmpty()) {
        wireNet->set_name(newName);
        qDebug() << "Wire renamed to:" << newName;
    }
}

QString SchematicWindow::autoGenerateWireName(const QSchematic::Items::WireNet *wireNet) const
{
    if (!wireNet) {
        return QString();
    }

    /* Get wire manager from scene */
    auto wm = scene.wire_manager();
    if (!wm) {
        return QString("unnamed");
    }

    /* Collect wires from this wireNet */
    QList<const QSchematic::Items::Wire *> netWires;
    for (const auto &wire : wireNet->wires()) {
        auto qsWire = std::dynamic_pointer_cast<QSchematic::Items::Wire>(wire);
        if (qsWire) {
            netWires.append(qsWire.get());
        }
    }

    if (netWires.isEmpty()) {
        return QString("unnamed");
    }

    /* Find the first connector connected to any wire in this net */
    QString baseName;
    bool    found = false;

    for (const auto &node : scene.nodes()) {
        if (!node) {
            continue;
        }

        for (const auto &connector : node->connectors()) {
            if (!connector) {
                continue;
            }

            /* Check if this connector is connected to any wire in our net */
            auto cr = wm->attached_wire(connector.get());
            if (!cr || !cr->wire) {
                continue;
            }

            /* Check if the attached wire belongs to our net */
            if (std::find(netWires.begin(), netWires.end(), cr->wire) != netWires.end()) {
                /* Found the first connection! */

                /* Get instance and port names */
                auto socItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
                if (!socItem) {
                    continue; // Skip non-SocModuleItem nodes
                }

                QString instanceName = socItem->instanceName();
                QString portName     = connector->text();
                if (portName.isEmpty() && connector->label()) {
                    portName = connector->label()->text();
                }

                /* Generate base name: instance_port */
                baseName = portName.isEmpty() ? instanceName : instanceName + "_" + portName;

                found = true;
                break;
            }
        }

        if (found) {
            break;
        }
    }

    /* Only generate name if we found at least one connection */
    if (baseName.isEmpty()) {
        return QString("unnamed");
    }

    /* Check for name conflicts and add numeric suffix if needed */
    QString finalName = baseName;
    int     suffix    = 0;

    /* Collect all existing wire net names */
    QSet<QString> existingNames;
    for (const auto &net : scene.wire_manager()->nets()) {
        auto wireNetPtr = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
        if (wireNetPtr && !wireNetPtr->name().isEmpty()) {
            existingNames.insert(wireNetPtr->name());
        }
    }

    /* Find unique name by adding suffix */
    while (existingNames.contains(finalName)) {
        suffix++;
        finalName = QString("%1_%2").arg(baseName).arg(suffix);
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

    /* Build instance map: instance_name -> { module_type, ports } */
    struct PortConnection
    {
        QString portName;
        QString netName;
    };

    struct InstanceInfo
    {
        QString               moduleName;
        QList<PortConnection> ports;
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

            /* Add to instances map */
            if (!instances.contains(instanceName)) {
                instances[instanceName].moduleName = moduleName;
            }

            /* Add port connection */
            PortConnection portConn;
            portConn.portName = portName;
            portConn.netName  = netName;
            instances[instanceName].ports.append(portConn);
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

    qDebug() << "Netlist exported successfully to:" << filePath;
    return true;
}
