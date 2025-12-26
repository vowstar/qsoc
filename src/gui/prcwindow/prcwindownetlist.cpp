// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "./ui_prcwindow.h"
#include "common/qsocprojectmanager.h"
#include "gui/prcwindow/prcconfigdialog.h"
#include "gui/prcwindow/prcprimitiveitem.h"
#include "gui/prcwindow/prcwindow.h"

#include <yaml-cpp/yaml.h>
#include <QDir>
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
#include <qschematic/items/wire.hpp>
#include <qschematic/items/wirenet.hpp>

/* Event Filter */
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

        /* Find wire net containing this item (wire or label) via wire_manager */
        auto wm = scene.wire_manager();
        if (wm) {
            for (const auto &net : wm->nets()) {
                auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
                if (!wireNet) {
                    continue;
                }

                /* Check if clicked item is this net's label */
                if (wireNet->label().get() == item) {
                    handleWireDoubleClick(wireNet.get());
                    return true;
                }

                /* Check if clicked item is one of this net's wires */
                for (const auto &wire : wireNet->wires()) {
                    auto qsWire = std::dynamic_pointer_cast<QSchematic::Items::Wire>(wire);
                    if (qsWire && qsWire.get() == item) {
                        handleWireDoubleClick(wireNet.get());
                        return true;
                    }
                }
            }
        }

        return false;
    }

    return QMainWindow::eventFilter(watched, event);
}

/* Export Netlist Action */
void PrcWindow::on_actionExportNetlist_triggered()
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

    /* Generate default filename from current PRC file */
    QString defaultFileName;
    if (!m_currentFilePath.isEmpty()) {
        QFileInfo fileInfo(m_currentFilePath);
        QString   baseName = fileInfo.completeBaseName();
        defaultFileName    = QDir(defaultPath).filePath(baseName + ".soc_net");
    } else {
        defaultFileName = defaultPath;
    }

    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Export PRC Netlist"),
        defaultFileName,
        tr("SOC Netlist Files (*.soc_net);;All Files (*)"));

    if (filePath.isEmpty()) {
        return;
    }

    if (!filePath.endsWith(".soc_net")) {
        filePath += ".soc_net";
    }

    /* Write netlist to file */
    if (exportNetlist(filePath)) {
        statusBar()->showMessage(tr("Netlist exported successfully: %1").arg(filePath), 3000);
    } else {
        QMessageBox::critical(
            this, tr("Export Failed"), tr("Failed to export netlist to: %1").arg(filePath));
    }
}

/* Dialog Handlers */
void PrcWindow::handlePrcItemDoubleClick(QSchematic::Items::Item *item)
{
    auto *prcItem = qgraphicsitem_cast<PrcLibrary::PrcPrimitiveItem *>(item);
    if (!prcItem) {
        return;
    }

    /* Get connected sources for target primitives */
    QStringList connectedSources;
    if (prcItem->primitiveType() == PrcLibrary::ClockTarget
        || prcItem->primitiveType() == PrcLibrary::ResetTarget) {
        QSet<QString> sources = getConnectedSources(prcItem->primitiveName());
        connectedSources      = sources.values();
        connectedSources.sort();
    }

    /* Show configuration dialog and apply changes if accepted */
    PrcLibrary::PrcConfigDialog dialog(prcItem, &scene, connectedSources, this);
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

    /* Determine source and target names from wire connections */
    QString sourceName = "source";
    QString targetName = "target";

    /* Try to extract names from wire name if available */
    QString wireName = wireNet->name();
    if (!wireName.isEmpty() && wireName.contains("->")) {
        QStringList parts = wireName.split("->");
        if (parts.size() >= 2) {
            sourceName = parts[0].trimmed();
            /* Extract target name (remove operation markers) */
            targetName     = parts[1].trimmed();
            int bracketPos = targetName.indexOf(" [");
            if (bracketPos > 0) {
                targetName = targetName.left(bracketPos);
            }
        }
    }

    /* Create base wire name for lookup (without operation markers) */
    QString baseWireName = QString("%1->%2").arg(sourceName, targetName);

    /* Get existing link parameters or create default */
    PrcLibrary::ClockLinkParams linkParams = getLinkParams(baseWireName);
    linkParams.sourceName                  = sourceName;

    /* Show link configuration dialog */
    PrcLibrary::PrcLinkConfigDialog dialog(sourceName, targetName, linkParams, this);
    if (dialog.exec() == QDialog::Accepted) {
        /* Get configured parameters and store them */
        PrcLibrary::ClockLinkParams newParams = dialog.getLinkParams();

        /* Check if any operation is configured */
        bool hasConfig = newParams.icg.configured || newParams.div.configured
                         || newParams.inv.configured || newParams.sta_guide.configured;

        if (hasConfig) {
            setLinkParams(baseWireName, newParams);

            /* Update wire name to show it has operations */
            QString newName = baseWireName;
            if (newParams.icg.configured) {
                newName += " [ICG]";
            }
            if (newParams.div.configured) {
                newName += QString(" [DIV/%1]").arg(newParams.div.default_value);
            }
            if (newParams.inv.configured) {
                newName += " [INV]";
            }
            wireNet->set_name(newName);
        } else {
            /* Remove link params if nothing configured */
            removeLinkParams(baseWireName);
            wireNet->set_name(baseWireName);
        }

        /* Mark as modified */
        scene.undoStack()->resetClean();
        updateWindowTitle();
    }
}

