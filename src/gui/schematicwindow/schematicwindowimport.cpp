// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocconsole.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocyamlutils.h"
#include "gui/schematicwindow/schematicconnector.h"
#include "gui/schematicwindow/schematicmodule.h"
#include "gui/schematicwindow/schematicwindow.h"
#include "gui/schematicwindow/schematicwire.h"

#include "./ui_schematicwindow.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QtMath>

#include <qschematic/commands/item_add.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/wire.hpp>
#include <qschematic/items/wirenet.hpp>
#include <qschematic/scene.hpp>
#include <qschematic/wire_system/manager.hpp>

#include <yaml-cpp/yaml.h>

namespace {

/* Direction heuristic for synthesized port specs when the referenced module
 * is not present in the library. We only see port names from the netlist's
 * `instance.<name>.port.<p>: { link: net }` entries, not directions. The
 * heuristic is intentionally dumb: we just need a visible side so the user
 * can see topology. */
QString guessDirection(const QString &portName)
{
    const QString lower = portName.toLower();
    if (lower.endsWith(QStringLiteral("_o")) || lower.endsWith(QStringLiteral("_oe"))
        || lower.endsWith(QStringLiteral("_out")) || lower.startsWith(QStringLiteral("o_"))
        || lower.contains(QStringLiteral("out"))) {
        return QStringLiteral("output");
    }
    return QStringLiteral("input");
}

/* Build a minimal module YAML node from the port/bus names referenced by an
 * instance. Used only when the module is not in the loaded library. */
YAML::Node synthesizeModuleYaml(const YAML::Node &instanceNode)
{
    YAML::Node mod;
    if (instanceNode["port"] && instanceNode["port"].IsMap()) {
        YAML::Node portSection;
        for (const auto &portEntry : instanceNode["port"]) {
            const QString portName = QString::fromStdString(portEntry.first.as<std::string>());
            YAML::Node    portDef;
            portDef["direction"]                = guessDirection(portName).toStdString();
            portDef["type"]                     = "logic";
            portSection[portName.toStdString()] = portDef;
        }
        mod["port"] = portSection;
    }
    if (instanceNode["bus"] && instanceNode["bus"].IsMap()) {
        YAML::Node busSection;
        for (const auto &busEntry : instanceNode["bus"]) {
            const std::string busName = busEntry.first.as<std::string>();
            YAML::Node        busDef;
            busDef["bus"]       = busName;
            busDef["mode"]      = "slave";
            busSection[busName] = busDef;
        }
        mod["bus"] = busSection;
    }
    return mod;
}

/* Extract the net name referenced by `link:`; the value may be either a
 * plain scalar or a map containing a `link:` key. Empty string means the
 * port has no explicit link. */
QString extractLinkedNet(const YAML::Node &portOrBusEntry)
{
    if (!portOrBusEntry) {
        return {};
    }
    if (portOrBusEntry.IsScalar()) {
        return QString::fromStdString(portOrBusEntry.as<std::string>());
    }
    if (portOrBusEntry.IsMap() && portOrBusEntry["link"]) {
        return QString::fromStdString(portOrBusEntry["link"].as<std::string>());
    }
    return {};
}

} /* namespace */

