// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "./ui_prcwindow.h"
#include "gui/prcwindow/prcconfigdialog.h"
#include "gui/prcwindow/prcprimitiveitem.h"
#include "gui/prcwindow/prcwindow.h"

#include <yaml-cpp/yaml.h>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTextStream>

#include <qschematic/items/label.hpp>
#include <qschematic/items/wirenet.hpp>

bool PrcWindow::eventFilter(QObject *watched, QEvent *event)
{
    /* Prevent Delete key from being consumed by ShortcutOverride */
    if (watched == ui->prcView && event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            event->accept();
            return true;
        }
    }

    /* Handle double-click on view items */
    if (watched == ui->prcView->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        auto          *mouseEvent = static_cast<QMouseEvent *>(event);
        QPointF        scenePos   = ui->prcView->mapToScene(mouseEvent->pos());
        QGraphicsItem *item       = scene.itemAt(scenePos, ui->prcView->transform());

        if (!item) {
            return QMainWindow::eventFilter(watched, event);
        }

        /* Open configuration dialog for PrcPrimitiveItem */
        auto *prcItem = qgraphicsitem_cast<PrcLibrary::PrcPrimitiveItem *>(item);
        if (prcItem) {
            handlePrcItemDoubleClick(prcItem);
            return true;
        }

        /* Check parent hierarchy for PrcPrimitiveItem */
        auto *parent = item->parentItem();
        while (parent) {
            auto *parentPrcItem = qgraphicsitem_cast<PrcLibrary::PrcPrimitiveItem *>(parent);
            if (parentPrcItem) {
                handlePrcItemDoubleClick(parentPrcItem);
                return true;
            }
            parent = parent->parentItem();
        }

        /* Open rename dialog for wire */
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

        /* Handle label attached to wire */
        auto *label = qgraphicsitem_cast<QSchematic::Items::Label *>(item);
        if (label) {
            auto *parent = label->parentItem();
            while (parent) {
                auto *wireNet = dynamic_cast<QSchematic::Items::WireNet *>(parent);
                if (wireNet) {
                    handleWireDoubleClick(wireNet);
                    return true;
                }

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

        return false;
    }

    return QMainWindow::eventFilter(watched, event);
}

void PrcWindow::on_actionExportNetlist_triggered()
{
    /* Determine default save path */
    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!m_currentFilePath.isEmpty()) {
        QFileInfo info(m_currentFilePath);
        defaultPath = info.absolutePath() + "/" + info.baseName() + ".soc_net";
    }

    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Export PRC Netlist"),
        defaultPath,
        tr("SOC Netlist Files (*.soc_net);;All Files (*)"));

    if (filePath.isEmpty()) {
        return;
    }

    /* Write netlist to file */
    if (exportNetlist(filePath)) {
        statusBar()->showMessage(tr("Netlist exported successfully: %1").arg(filePath), 3000);
    } else {
        QMessageBox::critical(
            this, tr("Export Failed"), tr("Failed to export netlist to: %1").arg(filePath));
    }
}

void PrcWindow::handlePrcItemDoubleClick(QSchematic::Items::Item *item)
{
    auto *prcItem = qgraphicsitem_cast<PrcLibrary::PrcPrimitiveItem *>(item);
    if (!prcItem) {
        return;
    }

    /* Show configuration dialog and apply changes if accepted */
    PrcLibrary::PrcConfigDialog dialog(prcItem, this);
    if (dialog.exec() == QDialog::Accepted) {
        /* Dialog applies configuration automatically via applyConfiguration() */

        /* Mark as modified */
        scene.undoStack()->resetClean();
        updateWindowTitle();
    }
}

void PrcWindow::handleWireDoubleClick(QSchematic::Items::WireNet *wireNet)
{
    if (!wireNet) {
        return;
    }

    /* Use existing name or generate new one */
    QString currentName = wireNet->name();
    if (currentName.isEmpty()) {
        currentName = autoGenerateWireName(wireNet);
    }

    /* Show rename dialog */
    bool    ok;
    QString newName = QInputDialog::getText(
        this,
        tr("Rename Wire/Net"),
        tr("Enter new name for the wire/net:"),
        QLineEdit::Normal,
        currentName,
        &ok);

    if (ok && !newName.isEmpty() && newName != currentName) {
        wireNet->set_name(newName);
    }
}