/* Wire Helpers */
QPointF PrcWindow::getWireStartPos(const QSchematic::Items::WireNet *wireNet) const
{
    Q_UNUSED(wireNet);
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

/* Netlist Export - YAML Helpers */
namespace {

void emitSTAGuide(YAML::Emitter &out, const PrcLibrary::STAGuideParams &p)
{
    if (!p.configured) {
        return;
    }
    out << YAML::Key << "sta_guide" << YAML::Value << YAML::BeginMap;
    if (!p.cell.isEmpty()) {
        out << YAML::Key << "cell" << YAML::Value << p.cell.toStdString();
    }
    if (!p.in.isEmpty()) {
        out << YAML::Key << "in" << YAML::Value << p.in.toStdString();
    }
    if (!p.out.isEmpty()) {
        out << YAML::Key << "out" << YAML::Value << p.out.toStdString();
    }
    if (!p.instance.isEmpty()) {
        out << YAML::Key << "instance" << YAML::Value << p.instance.toStdString();
    }
    out << YAML::EndMap;
}

void emitICG(YAML::Emitter &out, const PrcLibrary::ICGParams &p)
{
    if (!p.configured) {
        return;
    }
    out << YAML::Key << "icg" << YAML::Value << YAML::BeginMap;
    if (!p.enable.isEmpty()) {
        out << YAML::Key << "enable" << YAML::Value << p.enable.toStdString();
    }
    if (!p.polarity.isEmpty()) {
        out << YAML::Key << "polarity" << YAML::Value << p.polarity.toStdString();
    }
    if (!p.reset.isEmpty()) {
        out << YAML::Key << "reset" << YAML::Value << p.reset.toStdString();
    }
    out << YAML::Key << "clock_on_reset" << YAML::Value << p.clock_on_reset;
    emitSTAGuide(out, p.sta_guide);
    out << YAML::EndMap;
}

void emitDIV(YAML::Emitter &out, const PrcLibrary::DIVParams &p)
{
    if (!p.configured) {
        return;
    }
    out << YAML::Key << "div" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "default" << YAML::Value << p.default_value;
    if (!p.value.isEmpty()) {
        out << YAML::Key << "value" << YAML::Value << p.value.toStdString();
    }
    if (p.width > 0) {
        out << YAML::Key << "width" << YAML::Value << p.width;
    }
    if (!p.reset.isEmpty()) {
        out << YAML::Key << "reset" << YAML::Value << p.reset.toStdString();
    }
    out << YAML::Key << "clock_on_reset" << YAML::Value << p.clock_on_reset;
    emitSTAGuide(out, p.sta_guide);
    out << YAML::EndMap;
}

void emitMUX(YAML::Emitter &out, const PrcLibrary::MUXParams &p)
{
    if (!p.configured) {
        return;
    }
    out << YAML::Key << "mux" << YAML::Value << YAML::BeginMap;
    emitSTAGuide(out, p.sta_guide);
    out << YAML::EndMap;
}

void emitINV(YAML::Emitter &out, const PrcLibrary::INVParams &p)
{
    if (!p.configured) {
        return;
    }
    out << YAML::Key << "inv" << YAML::Value << YAML::Null;
}

/**
 * @brief Check if link has any configured operations
 */
bool hasLinkOperations(const PrcLibrary::ClockLinkParams &p)
{
    return p.icg.configured || p.div.configured || p.inv.configured || p.sta_guide.configured;
}

/**
 * @brief Emit link-level operations for a single link
 */
void emitLinkOperations(YAML::Emitter &out, const PrcLibrary::ClockLinkParams &p)
{
    if (p.icg.configured) {
        out << YAML::Key << "icg" << YAML::Value << YAML::BeginMap;
        if (!p.icg.enable.isEmpty()) {
            out << YAML::Key << "enable" << YAML::Value << p.icg.enable.toStdString();
        }
        if (!p.icg.polarity.isEmpty()) {
            out << YAML::Key << "polarity" << YAML::Value << p.icg.polarity.toStdString();
        }
        if (!p.icg.test_enable.isEmpty()) {
            out << YAML::Key << "test_enable" << YAML::Value << p.icg.test_enable.toStdString();
        }
        if (!p.icg.reset.isEmpty()) {
            out << YAML::Key << "reset" << YAML::Value << p.icg.reset.toStdString();
        }
        if (p.icg.clock_on_reset) {
            out << YAML::Key << "clock_on_reset" << YAML::Value << true;
        }
        emitSTAGuide(out, p.icg.sta_guide);
        out << YAML::EndMap;
    }

    if (p.div.configured) {
        out << YAML::Key << "div" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "default" << YAML::Value << p.div.default_value;
        if (!p.div.value.isEmpty()) {
            out << YAML::Key << "value" << YAML::Value << p.div.value.toStdString();
        }
        if (p.div.width > 0) {
            out << YAML::Key << "width" << YAML::Value << p.div.width;
        }
        if (!p.div.reset.isEmpty()) {
            out << YAML::Key << "reset" << YAML::Value << p.div.reset.toStdString();
        }
        if (p.div.clock_on_reset) {
            out << YAML::Key << "clock_on_reset" << YAML::Value << true;
        }
        emitSTAGuide(out, p.div.sta_guide);
        out << YAML::EndMap;
    }

    if (p.inv.configured) {
        if (p.inv.sta_guide.configured) {
            out << YAML::Key << "inv" << YAML::Value << YAML::BeginMap;
            emitSTAGuide(out, p.inv.sta_guide);
            out << YAML::EndMap;
        } else {
            out << YAML::Key << "inv" << YAML::Value << YAML::Null;
        }
    }

    /* Link-level STA guide at end of chain */
    if (p.sta_guide.configured) {
        emitSTAGuide(out, p.sta_guide);
    }
}

} // namespace

/* Netlist Export - Main Function */
bool PrcWindow::exportNetlist(const QString &filePath)
{
    try {
        YAML::Emitter out;
        out << YAML::BeginMap;

        /* Collect primitives by type */
        QList<std::shared_ptr<PrcLibrary::PrcPrimitiveItem>> clockInputs;
        QList<std::shared_ptr<PrcLibrary::PrcPrimitiveItem>> clockTargets;
        QList<std::shared_ptr<PrcLibrary::PrcPrimitiveItem>> resetSources;
        QList<std::shared_ptr<PrcLibrary::PrcPrimitiveItem>> resetTargets;
        QList<std::shared_ptr<PrcLibrary::PrcPrimitiveItem>> powerDomains;

        for (const auto &node : scene.nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
            if (!prcItem) {
                continue;
            }
            switch (prcItem->primitiveType()) {
            case PrcLibrary::ClockInput:
                clockInputs.append(prcItem);
                break;
            case PrcLibrary::ClockTarget:
                clockTargets.append(prcItem);
                break;
            case PrcLibrary::ResetSource:
                resetSources.append(prcItem);
                break;
            case PrcLibrary::ResetTarget:
                resetTargets.append(prcItem);
                break;
            case PrcLibrary::PowerDomain:
                powerDomains.append(prcItem);
                break;
            }
        }

        /* Get link parameters organized by target then source */
        QMap<QString, QMap<QString, PrcLibrary::ClockLinkParams>> linkParamsByTarget
            = getAllLinkParamsByTarget();

        /* Export clock primitives */
        if (!clockInputs.isEmpty() || !clockTargets.isEmpty()) {
            out << YAML::Key << "clock";
            out << YAML::Value << YAML::BeginSeq;
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << "clock_ctrl";

            if (!clockInputs.isEmpty()) {
                out << YAML::Key << "input" << YAML::Value << YAML::BeginMap;
                for (const auto &input : clockInputs) {
                    const auto &inputParams = std::get<PrcLibrary::ClockInputParams>(
                        input->params());
                    out << YAML::Key << input->primitiveName().toStdString();
                    out << YAML::Value << YAML::BeginMap;
                    if (!inputParams.freq.isEmpty()) {
                        out << YAML::Key << "freq" << YAML::Value << inputParams.freq.toStdString();
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }

            if (!clockTargets.isEmpty()) {
                out << YAML::Key << "target" << YAML::Value << YAML::BeginMap;
                for (const auto &target : clockTargets) {
                    const auto &targetParams = std::get<PrcLibrary::ClockTargetParams>(
                        target->params());
                    QString targetName = target->primitiveName();
                    out << YAML::Key << targetName.toStdString();
                    out << YAML::Value << YAML::BeginMap;

                    if (!targetParams.freq.isEmpty()) {
                        out << YAML::Key << "freq" << YAML::Value
                            << targetParams.freq.toStdString();
                    }

                    emitMUX(out, targetParams.mux);
                    emitICG(out, targetParams.icg);
                    emitDIV(out, targetParams.div);
                    emitINV(out, targetParams.inv);

                    /* Link section - only for actually connected sources */
                    QSet<QString> connectedSources = getConnectedSources(targetName);
                    if (!connectedSources.isEmpty()) {
                        out << YAML::Key << "link" << YAML::Value << YAML::BeginMap;

                        /* Get link params for this target */
                        QMap<QString, PrcLibrary::ClockLinkParams> targetLinks
                            = linkParamsByTarget.value(targetName);

                        for (const QString &sourceName : connectedSources) {
                            out << YAML::Key << sourceName.toStdString();

                            /* Check if this link has operations configured */
                            if (targetLinks.contains(sourceName)
                                && hasLinkOperations(targetLinks[sourceName])) {
                                out << YAML::Value << YAML::BeginMap;
                                emitLinkOperations(out, targetLinks[sourceName]);
                                out << YAML::EndMap;
                            } else {
                                out << YAML::Value << YAML::Null;
                            }
                        }
                        out << YAML::EndMap;
                    }

                    if (!targetParams.select.isEmpty()) {
                        out << YAML::Key << "select" << YAML::Value
                            << targetParams.select.toStdString();
                    }
                    if (!targetParams.reset.isEmpty()) {
                        out << YAML::Key << "reset" << YAML::Value
                            << targetParams.reset.toStdString();
                    }
                    if (!targetParams.test_clock.isEmpty()) {
                        out << YAML::Key << "test_clock" << YAML::Value
                            << targetParams.test_clock.toStdString();
                    }

                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap;
            out << YAML::EndSeq;
        }

        /* Export reset primitives */
        if (!resetSources.isEmpty() || !resetTargets.isEmpty()) {
            out << YAML::Key << "reset";
            out << YAML::Value << YAML::BeginSeq;
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << "reset_ctrl";

            if (!resetSources.isEmpty()) {
                out << YAML::Key << "source" << YAML::Value << YAML::BeginMap;
                for (const auto &source : resetSources) {
                    const auto &srcParams = std::get<PrcLibrary::ResetSourceParams>(
                        source->params());
                    out << YAML::Key << source->primitiveName().toStdString();
                    out << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "active" << YAML::Value << srcParams.active.toStdString();
                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }

            if (!resetTargets.isEmpty()) {
                out << YAML::Key << "target" << YAML::Value << YAML::BeginMap;
                for (const auto &target : resetTargets) {
                    const auto &tgtParams = std::get<PrcLibrary::ResetTargetParams>(
                        target->params());
                    QString targetName = target->primitiveName();
                    out << YAML::Key << targetName.toStdString();
                    out << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "active" << YAML::Value << tgtParams.active.toStdString();

                    /* Link section - only for actually connected sources */
                    QSet<QString> connectedSources = getConnectedSources(targetName);
                    if (!connectedSources.isEmpty()) {
                        out << YAML::Key << "link" << YAML::Value << YAML::BeginMap;
                        for (const QString &sourceName : connectedSources) {
                            out << YAML::Key << sourceName.toStdString();
                            if (tgtParams.sync.async_configured) {
                                out << YAML::Value << YAML::BeginMap;
                                out << YAML::Key << "async" << YAML::Value << YAML::BeginMap;
                                out << YAML::Key << "clock" << YAML::Value
                                    << tgtParams.sync.async_clock.toStdString();
                                out << YAML::Key << "stage" << YAML::Value
                                    << tgtParams.sync.async_stage;
                                out << YAML::EndMap;
                                out << YAML::EndMap;
                            } else {
                                out << YAML::Value << YAML::Null;
                            }
                        }
                        out << YAML::EndMap;
                    }

                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap;
            out << YAML::EndSeq;
        }

        /* Export power primitives */
        if (!powerDomains.isEmpty()) {
            out << YAML::Key << "power";
            out << YAML::Value << YAML::BeginSeq;
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << "power_ctrl";

            out << YAML::Key << "domain" << YAML::Value << YAML::BeginSeq;
            for (const auto &domain : powerDomains) {
                const auto &domParams = std::get<PrcLibrary::PowerDomainParams>(domain->params());
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << domain->primitiveName().toStdString();
                out << YAML::Key << "v_mv" << YAML::Value << domParams.v_mv;
                if (!domParams.pgood.isEmpty()) {
                    out << YAML::Key << "pgood" << YAML::Value << domParams.pgood.toStdString();
                }
                out << YAML::Key << "wait_dep" << YAML::Value << domParams.wait_dep;
                out << YAML::Key << "settle_on" << YAML::Value << domParams.settle_on;
                out << YAML::Key << "settle_off" << YAML::Value << domParams.settle_off;

                /* Dependencies */
                if (!domParams.depend.isEmpty()) {
                    out << YAML::Key << "depend" << YAML::Value << YAML::BeginSeq;
                    for (const auto &dep : domParams.depend) {
                        out << YAML::BeginMap;
                        out << YAML::Key << "name" << YAML::Value << dep.name.toStdString();
                        out << YAML::Key << "type" << YAML::Value << dep.type.toStdString();
                        out << YAML::EndMap;
                    }
                    out << YAML::EndSeq;
                }

                /* Follow entries */
                if (!domParams.follow.isEmpty()) {
                    out << YAML::Key << "follow" << YAML::Value << YAML::BeginSeq;
                    for (const auto &fol : domParams.follow) {
                        out << YAML::BeginMap;
                        out << YAML::Key << "clock" << YAML::Value << fol.clock.toStdString();
                        out << YAML::Key << "reset" << YAML::Value << fol.reset.toStdString();
                        out << YAML::Key << "stage" << YAML::Value << fol.stage;
                        out << YAML::EndMap;
                    }
                    out << YAML::EndSeq;
                }

                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
            out << YAML::EndSeq;
        }

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

/* Link Parameter Management */
PrcLibrary::ClockLinkParams PrcWindow::getLinkParams(const QString &wireNetName) const
{
    return m_linkParams.value(wireNetName, PrcLibrary::ClockLinkParams());
}

void PrcWindow::setLinkParams(const QString &wireNetName, const PrcLibrary::ClockLinkParams &params)
{
    m_linkParams[wireNetName] = params;
}

bool PrcWindow::hasLinkParams(const QString &wireNetName) const
{
    return m_linkParams.contains(wireNetName);
}

void PrcWindow::removeLinkParams(const QString &wireNetName)
{
    m_linkParams.remove(wireNetName);
}

QMap<QString, QMap<QString, PrcLibrary::ClockLinkParams>> PrcWindow::getAllLinkParamsByTarget() const
{
    /* This function maps link parameters by target name then source name.
     * Wire names are stored in "source->target" format.
     */
    QMap<QString, QMap<QString, PrcLibrary::ClockLinkParams>> result;

    /* Iterate through all stored link params and organize by target/source */
    for (auto it = m_linkParams.constBegin(); it != m_linkParams.constEnd(); ++it) {
        QString                            wireName = it.key();
        const PrcLibrary::ClockLinkParams &params   = it.value();

        /* Parse wire name to get source and target */
        if (wireName.contains("->")) {
            QStringList parts = wireName.split("->");
            if (parts.size() >= 2) {
                QString sourceName = parts[0].trimmed();
                QString targetName = parts[1].trimmed();

                /* Remove operation markers if present */
                int bracketPos = targetName.indexOf(" [");
                if (bracketPos > 0) {
                    targetName = targetName.left(bracketPos);
                }

                if (!targetName.isEmpty() && !sourceName.isEmpty()) {
                    result[targetName][sourceName] = params;
                }
            }
        }
    }

    return result;
}

/* Wire Connection Analysis */
QList<PrcWindow::WireConnectionInfo> PrcWindow::analyzeWireConnections() const
{
    QList<WireConnectionInfo> connections;

    auto wm = scene.wire_manager();
    if (!wm) {
        return connections;
    }

    /* Build a list of connector info (position, item, text) */
    struct ConnectorInfo
    {
        QPointF                       pos;
        PrcLibrary::PrcPrimitiveItem *item;
        QString                       text;
    };
    QList<ConnectorInfo> connectorList;
    const qreal          tolerance = 10.0;

    for (const auto &node : scene.nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
        if (!prcItem) {
            continue;
        }

        /* Get connectors from this item */
        for (const auto &conn : prcItem->connectors()) {
            ConnectorInfo info;
            info.pos  = conn->scenePos();
            info.item = prcItem.get();
            info.text = conn->text();
            connectorList.append(info);
        }
    }

    /* Helper to find connector at a position */
    auto findConnectorAt =
        [&](const QPointF &pos) -> std::pair<PrcLibrary::PrcPrimitiveItem *, QString> {
        for (const auto &info : connectorList) {
            if (QLineF(info.pos, pos).length() < tolerance) {
                return {info.item, info.text};
            }
        }
        return {nullptr, QString()};
    };

    /* Traverse all wire nets */
    for (const auto &net : wm->nets()) {
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(net);
        if (!wireNet) {
            continue;
        }

        /* Find all wire endpoints and their connected items */
        QSet<QString>                            sourceNames;
        QSet<QString>                            targetNames;
        QMap<QString, PrcLibrary::PrimitiveType> itemTypes;

        for (const auto &wire : wireNet->wires()) {
            auto qsWire = std::dynamic_pointer_cast<QSchematic::Items::Wire>(wire);
            if (!qsWire || qsWire->points_count() < 2) {
                continue;
            }

            /* Check both endpoints of the wire */
            QPointF startPos = qsWire->scenePos() + qsWire->pointsRelative().first();
            QPointF endPos   = qsWire->scenePos() + qsWire->pointsRelative().last();

            for (const QPointF &pos : {startPos, endPos}) {
                auto [item, connText] = findConnectorAt(pos);
                if (!item) {
                    continue;
                }

                QString itemName    = item->primitiveName();
                auto    primType    = item->primitiveType();
                itemTypes[itemName] = primType;

                /* Classify by primitive type and connector name */
                if (primType == PrcLibrary::ClockInput || primType == PrcLibrary::ResetSource) {
                    /* Source types have "out" connectors */
                    if (connText == "out") {
                        sourceNames.insert(itemName);
                    }
                } else if (primType == PrcLibrary::ClockTarget || primType == PrcLibrary::ResetTarget) {
                    /* Target types have "in" connectors */
                    if (connText == "in" || connText.startsWith("in_")) {
                        targetNames.insert(itemName);
                    }
                } else if (primType == PrcLibrary::PowerDomain) {
                    /* Power domains can be both source and target */
                    if (connText == "out") {
                        sourceNames.insert(itemName);
                    } else if (connText == "dep") {
                        targetNames.insert(itemName);
                    }
                }
            }
        }

        /* Create connection records for each source->target pair in this net */
        QString wireNetName = wireNet->name();
        for (const QString &src : sourceNames) {
            for (const QString &tgt : targetNames) {
                /* Validate type compatibility */
                auto srcType = itemTypes.value(src);
                auto tgtType = itemTypes.value(tgt);

                bool compatible = false;
                if (srcType == PrcLibrary::ClockInput && tgtType == PrcLibrary::ClockTarget) {
                    compatible = true;
                } else if (srcType == PrcLibrary::ResetSource && tgtType == PrcLibrary::ResetTarget) {
                    compatible = true;
                } else if (srcType == PrcLibrary::PowerDomain && tgtType == PrcLibrary::PowerDomain) {
                    compatible = true;
                }

                if (compatible) {
                    WireConnectionInfo info;
                    info.sourceName  = src;
                    info.targetName  = tgt;
                    info.wireNetName = wireNetName;
                    connections.append(info);
                }
            }
        }
    }

    return connections;
}

QSet<QString> PrcWindow::getConnectedSources(const QString &targetName) const
{
    QSet<QString> sources;
    auto          connections = analyzeWireConnections();

    for (const auto &conn : connections) {
        if (conn.targetName == targetName) {
            sources.insert(conn.sourceName);
        }
    }

    return sources;
}

/* Dynamic Port Update */
void PrcWindow::updateAllDynamicPorts()
{
    /* Build set of connected connector positions from wire analysis */
    QSet<QPair<QString, QString>> connectedPorts; /* (primitiveName, portName) */

    /* Analyze all wire connections using net points (scene coordinates) */
    for (const auto &net : scene.wire_manager()->nets()) {
        if (!net) {
            continue;
        }

        /* Get all points in this net (scene coordinates) */
        for (const auto &point : net->points()) {
            QPointF scenePoint = point.toPointF();

            /* Find connector at this point */
            for (const auto &node : scene.nodes()) {
                auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
                if (!prcItem) {
                    continue;
                }
                for (const auto &conn : prcItem->connectors()) {
                    QPointF connPos = conn->scenePos();
                    if (qAbs(connPos.x() - scenePoint.x()) < 5
                        && qAbs(connPos.y() - scenePoint.y()) < 5) {
                        connectedPorts.insert({prcItem->primitiveName(), conn->text()});
                    }
                }
            }
        }
    }

    /* First pass: set connection state for all existing connectors */
    for (const auto &node : scene.nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
        if (prcItem) {
            for (const auto &conn : prcItem->connectors()) {
                auto *prcConn = dynamic_cast<PrcLibrary::PrcConnector *>(conn.get());
                if (prcConn) {
                    bool isConnected = connectedPorts.contains(
                        {prcItem->primitiveName(), conn->text()});
                    prcConn->setConnected(isConnected);
                }
            }
        }
    }

    /* Second pass: update dynamic ports (needs connection state set first) */
    for (const auto &node : scene.nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
        if (prcItem) {
            prcItem->updateDynamicPorts();
        }
    }
}