void SchematicWindow::on_actionImportNetlist_triggered()
{
    QString defaultPath;
    if (projectManager) {
        defaultPath = projectManager->getOutputPath();
    }
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import Netlist"), defaultPath, tr("SOC Netlist Files (*.soc_net);;All Files (*)"));

    if (files.isEmpty()) {
        return;
    }

    /* If the scene already has content, ask before replacing. The import is
     * destructive in the "replace" path; we never auto-merge into existing
     * work. */
    bool haveContent = false;
    for (const auto &item : scene.items()) {
        if (item) {
            haveContent = true;
            break;
        }
    }
    if (haveContent) {
        const QMessageBox::StandardButton resp = QMessageBox::question(
            this,
            tr("Import Netlist"),
            tr("The current scene already has content. Replace it with the imported netlist?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (resp != QMessageBox::Yes) {
            return;
        }
        scene.clear();
        scene.undoStack()->clear();
    }

    if (!importNetlistFiles(files)) {
        QMessageBox::warning(
            this, tr("Import Netlist"), tr("No instances were imported from the selected files."));
        return;
    }
    m_lastImportedFiles = files;
}

void SchematicWindow::on_actionAutoArrange_triggered()
{
    if (m_lastImportedFiles.isEmpty()) {
        QMessageBox::information(
            this,
            tr("Auto Arrange"),
            tr("No netlist has been imported in this session yet. Use "
               "File → Import Netlist first."));
        return;
    }

    /* Verify the files still exist before wiping the scene. */
    for (const QString &path : m_lastImportedFiles) {
        if (!QFileInfo::exists(path)) {
            QMessageBox::warning(
                this,
                tr("Auto Arrange"),
                tr("The previously imported file is no longer accessible:\n%1").arg(path));
            return;
        }
    }

    scene.clear();
    scene.undoStack()->clear();
    if (!importNetlistFiles(m_lastImportedFiles)) {
        QMessageBox::warning(
            this, tr("Auto Arrange"), tr("No instances were imported during re-layout."));
    }
}

bool SchematicWindow::importNetlistFiles(const QStringList &filePaths)
{
    if (filePaths.isEmpty()) {
        return false;
    }

    /* autoNameWires() runs on every netlistChanged signal, which fires
     * per scene.addItem / scene.addWire. During bulk import it would
     * rescan all nodes/connectors/nets for every wire added, turning
     * the loop into O(W^3). Disconnect it for the duration and call
     * it once at the end. */
    disconnect(&scene, &QSchematic::Scene::netlistChanged, this, &SchematicWindow::autoNameWires);
    auto restoreAutoName = qScopeGuard([this]() {
        connect(&scene, &QSchematic::Scene::netlistChanged, this, &SchematicWindow::autoNameWires);
    });

    /* Merge the selected soc_net YAML files. The merge prefers later
     * files over earlier ones, using the same utility that
     * `qsoc generate --merge` relies on so behavior stays consistent. */
    const YAML::Node merged = QSocYamlUtils::loadAndMergeFiles(filePaths);
    if (!merged || !merged.IsMap()) {
        QMessageBox::critical(
            this, tr("Import Netlist"), tr("Failed to parse or merge the selected files."));
        return false;
    }

    if (!merged["instance"] || !merged["instance"].IsMap()) {
        QMessageBox::warning(
            this,
            tr("Import Netlist"),
            tr("The netlist has no `instance:` section; nothing to place."));
        return false;
    }

    /* Ensure the module library is loaded so we can resolve module
     * definitions. If it is not loaded yet, load everything. */
    if (moduleManager && projectManager && projectManager->isValid()) {
        moduleManager->load(QRegularExpression(QStringLiteral(".*")));
    }

    /* Build SchematicModule items in a two-pass loop. The first pass
     * creates the modules so we can read their actual widths/heights
     * (they size themselves to fit the longest port label). The second
     * pass positions them by layer, with column width computed from the
     * widest module plus widest net-label width plus stub length, so
     * labels from adjacent columns do not overlap. */
    const YAML::Node &instances     = merged["instance"];
    const int         instanceCount = static_cast<int>(instances.size());
    if (instanceCount == 0) {
        return false;
    }

    /* Net name -> list of (instance, port, isBus). */
    struct PortRef
    {
        QString instanceName;
        QString portName;
        bool    isBus;
    };
    QHash<QString, QList<PortRef>> netToEndpoints;

    /* Collected modules from pass 1. outgoingNets/incomingNets are the
     * nets this instance drives / reads, used by the layering pass. */
    struct Prepared
    {
        QString                          instanceName;
        std::shared_ptr<SchematicModule> module;
        QStringList                      outgoingNets;
        QStringList                      incomingNets;
    };
    QList<Prepared> prepared;
    prepared.reserve(instanceCount);

    for (auto it = instances.begin(); it != instances.end(); ++it) {
        const QString     instanceName = QString::fromStdString(it->first.as<std::string>());
        const YAML::Node &instNode     = it->second;
        if (!instNode.IsMap()) {
            continue;
        }

        QString moduleName;
        if (instNode["module"]) {
            moduleName = QString::fromStdString(instNode["module"].as<std::string>());
        }
        if (moduleName.isEmpty()) {
            moduleName = instanceName;
        }

        /* Module YAML: try library, fall back to a synthesized one. */
        YAML::Node modYaml;
        if (moduleManager && moduleManager->isModuleExist(moduleName)) {
            modYaml = moduleManager->getModuleYaml(moduleName);
        }
        if (!modYaml || !modYaml.IsMap()) {
            modYaml = synthesizeModuleYaml(instNode);
        }

        auto module = std::make_shared<SchematicModule>(moduleName, modYaml);
        module->setInstanceName(instanceName);

        Prepared entry;
        entry.instanceName = instanceName;
        entry.module       = module;

        /* Collect port->net mapping and direction for this instance. Port
         * direction comes from the *module* YAML (the netlist entry only
         * carries a `link:` value). Bus ports are always treated as
         * bidirectional so they show up as both producer and consumer for
         * layering; this keeps masters/slaves balanced around the bus. */
        if (instNode["port"] && instNode["port"].IsMap()) {
            for (const auto &portEntry : instNode["port"]) {
                const QString portName = QString::fromStdString(portEntry.first.as<std::string>());
                const QString netName  = extractLinkedNet(portEntry.second);
                if (netName.isEmpty()) {
                    continue;
                }
                netToEndpoints[netName].append(PortRef{instanceName, portName, false});

                /* Look up the direction for this port in the module YAML. */
                std::string dir;
                if (modYaml["port"] && modYaml["port"][portName.toStdString()]
                    && modYaml["port"][portName.toStdString()]["direction"]) {
                    dir = modYaml["port"][portName.toStdString()]["direction"].as<std::string>();
                }
                const bool isOut = (dir == "out" || dir == "output" || dir == "inout");
                const bool isIn  = (dir == "in" || dir == "input" || dir == "inout" || dir.empty());
                if (isOut) {
                    entry.outgoingNets.append(netName);
                }
                if (isIn) {
                    entry.incomingNets.append(netName);
                }
            }
        }
        if (instNode["bus"] && instNode["bus"].IsMap()) {
            for (const auto &busEntry : instNode["bus"]) {
                const QString busName = QString::fromStdString(busEntry.first.as<std::string>());
                const QString netName = extractLinkedNet(busEntry.second);
                if (netName.isEmpty()) {
                    continue;
                }
                netToEndpoints[netName].append(PortRef{instanceName, busName, true});
                entry.outgoingNets.append(netName);
                entry.incomingNets.append(netName);
            }
        }

        prepared.append(entry);
    }

    if (prepared.isEmpty()) {
        return false;
    }

    /* Large-net (broadcast) classification, following NLView's `largenet`
     * property. Nets whose fanout exceeds the threshold do not constrain
     * the DAG: they collapse the layering (clock/reset pull every module
     * into the same layer) and they render as per-endpoint stubs later
     * anyway. Threshold scales with design size so small projects still
     * see a sensible grid. */
    const int largeNetThreshold
        = std::max(8, static_cast<int>(std::round(std::sqrt(instanceCount))));
    QSet<QString> largeNets;
    for (auto netIt = netToEndpoints.constBegin(); netIt != netToEndpoints.constEnd(); ++netIt) {
        if (netIt.value().size() >= largeNetThreshold) {
            largeNets.insert(netIt.key());
        }
    }

    /* Layer assignment. Build an instance-level DAG where an edge
     * (driver -> consumer) exists whenever `driver` has an output linked
     * to some net that `consumer` reads as input. Longest-path layer
     * assignment gives a natural left-to-right data-flow view. Feedback
     * edges in real SoCs are inevitable (CPU<->MEM buses), so we resolve
     * cycles by assigning the earliest-possible layer to any instance
     * that cannot be settled in a forward pass. */
    QHash<QString, QString> netDriver;
    for (const Prepared &pre : prepared) {
        for (const QString &netName : pre.outgoingNets) {
            if (largeNets.contains(netName)) {
                continue;
            }
            if (!netDriver.contains(netName)) {
                netDriver.insert(netName, pre.instanceName);
            }
        }
    }

    QHash<QString, QSet<QString>> upstream;
    for (const Prepared &pre : prepared) {
        QSet<QString> &set = upstream[pre.instanceName];
        for (const QString &netName : pre.incomingNets) {
            if (largeNets.contains(netName)) {
                continue;
            }
            const auto it = netDriver.constFind(netName);
            if (it != netDriver.constEnd() && it.value() != pre.instanceName) {
                set.insert(it.value());
            }
        }
    }

    QHash<QString, int> layerOf;
    {
        bool progress = true;
        while (progress) {
            progress = false;
            for (const Prepared &pre : prepared) {
                if (layerOf.contains(pre.instanceName)) {
                    continue;
                }
                int  maxUp     = -1;
                bool allSolved = true;
                for (const QString &up : upstream.value(pre.instanceName)) {
                    const auto it = layerOf.constFind(up);
                    if (it == layerOf.constEnd()) {
                        allSolved = false;
                        break;
                    }
                    if (it.value() > maxUp) {
                        maxUp = it.value();
                    }
                }
                if (allSolved) {
                    layerOf.insert(pre.instanceName, maxUp + 1);
                    progress = true;
                }
            }
        }
        /* Any instance left unassigned sits in a cycle. Put it one step
         * after the highest layer among its already-settled drivers (or
         * zero if none are). This is stable regardless of iteration order. */
        for (const Prepared &pre : prepared) {
            if (layerOf.contains(pre.instanceName)) {
                continue;
            }
            int maxUp = -1;
            for (const QString &up : upstream.value(pre.instanceName)) {
                const auto it = layerOf.constFind(up);
                if (it != layerOf.constEnd() && it.value() > maxUp) {
                    maxUp = it.value();
                }
            }
            layerOf.insert(pre.instanceName, maxUp + 1);
        }
    }

    /* Group instances by layer. Initial order within a layer is by
     * name so that equivalent runs produce stable output when the
     * barycenter pass below has nothing to improve. */
    QMap<int, QStringList> byLayer;
    for (const Prepared &pre : prepared) {
        byLayer[layerOf.value(pre.instanceName)].append(pre.instanceName);
    }
    for (auto sortIter = byLayer.begin(); sortIter != byLayer.end(); ++sortIter) {
        std::sort(sortIter.value().begin(), sortIter.value().end());
    }

    /* Barycenter-based crossing reduction. For each pair of adjacent
     * layers, reorder nodes in the target layer by the average index of
     * their connected neighbours in the fixed layer. Three down-sweep +
     * three up-sweep passes converge for most real-world SoC graphs;
     * additional passes have diminishing return versus their cost. */
    QHash<QString, QSet<QString>> downstream;
    for (auto upIt = upstream.constBegin(); upIt != upstream.constEnd(); ++upIt) {
        for (const QString &driver : upIt.value()) {
            downstream[driver].insert(upIt.key());
        }
    }

    auto positionIn = [](const QStringList &layerList, const QString &name) -> int {
        const int index = layerList.indexOf(name);
        return index < 0 ? 0 : index;
    };

    auto sweepLayer = [&](int layerKey, bool useUpstream) {
        QStringList &list       = byLayer[layerKey];
        const int    siblingKey = useUpstream ? (layerKey - 1) : (layerKey + 1);
        if (!byLayer.contains(siblingKey)) {
            return;
        }
        const QStringList           &sibling = byLayer.value(siblingKey);
        QList<QPair<qreal, QString>> scored;
        scored.reserve(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const QString       &name       = list.at(i);
            const QSet<QString> &neighbours = useUpstream ? upstream.value(name)
                                                          : downstream.value(name);
            qreal                sum        = 0.0;
            int                  count      = 0;
            for (const QString &neighbour : neighbours) {
                if (sibling.contains(neighbour)) {
                    sum += positionIn(sibling, neighbour);
                    count++;
                }
            }
            const qreal bary = (count > 0) ? (sum / count) : static_cast<qreal>(i);
            scored.append({bary, name});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.first < rhs.first;
        });
        QStringList reordered;
        reordered.reserve(scored.size());
        for (const auto &pair : scored) {
            reordered.append(pair.second);
        }
        list = reordered;
    };

    const QList<int> sweepKeys = byLayer.keys();
    for (int pass = 0; pass < 3; ++pass) {
        /* Down-sweep: layer L depends on L-1. */
        for (int i = 1; i < sweepKeys.size(); ++i) {
            sweepLayer(sweepKeys.at(i), /*useUpstream=*/true);
        }
        /* Up-sweep: layer L depends on L+1. */
        for (int i = sweepKeys.size() - 2; i >= 0; --i) {
            sweepLayer(sweepKeys.at(i), /*useUpstream=*/false);
        }
    }

    /* Per-layer dynamic sizing. Column width for layer L is the widest
     * module in that layer; the horizontal gap between L and L+1 is
     * `stub + max(label_width in gap) + stub + padding`, where
     * `label_width in gap` considers nets outgoing from L and incoming
     * to L+1 (both side labels appear in the gap). Vertical stride
     * within a layer uses the tallest module in that layer plus
     * vertical padding. */
    const qreal        stubLength   = 40.0;
    const qreal        horizPadding = 40.0;
    const qreal        vertPadding  = 60.0;
    const QFontMetrics labelMetrics{QFont{}};

    QHash<QString, Prepared const *> byInstance;
    for (const Prepared &pre : prepared) {
        byInstance.insert(pre.instanceName, &pre);
    }

    const QList<int> layerKeys = byLayer.keys();

    /* Max label width on one side of a layer. Outgoing nets produce labels
     * on the right side of that layer's modules; incoming nets produce
     * labels on the left side. The gap between two adjacent layers has to
     * fit BOTH the right-side labels of the left layer AND the left-side
     * labels of the right layer at the same Y, so we take the sum of the
     * two maxima, not the max. */
    auto maxLabelPxForNets = [&](const QStringList &nets) -> qreal {
        qreal widest = 0.0;
        for (const QString &netName : nets) {
            widest = std::max(widest, static_cast<qreal>(labelMetrics.horizontalAdvance(netName)));
        }
        return widest;
    };
    auto outLabelPxForLayer = [&](int layerKey) -> qreal {
        QStringList nets;
        for (const QString &inst : byLayer.value(layerKey)) {
            const Prepared *pre = byInstance.value(inst, nullptr);
            if (pre) {
                nets.append(pre->outgoingNets);
            }
        }
        return maxLabelPxForNets(nets);
    };
    auto inLabelPxForLayer = [&](int layerKey) -> qreal {
        QStringList nets;
        for (const QString &inst : byLayer.value(layerKey)) {
            const Prepared *pre = byInstance.value(inst, nullptr);
            if (pre) {
                nets.append(pre->incomingNets);
            }
        }
        return maxLabelPxForNets(nets);
    };

    QHash<int, qreal> layerWidth;
    QHash<int, qreal> layerMaxModuleH;
    for (int layerKey : layerKeys) {
        qreal widest  = 0.0;
        qreal tallest = 0.0;
        for (const QString &inst : byLayer.value(layerKey)) {
            const Prepared *pre = byInstance.value(inst, nullptr);
            if (!pre) {
                continue;
            }
            widest  = std::max(widest, pre->module->size().width());
            tallest = std::max(tallest, pre->module->size().height());
        }
        layerWidth.insert(layerKey, widest);
        layerMaxModuleH.insert(layerKey, tallest);
    }

    QHash<int, qreal> layerX;
    qreal             xCursor = 0.0;
    for (int i = 0; i < layerKeys.size(); ++i) {
        const int layerKey = layerKeys.at(i);
        layerX.insert(layerKey, xCursor);
        const qreal widthHere = layerWidth.value(layerKey);
        if (i + 1 < layerKeys.size()) {
            const int   nextKey      = layerKeys.at(i + 1);
            const qreal thisOutLabel = outLabelPxForLayer(layerKey);
            const qreal nextInLabel  = inLabelPxForLayer(nextKey);
            xCursor += widthHere + (2.0 * stubLength) + thisOutLabel + nextInLabel + horizPadding;
        } else {
            xCursor += widthHere;
        }
    }

    /* Position modules. */
    QHash<QString, SchematicModule *> placedByInstance;
    for (int layerKey : layerKeys) {
        const qreal xHere   = layerX.value(layerKey);
        qreal       yCursor = 0.0;
        for (const QString &inst : byLayer.value(layerKey)) {
            const Prepared *pre = byInstance.value(inst, nullptr);
            if (!pre) {
                continue;
            }
            pre->module->setPos(QPointF(xHere, yCursor));
            scene.addItem(pre->module);
            placedByInstance.insert(pre->instanceName, pre->module.get());
            yCursor += pre->module->size().height() + vertPadding;
        }
    }

    /* Port-level barycenter reorder. Within each module and each side,
     * reorder connector Y positions by the average scene-Y of the remote
     * endpoints of the net each connector is on. This cuts down on the
     * "port row connects to a wire that sweeps past the module" tangles
     * that remain after module-level ordering. Runs once using the
     * positions established above; no iteration with module ordering. */
    QHash<QPair<QString, QString>, QString> portToNet;
    for (auto netIt = netToEndpoints.constBegin(); netIt != netToEndpoints.constEnd(); ++netIt) {
        for (const PortRef &endpoint : netIt.value()) {
            portToNet.insert({endpoint.instanceName, endpoint.portName}, netIt.key());
        }
    }

    /* Module side-flip decision. For each placed module, score how many
     * of its ports have a connection direction that disagrees with the
     * module side they currently sit on (Left-side port connecting to a
     * remote on the right goes through the module body, which is bad).
     * If flipping the whole module swaps more mismatches into matches
     * than it creates, swap every connector's Left/Right assignment and
     * reassign gridPosX to the corresponding module edge. */
    {
        constexpr int gridSize = 20;
        for (const Prepared &pre : prepared) {
            SchematicModule *module = pre.module.get();
            if (!module) {
                continue;
            }
            const qreal myCenterX   = module->pos().x() + (module->size().width() / 2.0);
            int         currentCost = 0;
            int         flippedCost = 0;

            for (const auto &baseConn : module->connectors()) {
                auto sc = std::dynamic_pointer_cast<SchematicConnector>(baseConn);
                if (!sc) {
                    continue;
                }
                if (sc->modulePosition() != SchematicConnector::Left
                    && sc->modulePosition() != SchematicConnector::Right) {
                    continue;
                }
                QString portName = sc->text();
                if (portName.isEmpty() && sc->label()) {
                    portName = sc->label()->text();
                }
                const QString netName = portToNet.value({pre.instanceName, portName}, QString());
                if (netName.isEmpty()) {
                    continue;
                }
                int remotesLeft  = 0;
                int remotesRight = 0;
                for (const PortRef &other : netToEndpoints.value(netName)) {
                    if (other.instanceName == pre.instanceName && other.portName == portName) {
                        continue;
                    }
                    const SchematicModule *otherModule
                        = placedByInstance.value(other.instanceName, nullptr);
                    if (!otherModule) {
                        continue;
                    }
                    const qreal otherCenterX = otherModule->pos().x()
                                               + (otherModule->size().width() / 2.0);
                    if (otherCenterX < myCenterX) {
                        remotesLeft++;
                    } else if (otherCenterX > myCenterX) {
                        remotesRight++;
                    }
                }
                if (sc->modulePosition() == SchematicConnector::Left) {
                    currentCost += remotesRight;
                    flippedCost += remotesLeft;
                } else {
                    currentCost += remotesLeft;
                    flippedCost += remotesRight;
                }
            }

            if (flippedCost >= currentCost) {
                continue;
            }

            const int rightEdgeGrid = static_cast<int>(
                std::round(module->size().width() / gridSize));
            for (const auto &baseConn : module->connectors()) {
                auto sc = std::dynamic_pointer_cast<SchematicConnector>(baseConn);
                if (!sc) {
                    continue;
                }
                if (sc->modulePosition() == SchematicConnector::Left) {
                    sc->setModulePosition(SchematicConnector::Right);
                    sc->setGridPosX(rightEdgeGrid);
                } else if (sc->modulePosition() == SchematicConnector::Right) {
                    sc->setModulePosition(SchematicConnector::Left);
                    sc->setGridPosX(0);
                }
            }
        }
    }

    auto connectorSceneY = [](const SchematicConnector *sc) -> qreal {
        return sc ? sc->scenePos().y() : 0.0;
    };

    for (const Prepared &pre : prepared) {
        SchematicModule *module = pre.module.get();

        QList<std::shared_ptr<SchematicConnector>> leftPorts;
        QList<std::shared_ptr<SchematicConnector>> rightPorts;
        for (const auto &baseConn : module->connectors()) {
            auto sc = std::dynamic_pointer_cast<SchematicConnector>(baseConn);
            if (!sc) {
                continue;
            }
            if (sc->modulePosition() == SchematicConnector::Left) {
                leftPorts.append(sc);
            } else if (sc->modulePosition() == SchematicConnector::Right) {
                rightPorts.append(sc);
            }
        }

        auto reorder = [&](QList<std::shared_ptr<SchematicConnector>> &ports) {
            if (ports.size() < 2) {
                return;
            }
            QList<QPair<qreal, std::shared_ptr<SchematicConnector>>> scored;
            scored.reserve(ports.size());
            for (const auto &port : ports) {
                QString portName = port->text();
                if (portName.isEmpty() && port->label()) {
                    portName = port->label()->text();
                }
                const QString netName = portToNet.value({pre.instanceName, portName}, QString());
                qreal         sum     = 0.0;
                int           count   = 0;
                if (!netName.isEmpty()) {
                    for (const PortRef &other : netToEndpoints.value(netName)) {
                        if (other.instanceName == pre.instanceName && other.portName == portName) {
                            continue;
                        }
                        const SchematicModule *otherModule
                            = placedByInstance.value(other.instanceName, nullptr);
                        if (!otherModule) {
                            continue;
                        }
                        for (const auto &otherBase : otherModule->connectors()) {
                            auto otherSc = std::dynamic_pointer_cast<SchematicConnector>(otherBase);
                            if (!otherSc) {
                                continue;
                            }
                            QString otherName = otherSc->text();
                            if (otherName.isEmpty() && otherSc->label()) {
                                otherName = otherSc->label()->text();
                            }
                            if (otherName == other.portName) {
                                sum += connectorSceneY(otherSc.get());
                                count++;
                                break;
                            }
                        }
                    }
                }
                const qreal bary = (count > 0) ? (sum / count) : port->scenePos().y();
                scored.append({bary, port});
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.first < rhs.first;
            });
            /* Greedy Y packing: each port starts at its barycenter Y,
             * then gets pushed down as needed to keep a minimum 1 grid
             * unit gap to the previous port. This matches the
             * 20 px / 40 px alternating spacing the original module
             * layout uses, so a module sized for N ports still fits
             * after packing. Ports land on their partner's grid row
             * whenever the barycenter allows, collapsing residual
             * L-bends into straight wires. */
            constexpr int gridSize          = 20;
            constexpr int minDeltaGrid      = 1;
            constexpr int topMarginGrid     = 3; /* 50 px ~ LABEL_HEIGHT + margin */
            const int     moduleTopGridY    = topMarginGrid;
            const qreal   moduleTopY        = pre.module->pos().y();
            const qreal   moduleH           = pre.module->size().height();
            const int     moduleBottomGridY = static_cast<int>((moduleH - 20.0) / gridSize);

            int prevGridY = moduleTopGridY - minDeltaGrid;
            for (const auto &pair : scored) {
                const qreal baryLocal = pair.first - moduleTopY;
                int         gridY     = static_cast<int>(std::round(baryLocal / gridSize));
                gridY                 = std::max(gridY, prevGridY + minDeltaGrid);
                gridY                 = std::max(gridY, moduleTopGridY);
                gridY                 = std::min(gridY, moduleBottomGridY);
                pair.second->setGridPosY(gridY);
                prevGridY = gridY;
            }
        };

        reorder(leftPorts);
        reorder(rightPorts);
    }

    /* Helper: find a module's connector by the port name used in the
     * netlist. Returns nullptr if the port is not exposed on the module
     * (for example, hidden by bus-mapping visibility rules). */
    auto findConnector = [&](SchematicModule *module,
                             const QString   &portName) -> std::shared_ptr<SchematicConnector> {
        for (const auto &baseConn : module->connectors()) {
            auto sc = std::dynamic_pointer_cast<SchematicConnector>(baseConn);
            if (!sc) {
                continue;
            }
            QString label = sc->text();
            if (label.isEmpty() && sc->label()) {
                label = sc->label()->text();
            }
            if (label == portName) {
                return sc;
            }
        }
        return nullptr;
    };

    /* Classify nets into "real wire" vs "stub+label". A net qualifies
     * as a real wire when it has exactly two non-bus endpoints, one on
     * each side (Right + Left), and the two modules sit in adjacent
     * layers. Forward edges (Right in lower layer, Left in higher
     * layer) route as a simple L through the inter-layer gap. Backward
     * edges (the opposite) route as a U-shape above the top of both
     * modules to stay clear of module bodies. Broadcast nets, buses,
     * and non-adjacent layers still fall back to stub-per-endpoint
     * net-by-name. */
    struct WirePlan
    {
        QPointF rightPort;  /* Endpoint on the Right side of its module. */
        QPointF leftPort;   /* Endpoint on the Left side of its module. */
        bool    isBackward; /* true when right-side module is in a higher layer. */
        qreal   detourY;    /* For backward: Y above both modules' tops. */
    };
    const qreal              detourMargin = 30.0;
    QHash<QString, WirePlan> realWireNets;
    for (auto netIt = netToEndpoints.constBegin(); netIt != netToEndpoints.constEnd(); ++netIt) {
        const QList<PortRef> &endpoints = netIt.value();
        if (endpoints.size() != 2) {
            continue;
        }
        if (endpoints.at(0).isBus || endpoints.at(1).isBus) {
            continue;
        }
        SchematicModule *moduleA = placedByInstance.value(endpoints.at(0).instanceName, nullptr);
        SchematicModule *moduleB = placedByInstance.value(endpoints.at(1).instanceName, nullptr);
        if (!moduleA || !moduleB) {
            continue;
        }
        const auto connA = findConnector(moduleA, endpoints.at(0).portName);
        const auto connB = findConnector(moduleB, endpoints.at(1).portName);
        if (!connA || !connB) {
            continue;
        }

        /* Identify which endpoint is on the Right side and which is on
         * the Left side. Reject if both are on the same side. */
        std::shared_ptr<SchematicConnector> rightConn;
        std::shared_ptr<SchematicConnector> leftConn;
        SchematicModule                    *rightModule = nullptr;
        SchematicModule                    *leftModule  = nullptr;
        QString                             rightInst;
        QString                             leftInst;
        if (connA->modulePosition() == SchematicConnector::Right
            && connB->modulePosition() == SchematicConnector::Left) {
            rightConn   = connA;
            leftConn    = connB;
            rightModule = moduleA;
            leftModule  = moduleB;
            rightInst   = endpoints.at(0).instanceName;
            leftInst    = endpoints.at(1).instanceName;
        } else if (
            connB->modulePosition() == SchematicConnector::Right
            && connA->modulePosition() == SchematicConnector::Left) {
            rightConn   = connB;
            leftConn    = connA;
            rightModule = moduleB;
            leftModule  = moduleA;
            rightInst   = endpoints.at(1).instanceName;
            leftInst    = endpoints.at(0).instanceName;
        } else {
            continue;
        }

        const int rightLayer = layerOf.value(rightInst, -1);
        const int leftLayer  = layerOf.value(leftInst, -1);
        if (std::abs(rightLayer - leftLayer) != 1) {
            continue;
        }

        WirePlan plan;
        plan.rightPort  = rightConn->scenePos();
        plan.leftPort   = leftConn->scenePos();
        plan.isBackward = (rightLayer > leftLayer);

        if (plan.isBackward) {
            /* For U-shape routing over the top we need the minimum Y of
             * both module bounding boxes, then subtract a margin so the
             * horizontal segment sits clearly above the bodies. */
            const qreal topY = std::min(rightModule->pos().y(), leftModule->pos().y())
                               - detourMargin;
            plan.detourY = topY;
        }

        realWireNets.insert(netIt.key(), plan);
    }

    /* Track the wires created for each real-wire net so we can evaluate
     * geometric crossings afterwards and fall back to stubs when a net
     * turns out to tangle with too many others. */
    QHash<QString, QList<std::shared_ptr<QSchematic::Items::Wire>>> realWireByNet;

    /* Position a WireNet's label centred on a given scene point, with
     * horizontal rotation. Offsets the label by half its height/width so
     * labelCenter is the visual centre. */
    auto placeWireLabel = [](const std::shared_ptr<QSchematic::Items::WireNet> &wireNet,
                             const QString                                     &netName,
                             const QPointF                                     &labelCenter) {
        if (!wireNet) {
            return;
        }
        auto label = wireNet->label();
        if (!label) {
            return;
        }
        label->setText(netName);
        const qreal labelW = label->boundingRect().width();
        const qreal labelH = label->boundingRect().height();
        label->setRotation(0.0);
        label->setPos(labelCenter.x() - (labelW / 2.0), labelCenter.y() - (labelH / 2.0));
    };

    /* Draw each planned wire. Forward: L-shape through the gap.
     * Backward: U-shape up above the modules, across, down into the
     * left-side port. */
    for (auto planIt = realWireNets.constBegin(); planIt != realWireNets.constEnd(); ++planIt) {
        const QString  &netName = planIt.key();
        const WirePlan &plan    = planIt.value();

        auto wire = std::make_shared<SchematicWire>();
        if (plan.isBackward) {
            /* U-shape: right port -> right past module -> up to detourY
             * -> across -> down to left port Y -> into left port. */
            wire->append_point(plan.rightPort);
            wire->append_point(QPointF(plan.rightPort.x() + stubLength, plan.rightPort.y()));
            wire->append_point(QPointF(plan.rightPort.x() + stubLength, plan.detourY));
            wire->append_point(QPointF(plan.leftPort.x() - stubLength, plan.detourY));
            wire->append_point(QPointF(plan.leftPort.x() - stubLength, plan.leftPort.y()));
            wire->append_point(plan.leftPort);
        } else {
            /* Forward L-shape through the gap. Collapses to a straight
             * line when the endpoints share a Y. */
            wire->append_point(plan.rightPort);
            if (!qFuzzyCompare(plan.rightPort.y(), plan.leftPort.y())) {
                const qreal midX = (plan.rightPort.x() + plan.leftPort.x()) / 2.0;
                wire->append_point(QPointF(midX, plan.rightPort.y()));
                wire->append_point(QPointF(midX, plan.leftPort.y()));
            }
            wire->append_point(plan.leftPort);
        }
        if (!scene.addWire(wire)) {
            continue;
        }
        realWireByNet[netName].append(wire);

        auto wm = scene.wire_manager();
        if (!wm) {
            continue;
        }
        /* scene.addWire always appends a fresh net at the tail of
         * wm->nets(), so the just-added wire's net is the last one. */
        const auto allNets = wm->nets();
        if (allNets.empty()) {
            continue;
        }
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(allNets.back());
        if (!wireNet) {
            continue;
        }
        wireNet->set_name(netName);

        QPointF labelCenter;
        if (plan.isBackward) {
            labelCenter = {(plan.rightPort.x() + plan.leftPort.x()) / 2.0, plan.detourY};
        } else if (qFuzzyCompare(plan.rightPort.y(), plan.leftPort.y())) {
            labelCenter = {(plan.rightPort.x() + plan.leftPort.x()) / 2.0, plan.rightPort.y()};
        } else {
            const qreal midX = (plan.rightPort.x() + plan.leftPort.x()) / 2.0;
            labelCenter      = {(plan.rightPort.x() + midX) / 2.0, plan.rightPort.y()};
        }
        if (auto tmpLabel = wireNet->label()) {
            labelCenter.ry() -= tmpLabel->boundingRect().height();
        }
        placeWireLabel(wireNet, netName, labelCenter);
    }

    /* Fanin / fanout trunk routing. For nets with ≥3 non-bus endpoints
     * where exactly one endpoint sits opposite an N≥2 cluster all in an
     * adjacent layer (e.g. two drivers converging on one consumer or
     * one driver feeding several consumers), draw a vertical trunk in
     * the inter-layer gap plus one horizontal branch per endpoint. The
     * branches share the trunk's X, so the wire-system merges them
     * into a single WireNet. Fully-broadcast nets (no side-split, or
     * spanning more than two layers) still fall back to stubs. */
    for (auto netIt = netToEndpoints.constBegin(); netIt != netToEndpoints.constEnd(); ++netIt) {
        const QString        &netName   = netIt.key();
        const QList<PortRef> &endpoints = netIt.value();
        if (realWireNets.contains(netName)) {
            continue;
        }
        if (endpoints.size() < 3 || endpoints.size() > 4) {
            continue;
        }

        QList<std::shared_ptr<SchematicConnector>> leftConns;
        QList<std::shared_ptr<SchematicConnector>> rightConns;
        bool                                       hasBus = false;
        for (const PortRef &ep : endpoints) {
            if (ep.isBus) {
                hasBus = true;
                break;
            }
            SchematicModule *module = placedByInstance.value(ep.instanceName, nullptr);
            if (!module) {
                hasBus = true;
                break;
            }
            const auto conn = findConnector(module, ep.portName);
            if (!conn) {
                hasBus = true;
                break;
            }
            if (conn->modulePosition() == SchematicConnector::Left) {
                leftConns.append(conn);
            } else if (conn->modulePosition() == SchematicConnector::Right) {
                rightConns.append(conn);
            } else {
                hasBus = true;
                break;
            }
        }
        if (hasBus) {
            continue;
        }

        std::shared_ptr<SchematicConnector>        singleton;
        QList<std::shared_ptr<SchematicConnector>> cluster;
        SchematicConnector::Position               singletonSide = SchematicConnector::Left;
        if (leftConns.size() == 1 && rightConns.size() >= 2) {
            singleton     = leftConns.first();
            cluster       = rightConns;
            singletonSide = SchematicConnector::Left;
        } else if (rightConns.size() == 1 && leftConns.size() >= 2) {
            singleton     = rightConns.first();
            cluster       = leftConns;
            singletonSide = SchematicConnector::Right;
        } else {
            continue;
        }

        /* Require the cluster to be in one layer and the singleton in
         * the adjacent layer, with the singleton's side consistent with
         * normal data flow (Left singleton = consumer in later layer,
         * Right singleton = driver in earlier layer). */
        auto instanceOf = [&](const std::shared_ptr<SchematicConnector> &conn) -> QString {
            for (const PortRef &ep : endpoints) {
                SchematicModule *module = placedByInstance.value(ep.instanceName, nullptr);
                if (module && module->connectors().contains(conn)) {
                    return ep.instanceName;
                }
            }
            return {};
        };

        const int singletonLayer = layerOf.value(instanceOf(singleton), -1);
        QSet<int> clusterLayers;
        for (const auto &conn : cluster) {
            clusterLayers.insert(layerOf.value(instanceOf(conn), -1));
        }
        if (clusterLayers.size() != 1) {
            continue;
        }
        const int clusterLayer = *clusterLayers.cbegin();
        if (std::abs(singletonLayer - clusterLayer) != 1) {
            continue;
        }
        if (singletonSide == SchematicConnector::Left && clusterLayer >= singletonLayer) {
            continue;
        }
        if (singletonSide == SchematicConnector::Right && clusterLayer <= singletonLayer) {
            continue;
        }

        /* Build trunk X at the gap midpoint and cover the full Y span. */
        const QPointF singletonPos = singleton->scenePos();
        qreal         trunkX       = singletonPos.x();
        qreal         minY         = singletonPos.y();
        qreal         maxY         = singletonPos.y();
        for (const auto &conn : cluster) {
            const QPointF pos = conn->scenePos();
            trunkX            = (trunkX + pos.x()) / 2.0; /* running pairwise mean */
            minY              = std::min(minY, pos.y());
            maxY              = std::max(maxY, pos.y());
        }
        /* Keep trunk away from both module bodies. */
        if (singletonSide == SchematicConnector::Left) {
            trunkX = std::max(trunkX, singletonPos.x() - (2.0 * stubLength));
            trunkX = std::min(trunkX, cluster.first()->scenePos().x() + (2.0 * stubLength));
        } else {
            trunkX = std::min(trunkX, singletonPos.x() + (2.0 * stubLength));
            trunkX = std::max(trunkX, cluster.first()->scenePos().x() - (2.0 * stubLength));
        }

        QList<std::shared_ptr<QSchematic::Items::Wire>> createdWires;

        /* Trunk wire: vertical line covering min..max Y at trunkX. */
        auto trunkWire = std::make_shared<SchematicWire>();
        trunkWire->append_point(QPointF(trunkX, minY));
        trunkWire->append_point(QPointF(trunkX, maxY));
        if (!scene.addWire(trunkWire)) {
            continue;
        }
        createdWires.append(trunkWire);
        realWireByNet[netName].append(trunkWire);

        /* Singleton branch: horizontal from singleton port to trunk. */
        auto singletonBranch = std::make_shared<SchematicWire>();
        singletonBranch->append_point(singletonPos);
        singletonBranch->append_point(QPointF(trunkX, singletonPos.y()));
        scene.addWire(singletonBranch);
        createdWires.append(singletonBranch);
        realWireByNet[netName].append(singletonBranch);

        /* Cluster branches: one horizontal wire per endpoint to trunk. */
        for (const auto &conn : cluster) {
            const QPointF pos    = conn->scenePos();
            auto          branch = std::make_shared<SchematicWire>();
            branch->append_point(pos);
            branch->append_point(QPointF(trunkX, pos.y()));
            scene.addWire(branch);
            createdWires.append(branch);
            realWireByNet[netName].append(branch);
        }

        auto wm = scene.wire_manager();
        if (!wm) {
            realWireNets.insert(netName, WirePlan{});
            continue;
        }
        /* Each scene.addWire call above appended exactly one fresh net
         * at the tail of wm->nets(); name the trailing createdWires.size()
         * nets (net-by-name grouping then merges them logically). */
        const auto allNets   = wm->nets();
        const int  tailCount = std::min<int>(allNets.size(), createdWires.size());
        bool       placed    = false;
        for (int i = static_cast<int>(allNets.size()) - tailCount;
             i < static_cast<int>(allNets.size());
             ++i) {
            auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(allNets.at(i));
            if (!wireNet) {
                continue;
            }
            wireNet->set_name(netName);
            if (!placed) {
                QPointF labelCenter(trunkX, (minY + maxY) / 2.0);
                if (auto tmpLabel = wireNet->label()) {
                    labelCenter.rx() += (tmpLabel->boundingRect().width() / 2.0) + 4.0;
                }
                placeWireLabel(wireNet, netName, labelCenter);
                placed = true;
            }
        }
        realWireNets.insert(netName, WirePlan{});
    }

    /* Crossing-threshold fallback. If a real-wire net tangles with too
     * many other real wires, delete its wires and let the stub loop
     * below redo it as per-endpoint labelled stubs. This prevents
     * dense SoCs from looking like a spiderweb even when every net in
     * isolation qualified as a simple forward/backward/T candidate. */
    auto segmentsOf =
        [](const QList<std::shared_ptr<QSchematic::Items::Wire>> &wires) -> QList<QLineF> {
        QList<QLineF> segs;
        for (const auto &wire : wires) {
            const auto pts = wire->pointsAbsolute();
            for (int i = 1; i < pts.size(); ++i) {
                segs.append(QLineF(pts.at(i - 1), pts.at(i)));
            }
        }
        return segs;
    };
    auto segmentsCross = [](const QLineF &aSeg, const QLineF &bSeg) -> bool {
        const bool aHoriz = qFuzzyCompare(aSeg.y1(), aSeg.y2());
        const bool bHoriz = qFuzzyCompare(bSeg.y1(), bSeg.y2());
        if (aHoriz == bHoriz) {
            return false;
        }
        const QLineF &hSeg = aHoriz ? aSeg : bSeg;
        const QLineF &vSeg = aHoriz ? bSeg : aSeg;
        const qreal   hy   = hSeg.y1();
        const qreal   hx1  = std::min(hSeg.x1(), hSeg.x2());
        const qreal   hx2  = std::max(hSeg.x1(), hSeg.x2());
        const qreal   vx   = vSeg.x1();
        const qreal   vy1  = std::min(vSeg.y1(), vSeg.y2());
        const qreal   vy2  = std::max(vSeg.y1(), vSeg.y2());
        /* Strict interior crossing only; touching an endpoint is not a
         * crossing (that is just a shared node, e.g. T-junction). */
        return (hx1 < vx) && (vx < hx2) && (vy1 < hy) && (hy < vy2);
    };

    const int                     crossingThreshold = 3;
    QStringList                   bustedNets;
    QHash<QString, QList<QLineF>> segmentCache;
    for (auto it = realWireByNet.constBegin(); it != realWireByNet.constEnd(); ++it) {
        segmentCache.insert(it.key(), segmentsOf(it.value()));
    }
    for (auto it = realWireByNet.constBegin(); it != realWireByNet.constEnd(); ++it) {
        const QList<QLineF> &mySegs     = segmentCache.value(it.key());
        int                  totalCross = 0;
        for (auto oit = realWireByNet.constBegin(); oit != realWireByNet.constEnd(); ++oit) {
            if (oit.key() == it.key()) {
                continue;
            }
            const QList<QLineF> &otherSegs = segmentCache.value(oit.key());
            for (const QLineF &mine : mySegs) {
                for (const QLineF &theirs : otherSegs) {
                    if (segmentsCross(mine, theirs)) {
                        totalCross++;
                    }
                }
            }
        }
        if (totalCross > crossingThreshold) {
            bustedNets.append(it.key());
        }
    }
    for (const QString &netName : bustedNets) {
        for (const auto &wire : realWireByNet.value(netName)) {
            scene.removeWire(wire);
        }
        realWireByNet.remove(netName);
        realWireNets.remove(netName);
    }

    /* Create a short stub wire for each endpoint of every remaining net
     * (multi-endpoint, non-adjacent, or bus, or busted-up) and label it
     * with the net name. WireNet::global_nets() groups same-named nets
     * into one logical connection, so label-based net-by-name acts as
     * the connection mechanism for these.
     *
     * Per-endpoint stub length steps up in lanes when neighbouring ports
     * on the same module side are close, so labels land at distinct
     * columns instead of piling onto the same X/Y line. */
    struct StubEndpoint
    {
        QString                             netName;
        SchematicModule                    *module;
        std::shared_ptr<SchematicConnector> connector;
        QPointF                             portScene;
        SchematicConnector::Position        side;
        int                                 lane;
    };
    QList<StubEndpoint> stubList;
    for (auto netIt = netToEndpoints.constBegin(); netIt != netToEndpoints.constEnd(); ++netIt) {
        const QString        &netName   = netIt.key();
        const QList<PortRef> &endpoints = netIt.value();
        if (realWireNets.contains(netName)) {
            continue;
        }
        for (const PortRef &ep : endpoints) {
            SchematicModule *module = placedByInstance.value(ep.instanceName, nullptr);
            if (!module) {
                continue;
            }
            const auto connector = findConnector(module, ep.portName);
            if (!connector) {
                continue;
            }
            stubList.append(
                {netName, module, connector, connector->scenePos(), connector->modulePosition(), 0});
        }
    }

    /* Stable order so the label-on-first-endpoint rule is reproducible
     * across imports and picks the top-left endpoint for each net. */
    std::sort(stubList.begin(), stubList.end(), [](const StubEndpoint &a, const StubEndpoint &b) {
        if (!qFuzzyCompare(a.portScene.y(), b.portScene.y())) {
            return a.portScene.y() < b.portScene.y();
        }
        if (!qFuzzyCompare(a.portScene.x(), b.portScene.x())) {
            return a.portScene.x() < b.portScene.x();
        }
        return a.netName < b.netName;
    });

    QHash<QPair<SchematicModule *, int>, QList<int>> buckets;
    for (int i = 0; i < stubList.size(); ++i) {
        buckets[{stubList[i].module, static_cast<int>(stubList[i].side)}].append(i);
    }
    constexpr qreal laneGapThreshold = 28.0;
    constexpr int   laneCount        = 3;
    for (auto bucketIt = buckets.begin(); bucketIt != buckets.end(); ++bucketIt) {
        QList<int> &indices = bucketIt.value();
        const int   side    = bucketIt.key().second;
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            if (side == SchematicConnector::Left || side == SchematicConnector::Right) {
                return stubList[a].portScene.y() < stubList[b].portScene.y();
            }
            return stubList[a].portScene.x() < stubList[b].portScene.x();
        });
        int   prevLane  = -1;
        qreal prevCoord = -1e9;
        for (int idx : indices) {
            const qreal coord = (side == SchematicConnector::Left
                                 || side == SchematicConnector::Right)
                                    ? stubList[idx].portScene.y()
                                    : stubList[idx].portScene.x();
            int         lane  = 0;
            if (prevLane >= 0 && std::abs(coord - prevCoord) < laneGapThreshold) {
                lane = (prevLane + 1) % laneCount;
            }
            stubList[idx].lane = lane;
            prevLane           = lane;
            prevCoord          = coord;
        }
    }

    /* A net with multiple stub endpoints shows its label on the first
     * one only; the rest stay nameless to cut visual clutter. set_name
     * still runs on every WireNet so net-by-name grouping is unaffected. */
    QHash<QString, int> netStubCount;
    for (const StubEndpoint &se : stubList) {
        netStubCount[se.netName]++;
    }
    QHash<QString, bool> netLabelShown;

    constexpr qreal staircaseStep = 70.0;
    for (const StubEndpoint &se : stubList) {
        const QString &netName   = se.netName;
        const auto    &connector = se.connector;
        const QPointF  portScene = se.portScene;
        const qreal    thisStub  = stubLength + se.lane * staircaseStep;
        QPointF        stubEnd   = portScene;
        switch (se.side) {
        case SchematicConnector::Left:
            stubEnd.setX(portScene.x() - thisStub);
            break;
        case SchematicConnector::Right:
            stubEnd.setX(portScene.x() + thisStub);
            break;
        case SchematicConnector::Top:
            stubEnd.setY(portScene.y() - thisStub);
            break;
        case SchematicConnector::Bottom:
            stubEnd.setY(portScene.y() + thisStub);
            break;
        }

        auto wire = std::make_shared<SchematicWire>();
        wire->append_point(portScene);
        wire->append_point(stubEnd);
        if (!scene.addWire(wire)) {
            continue;
        }

        auto wm = scene.wire_manager();
        if (!wm) {
            continue;
        }
        /* scene.addWire always appends a fresh net at the tail of
         * wm->nets(), so the just-added stub's net is the last one. */
        const auto allNets = wm->nets();
        if (allNets.empty()) {
            continue;
        }
        auto wireNet = std::dynamic_pointer_cast<QSchematic::Items::WireNet>(allNets.back());
        if (!wireNet) {
            continue;
        }
        wireNet->set_name(netName);
        const bool showLabel = (netStubCount.value(netName, 1) <= 1)
                               || !netLabelShown.value(netName, false);
        if (auto label = wireNet->label()) {
            if (!showLabel) {
                label->setVisible(false);
            } else {
                netLabelShown[netName] = true;
                label->setText(netName);
                const qreal labelW = label->boundingRect().width();
                const qreal labelH = label->boundingRect().height();
                QPointF     labelPos;
                switch (se.side) {
                case SchematicConnector::Left:
                    labelPos.setX(stubEnd.x() - labelW);
                    labelPos.setY(stubEnd.y() - labelH / 2);
                    break;
                case SchematicConnector::Right:
                    labelPos.setX(stubEnd.x());
                    labelPos.setY(stubEnd.y() - labelH / 2);
                    break;
                case SchematicConnector::Top:
                    labelPos.setX(stubEnd.x() - labelW / 2);
                    labelPos.setY(stubEnd.y() - labelH);
                    break;
                case SchematicConnector::Bottom:
                    labelPos.setX(stubEnd.x() - labelW / 2);
                    labelPos.setY(stubEnd.y());
                    break;
                }
                label->setRotation(0.0);
                label->setPos(labelPos);
            }
        }
    }

    /* Grow the sceneRect to fit the imported content so QGraphicsView
     * scrollbars can reach everything; the default 3000x3000 is smaller
     * than a multi-file SoC layout. */
    const QRectF itemsRect = scene.itemsBoundingRect();
    if (!itemsRect.isEmpty()) {
        const qreal sceneMargin = 500.0;
        scene.setSceneRect(itemsRect.adjusted(-sceneMargin, -sceneMargin, sceneMargin, sceneMargin));
    }

    /* autoNameWires runs once here; the scope guard will re-hook it to
     * netlistChanged for subsequent interactive edits. */
    autoNameWires();

    QSocConsole::info().noquote() << QStringLiteral(
                                         "Imported %1 instance(s), %2 net(s) from %3 file(s)")
                                         .arg(placedByInstance.size())
                                         .arg(netToEndpoints.size())
                                         .arg(filePaths.size());
    return true;
}