QPointF PrcWindow::getWireStartPos(const QSchematic::Items::WireNet *wireNet) const
{
    Q_UNUSED(wireNet);
    /* Not implemented yet */
    return QPointF();
}

void PrcWindow::autoNameWires()
{
    /* Not implemented yet */
}

QSet<QString> PrcWindow::getExistingWireNames() const
{
    QSet<QString> existingNames;

    /* Collect all named wire nets from scene */
    for (const auto &item : scene.items()) {
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(item);
        if (wireNet) {
            QString name = wireNet->name();
            if (!name.isEmpty()) {
                existingNames.insert(name);
            }
        }
    }

    return existingNames;
}

PrcWindow::ConnectionInfo PrcWindow::findStartConnection(
    const QSchematic::Items::WireNet *wireNet) const
{
    Q_UNUSED(wireNet);
    /* Not implemented yet */
    return ConnectionInfo();
}

QString PrcWindow::autoGenerateWireName(const QSchematic::Items::WireNet *wireNet) const
{
    Q_UNUSED(wireNet);

    /* Generate unique unnamed_N name */
    QSet<QString> existingNames = getExistingWireNames();
    int           index         = 0;
    QString       candidateName;
    do {
        candidateName = QString("unnamed_%1").arg(index++);
    } while (existingNames.contains(candidateName));

    return candidateName;
}

bool PrcWindow::exportNetlist(const QString &filePath)
{
    try {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "prc_configuration";
        out << YAML::Value << YAML::BeginMap;

        /* Export clock sources */
        out << YAML::Key << "clock_sources" << YAML::Value << YAML::BeginSeq;
        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (prcItem && prcItem->primitiveType() == PrcLibrary::ClockSource) {
                const auto &params = std::get<PrcLibrary::ClockSourceParams>(prcItem->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << prcItem->primitiveName().toStdString();
                out << YAML::Key << "frequency_mhz" << YAML::Value << params.frequency_mhz;
                out << YAML::Key << "phase_deg" << YAML::Value << params.phase_deg;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;

        /* Export clock targets */
        out << YAML::Key << "clock_targets" << YAML::Value << YAML::BeginSeq;
        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (prcItem && prcItem->primitiveType() == PrcLibrary::ClockTarget) {
                const auto &params = std::get<PrcLibrary::ClockTargetParams>(prcItem->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << prcItem->primitiveName().toStdString();
                out << YAML::Key << "divider" << YAML::Value << params.divider;
                out << YAML::Key << "enable_gate" << YAML::Value << params.enable_gate;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;

        /* Export reset sources */
        out << YAML::Key << "reset_sources" << YAML::Value << YAML::BeginSeq;
        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (prcItem && prcItem->primitiveType() == PrcLibrary::ResetSource) {
                const auto &params = std::get<PrcLibrary::ResetSourceParams>(prcItem->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << prcItem->primitiveName().toStdString();
                out << YAML::Key << "active_low" << YAML::Value << params.active_low;
                out << YAML::Key << "duration_us" << YAML::Value << params.duration_us;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;

        /* Export reset targets */
        out << YAML::Key << "reset_targets" << YAML::Value << YAML::BeginSeq;
        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (prcItem && prcItem->primitiveType() == PrcLibrary::ResetTarget) {
                const auto &params = std::get<PrcLibrary::ResetTargetParams>(prcItem->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << prcItem->primitiveName().toStdString();
                out << YAML::Key << "synchronous" << YAML::Value << params.synchronous;
                out << YAML::Key << "stages" << YAML::Value << params.stages;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;

        /* Export power domains */
        out << YAML::Key << "power_domains" << YAML::Value << YAML::BeginSeq;
        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (prcItem && prcItem->primitiveType() == PrcLibrary::PowerDomain) {
                const auto &params = std::get<PrcLibrary::PowerDomainParams>(prcItem->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << prcItem->primitiveName().toStdString();
                out << YAML::Key << "voltage" << YAML::Value << params.voltage;
                out << YAML::Key << "isolation" << YAML::Value << params.isolation;
                out << YAML::Key << "retention" << YAML::Value << params.retention;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        out << YAML::EndMap;

        /* Write to file */
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return false;
        }

        QTextStream stream(&file);
        stream << out.c_str();
        file.close();

        return true;
    } catch (const std::exception &e) {
        qWarning() << "Failed to export netlist:" << e.what();
        return false;
    }
}
