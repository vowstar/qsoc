// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocconsole.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivecomb.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitiveseq.h"
#include "common/qsocgeneratereportunconnected.h"
#include "common/qsocpaths.h"
#include "common/qsocverilogutils.h"
#include "common/qstaticstringweaver.h"
#include "qsocgenerateprimitivefsm.h"
#include "qsocgenerateprimitivereset.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <fstream>
#include <iostream>
#include <limits>
#include <optional>

namespace {

std::optional<int> provenBuiltInWidth(const QString &type)
{
    static const QRegularExpression typeRegex(
        R"(^\s*(?:logic|wire|reg|bit|tri)(?:\s+(?:signed|unsigned))?)"
        R"(\s*(?:\[\s*(\d+)\s*(?::\s*(\d+)\s*)?\])?\s*$)",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = typeRegex.match(type);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    if (match.captured(1).isEmpty()) {
        return 1;
    }

    bool         msbOk = false;
    const qint64 msb   = match.captured(1).toLongLong(&msbOk);
    if (!msbOk || msb > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    qint64 width = msb + 1;
    if (!match.captured(2).isEmpty()) {
        bool         lsbOk = false;
        const qint64 lsb   = match.captured(2).toLongLong(&lsbOk);
        if (!lsbOk || lsb > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        width = msb >= lsb ? msb - lsb + 1 : lsb - msb + 1;
    }
    if (width <= 0 || width > QSocNumberInfo::MaximumDeclaredWidth) {
        return std::nullopt;
    }
    return static_cast<int>(width);
}

struct TieClassification
{
    QSocNumberInfo::NumericTextKind kind;
    QSocNumberInfo::Spelling        spelling;
    QString                         emittedText;
};

TieClassification classifyTie(const QString &value)
{
    const QSocNumberInfo::NumericText numeric = QSocNumberInfo::classifyNumericText(value);
    return {numeric.kind, numeric.spelling, QSocNumberInfo::normalizeHexBaseAliases(value)};
}

QString tieDiagnosticText(const QString &value)
{
    constexpr qsizetype maximumLength = 128;
    if (value.size() <= maximumLength) {
        return value;
    }
    return value.left(maximumLength - 3) + QStringLiteral("...");
}

} // namespace

bool QSocGenerateManager::generateVerilog(const QString &outputFileName)
{
    /* Create unconnected port reporter for collecting data */
    QSocGenerateReportUnconnected unconnectedPortReporter;

    /* Check if netlistData is valid (instance section is now optional) */

    // Check if instance section exists and is valid when present
    if (netlistData["instance"] && !netlistData["instance"].IsMap()) {
        QSocConsole::error() << "Invalid netlist data, 'instance' section is not a map";
        return false;
    }

    // Allow empty or missing instance section if comb, seq, or fsm section exists
    bool hasInstances  = netlistData["instance"] && netlistData["instance"].IsMap()
                         && netlistData["instance"].size() > 0;
    bool hasCombSeqFsm = netlistData["comb"] || netlistData["seq"] || netlistData["fsm"];
    bool hasReset      = netlistData["reset"] && netlistData["reset"].IsSequence()
                         && netlistData["reset"].size() > 0;
    bool hasClock      = netlistData["clock"] && netlistData["clock"].IsSequence()
                         && netlistData["clock"].size() > 0;
    bool hasPower      = netlistData["power"] && netlistData["power"].IsSequence()
                         && netlistData["power"].size() > 0;

    if (!hasInstances && !hasCombSeqFsm && !hasReset && !hasClock && !hasPower) {
        QSocConsole::error() << "Invalid netlist data, no 'instance' section and no 'comb', "
                                "'seq', 'fsm', 'reset', 'clock', or 'power' section found";
        return false;
    }

    /* Check if net section exists and has valid format if present */
    if (netlistData["net"] && !netlistData["net"].IsMap()) {
        QSocConsole::error() << "Invalid netlist data, 'net' section is not a map";
        return false;
    }

    /* Check if project manager is valid */
    if (!projectManager) {
        QSocConsole::error() << "Project manager is null";
        return false;
    }

    if (!projectManager->isValidOutputPath(true)) {
        QSocConsole::error() << "Invalid output path: " << projectManager->getOutputPath();
        return false;
    }

    const auto outputArtifact
        = QSocPaths::resolveArtifactPath(projectManager->getOutputPath(), outputFileName + ".v");
    if (!outputArtifact.isValid()) {
        QSocConsole::error() << outputArtifact.error;
        return false;
    }
    const QString outputFilePath = outputArtifact.path;

    /* Open output file for writing */
    QFile outputFile(outputFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QSocConsole::error() << "Failed to open output file for writing:" << outputFilePath;
        return false;
    }

    QTextStream out(&outputFile);

    /* Generate file header */
    out << "/**\n";
    out << " * @file " << outputFileName << ".v\n";
    out << " * @brief RTL implementation of " << outputFileName << "\n";
    out << " *\n";
    out << " * @details This file contains RTL implementation based on the input netlist.\n"
        << " *          Auto-generated RTL Verilog file. Generated by QSoC.\n";
    out << " * NOTE: Auto-generated file, do not edit manually.\n";
    out << " */\n\n";

    out << "`timescale 1ns / 1ps\n\n";

    /* A primitive that fails to generate leaves its controller out of the
     * output; the caller must not report success for a partial design. */
    bool primitiveFailed = false;

    /* Generate reset primitive controllers first (before top-level module) */
    if (netlistData["reset"] && netlistData["reset"].IsSequence()
        && netlistData["reset"].size() > 0) {
        for (size_t i = 0; i < netlistData["reset"].size(); ++i) {
            const YAML::Node &resetItem = netlistData["reset"][i];

            if (!resetItem.IsMap()) {
                QSocConsole::warn() << "Skipping invalid reset item at index" << i;
                continue;
            }

            if (!generateResetPrimitive(resetItem, out)) {
                QSocConsole::error() << "Failed to generate reset primitive at index" << i;
                primitiveFailed = true;
                continue;
            }

            /* Add blank line between different reset blocks */
            if (i < netlistData["reset"].size() - 1) {
                out << "\n";
            }
        }
    }

    /* Generate clock primitive controllers second (before top-level module) */
    if (netlistData["clock"] && netlistData["clock"].IsSequence()
        && netlistData["clock"].size() > 0) {
        for (size_t i = 0; i < netlistData["clock"].size(); ++i) {
            const YAML::Node &clockItem = netlistData["clock"][i];

            if (!clockItem.IsMap()) {
                QSocConsole::warn() << "Skipping invalid clock item at index" << i;
                continue;
            }

            if (!generateClockPrimitive(clockItem, out)) {
                QSocConsole::error() << "Failed to generate clock primitive at index" << i;
                primitiveFailed = true;
                continue;
            }

            /* Add blank line between different clock blocks */
            if (i < netlistData["clock"].size() - 1) {
                out << "\n";
            }
        }
    }

    /* Generate power primitive controllers third (before top-level module) */
    if (netlistData["power"] && netlistData["power"].IsSequence()
        && netlistData["power"].size() > 0) {
        for (size_t i = 0; i < netlistData["power"].size(); ++i) {
            const YAML::Node &powerItem = netlistData["power"][i];

            if (!powerItem.IsMap()) {
                QSocConsole::warn() << "Skipping invalid power item at index" << i;
                continue;
            }

            if (!generatePowerPrimitive(powerItem, out)) {
                QSocConsole::error() << "Failed to generate power primitive at index" << i;
                primitiveFailed = true;
                continue;
            }

            /* Add blank line between different power blocks */
            if (i < netlistData["power"].size() - 1) {
                out << "\n";
            }
        }
    }

    /* Generate FSM primitive controllers fourth (before top-level module) */
    if (netlistData["fsm"] && netlistData["fsm"].IsSequence() && netlistData["fsm"].size() > 0) {
        for (size_t i = 0; i < netlistData["fsm"].size(); ++i) {
            const YAML::Node &fsmItem = netlistData["fsm"][i];

            if (!fsmItem.IsMap() || !fsmItem["name"] || !fsmItem["name"].IsScalar()
                || !fsmItem["clk"] || !fsmItem["clk"].IsScalar() || !fsmItem["rst"]
                || !fsmItem["rst"].IsScalar() || !fsmItem["rst_state"]
                || !fsmItem["rst_state"].IsScalar()) {
                QSocConsole::warn() << "FSM" << i << "has invalid format, skipping";
                continue; /* Skip invalid FSM items */
            }

            if (!generateFSMPrimitive(fsmItem, out)) {
                QSocConsole::error() << "Failed to generate FSM primitive at index" << i;
                primitiveFailed = true;
                continue;
            }

            /* Add blank line between different FSM blocks */
            if (i < netlistData["fsm"].size() - 1) {
                out << "\n";
            }
        }
    }

    /* Check if we need to generate a top-level module */
    bool hasPorts = netlistData["port"] && netlistData["port"].IsMap()
                    && netlistData["port"].size() > 0;
    bool hasNets  = netlistData["net"] && netlistData["net"].IsMap()
                    && netlistData["net"].size() > 0;
    bool hasBus = netlistData["bus"] && netlistData["bus"].IsMap() && netlistData["bus"].size() > 0;

    bool needsTopLevelModule = hasInstances || hasPorts || hasNets || hasBus;

    if (!needsTopLevelModule) {
        /* No top-level module needed */
        if (hasCombSeqFsm) {
            /* If only comb/seq exist, generate a wrapper module for them */
            out << "module " << outputFileName << " ();\n\n";

            /* Generate combinational logic */
            if (!generateCombPrimitive(netlistData, {}, out)) {
                QSocConsole::warn() << "Failed to generate combinational logic primitives";
                return false;
            }

            /* Generate sequential logic */
            if (!generateSeqPrimitive(netlistData, {}, out)) {
                QSocConsole::warn() << "Failed to generate sequential logic primitives";
                return false;
            }

            out << "\nendmodule\n";
        }

        outputFile.close();
        QSocConsole::info() << "Successfully generated Verilog file:" << outputFilePath;
        return !primitiveFailed;
    }

    /* Generate top-level module declaration */
    out << "module " << outputFileName;

    /* Add module parameters if they exist */
    if (netlistData["parameter"]) {
        if (!netlistData["parameter"].IsMap()) {
            QSocConsole::warn() << "Top-level 'parameter' section is not a map, ignoring";
        } else if (netlistData["parameter"].size() == 0) {
            /* Empty map -> no parameters; not an error. */
        } else {
            out << " #(\n";
            QStringList   paramDeclarations;
            QSet<QString> seenParamNames;

            for (auto paramIter = netlistData["parameter"].begin();
                 paramIter != netlistData["parameter"].end();
                 ++paramIter) {
                if (!paramIter->first.IsScalar()) {
                    QSocConsole::warn() << "Invalid parameter name, skipping";
                    continue;
                }

                const QString paramName = QString::fromStdString(paramIter->first.as<std::string>());
                if (!QSocVerilogUtils::isValidVerilogIdentifier(paramName)) {
                    QSocConsole::warn() << "Parameter name" << paramName
                                        << "is not a valid Verilog identifier "
                                           "(reserved keyword or illegal character)";
                }
                if (seenParamNames.contains(paramName)) {
                    QSocConsole::warn() << "Duplicate parameter name" << paramName
                                        << "; only the first definition is emitted";
                    continue;
                }
                seenParamNames.insert(paramName);

                if (!paramIter->second.IsMap()) {
                    QSocConsole::warn()
                        << "Parameter" << paramName << "has invalid format, skipping";
                    continue;
                }

                /* Default to empty for Verilog 2001 */
                QString paramType  = "";
                QString paramValue = "";

                if (paramIter->second["type"] && paramIter->second["type"].IsScalar()) {
                    paramType = QString::fromStdString(paramIter->second["type"].as<std::string>());
                    /* Clean type for Verilog 2001 compatibility */
                    paramType = QSocGenerateManager::cleanTypeForWireDeclaration(paramType);

                    /* Add a space if type isn't empty after processing */
                    if (!paramType.isEmpty() && !paramType.endsWith(" ")) {
                        paramType += " ";
                    }
                }

                if (paramIter->second["value"] && paramIter->second["value"].IsScalar()) {
                    paramValue = QString::fromStdString(
                        paramIter->second["value"].as<std::string>());
                }

                paramDeclarations.append(
                    QString("    parameter %1%2 = %3").arg(paramType).arg(paramName).arg(paramValue));
            }

            if (!paramDeclarations.isEmpty()) {
                out << paramDeclarations.join(",\n") << "\n";
            }
            out << ")";
        }
    }

    /* Start port list */
    out << " (";

    /* Collect all ports for module interface */
    QStringList            ports;
    QMap<QString, QString> declaredSignalRanges;
    /* Full membership per net. Routing and alias emission both read this,
       then choose ownership by direction from the same component. */
    const QMap<QString, NetTopPorts> netToTopPortAliases = QSocGenerateManager::buildNetToTopPorts(
        netlistData);

    /* Process port section if it exists */
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        QSet<QString> seenPortNames;
        for (auto portIter = netlistData["port"].begin(); portIter != netlistData["port"].end();
             ++portIter) {
            if (!portIter->first.IsScalar()) {
                QSocConsole::warn() << "Invalid port name, skipping";
                continue;
            }

            const QString portName = QString::fromStdString(portIter->first.as<std::string>());
            if (!QSocVerilogUtils::isValidVerilogIdentifier(portName)) {
                QSocConsole::warn()
                    << "Top-level port name" << portName
                    << "is not a valid Verilog identifier (reserved keyword or illegal character)";
            }
            if (seenPortNames.contains(portName)) {
                QSocConsole::warn() << "Duplicate top-level port name" << portName
                                    << "; only the first definition is emitted";
                continue;
            }
            seenPortNames.insert(portName);

            if (!portIter->second.IsMap()) {
                QSocConsole::warn() << "Port" << portName << "has invalid format, skipping";
                continue;
            }

            QString direction = "input";
            QString type      = ""; /* Empty type by default for Verilog 2001 */

            if (portIter->second["direction"] && portIter->second["direction"].IsScalar()) {
                const QString dirStr = QString::fromStdString(
                                           portIter->second["direction"].as<std::string>())
                                           .toLower();

                /* Handle both full and abbreviated forms */
                if (dirStr == "out" || dirStr == "output") {
                    direction = "output";
                } else if (dirStr == "in" || dirStr == "input") {
                    direction = "input";
                } else if (dirStr == "inout") {
                    direction = "inout";
                }
            }

            /* Get port type/width information if present */
            if (portIter->second["type"] && portIter->second["type"].IsScalar()) {
                type = QString::fromStdString(portIter->second["type"].as<std::string>());
                /* Clean type for Verilog 2001 compatibility */
                type = QSocGenerateManager::cleanTypeForWireDeclaration(type);
            }

            /* Add port declaration */
            declaredSignalRanges.insert(portName, type);
            if (direction == "input" || direction == "output") {
                ports.append(QString("%1 wire %2")
                                 .arg(direction)
                                 .arg(type.isEmpty() ? portName : type + " " + portName));
            } else {
                ports.append(QString("%1 %2").arg(direction).arg(
                    type.isEmpty() ? portName : type + " " + portName));
            }
        }
    }

    /* Close module declaration */
    if (!ports.isEmpty()) {
        /* If we have parameters, add a comma after them */
        out << "\n    " << ports.join(",\n    ") << "\n";
    }
    out << ");\n\n";

    /* Build a mapping of all connections for each instance and port */
    QMap<QString, QMap<QString, QString>> instancePortConnections;

    /* Nets where a single instance.port appeared more than once. Carries
       across to wire-decl emission so a FIXME comment surfaces in the .v. */
    QSet<QString> duplicateConnectionNets;

    /* Track which net first claimed each (instance, port) pair so cross-net
       duplicates can be flagged. A port is a single signal; routing it to
       two different nets silently leaves all but one net dangling. */
    QMap<QPair<QString, QString>, QString> instancePortToFirstNet;

    /* Top-level port names. A net whose name matches a top-level port is
       served by the port itself; emitting an extra `wire` produces a
       duplicate identifier. */
    QSet<QString> topLevelPortNames;
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (auto portIter = netlistData["port"].begin(); portIter != netlistData["port"].end();
             ++portIter) {
            if (portIter->first.IsScalar()) {
                topLevelPortNames.insert(QString::fromStdString(portIter->first.as<std::string>()));
            }
        }
    }
    QMap<QString, QPair<QStringList, QStringList>> instanceGuards;
    QSet<QString>                                  emittableInstanceNames;
    if (netlistData["instance"] && netlistData["instance"].IsMap()) {
        QSet<QString> seenInstanceNames;
        for (auto instanceIter = netlistData["instance"].begin();
             instanceIter != netlistData["instance"].end();
             ++instanceIter) {
            if (!instanceIter->first.IsScalar()) {
                continue;
            }
            const QString instanceName = QString::fromStdString(
                instanceIter->first.as<std::string>());
            if (seenInstanceNames.contains(instanceName)) {
                continue;
            }
            seenInstanceNames.insert(instanceName);
            if (!instanceIter->second || !instanceIter->second.IsMap()
                || !instanceIter->second["module"] || !instanceIter->second["module"].IsScalar()) {
                continue;
            }
            QStringList ifdefList;
            QStringList ifndefList;
            if (parseMacroCondition(instanceIter->second, instanceName, ifdefList, ifndefList)) {
                emittableInstanceNames.insert(instanceName);
                instanceGuards.insert(instanceName, qMakePair(ifdefList, ifndefList));
            }
        }
    }
    enum class InstancePortRole { Input, Output, Inout, Unowned, Dropped };
    const auto instancePortRole = [&](const QString &instanceName, const QString &portName) {
        if (!emittableInstanceNames.contains(instanceName) || !netlistData["instance"]
            || !netlistData["instance"][instanceName.toStdString()]
            || !netlistData["instance"][instanceName.toStdString()]["module"]
            || !netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
            return InstancePortRole::Dropped;
        }
        const QString moduleName = QString::fromStdString(
            netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());
        if (!moduleManager || !moduleManager->isModuleExist(moduleName)) {
            return InstancePortRole::Unowned;
        }
        const YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);
        if (!moduleData["port"] || !moduleData["port"].IsMap()
            || !moduleData["port"][portName.toStdString()]) {
            return InstancePortRole::Dropped;
        }
        const YAML::Node portData = moduleData["port"][portName.toStdString()];
        if (!portData["direction"] || !portData["direction"].IsScalar()) {
            return InstancePortRole::Unowned;
        }
        const QString direction
            = QString::fromStdString(portData["direction"].as<std::string>()).toLower();
        if (direction == QStringLiteral("in") || direction == QStringLiteral("input")) {
            return InstancePortRole::Input;
        }
        if (direction == QStringLiteral("out") || direction == QStringLiteral("output")) {
            return InstancePortRole::Output;
        }
        if (direction == QStringLiteral("inout")) {
            return InstancePortRole::Inout;
        }
        return InstancePortRole::Unowned;
    };
    QSet<QString> forcedInternalWireNets;

    /* First, create the instancePortConnections map with port connections */
    /* This needs to be done before wire generation to ensure port names are used */
    if (netlistData["net"] && netlistData["net"].IsMap()) {
        for (auto netIter = netlistData["net"].begin(); netIter != netlistData["net"].end();
             ++netIter) {
            if (!netIter->first.IsScalar()) {
                continue;
            }

            const QString netName = QString::fromStdString(netIter->first.as<std::string>());

            /* Check if this net is connected to a top-level port */
            const NetTopPorts netTopPorts        = netToTopPortAliases.value(netName);
            const bool        connectedToTopPort = !netTopPorts.members.isEmpty()
                                                   || !netTopPorts.slices.isEmpty();
            /* A net is carried by its head sink so an instance driver lands
               on a port that may legally sit on the left of an assign;
               instance connections land on that port and slice together. */
            QString connectedPortNameValue;
            QString connectedPortSliceValue;
            {
                bool headFound = false;
                for (const QString &member : netTopPorts.members) {
                    if (topPortDirection(netlistData, member) == QStringLiteral("output")) {
                        connectedPortNameValue = member;
                        headFound              = true;
                        break;
                    }
                }
                if (!headFound) {
                    for (const auto &candidate : netTopPorts.slices) {
                        if (candidate.direction == QStringLiteral("output")) {
                            connectedPortNameValue  = candidate.port;
                            connectedPortSliceValue = candidate.slice;
                            headFound               = true;
                            break;
                        }
                    }
                }
                if (!headFound && !netTopPorts.members.isEmpty()) {
                    connectedPortNameValue = netTopPorts.members.first();
                } else if (!headFound && !netTopPorts.slices.isEmpty()) {
                    connectedPortNameValue  = netTopPorts.slices.first().port;
                    connectedPortSliceValue = netTopPorts.slices.first().slice;
                }
            }
            const QString &connectedPortName  = connectedPortNameValue;
            const QString &connectedPortSlice = connectedPortSliceValue;

            try {
                /* Build connections using List format only */
                const YAML::Node netNode = netIter->second;
                if (netNode.IsSequence()) {
                    /* Per-net guard: a single instance.port can connect to a net at
                       most once. A second entry would silently overwrite the first
                       in instancePortConnections (a QMap), losing the user's wiring. */
                    QSet<QPair<QString, QString>> seenInstancePort;

                    /* Process List format connections */
                    for (const auto &connectionNode : netNode) {
                        if (!connectionNode.IsMap()) {
                            QSocConsole::warn() << "Invalid connection node in net" << netName;
                            continue;
                        }

                        /* Get instance name */
                        if (!connectionNode["instance"] || !connectionNode["instance"].IsScalar()) {
                            QSocConsole::warn()
                                << "No instance name in connection for net" << netName;
                            continue;
                        }
                        const QString instanceName = QString::fromStdString(
                            connectionNode["instance"].as<std::string>());

                        /* Get port name */
                        if (!connectionNode["port"] || !connectionNode["port"].IsScalar()) {
                            QSocConsole::warn() << "No port name in connection for net" << netName;
                            continue;
                        }
                        const QString portName = QString::fromStdString(
                            connectionNode["port"].as<std::string>());

                        /* A top-level port may bind several distinct slices to
                           one net, so `top` keys carry the slice; an instance
                           port takes a single named connection either way. */
                        QString dedupPortKey = portName;
                        if (instanceName == "top" && connectionNode["bits"]
                            && connectionNode["bits"].IsScalar()) {
                            dedupPortKey += QSocVerilogUtils::normalizeBitSelect(
                                QString::fromStdString(connectionNode["bits"].as<std::string>()));
                        }
                        const auto instancePortKey = qMakePair(instanceName, dedupPortKey);
                        if (seenInstancePort.contains(instancePortKey)) {
                            QSocConsole::warn()
                                << "Net" << netName << "lists" << instanceName << "." << portName
                                << "more than once; only the first connection is kept";
                            duplicateConnectionNets.insert(netName);
                            continue;
                        }
                        seenInstancePort.insert(instancePortKey);

                        /* Top-level pseudo-instance "top" is a routing alias and
                           may legitimately appear on several nets simultaneously. */
                        if (instanceName != "top") {
                            const auto firstNetIt = instancePortToFirstNet.constFind(
                                instancePortKey);
                            if (firstNetIt != instancePortToFirstNet.constEnd()
                                && firstNetIt.value() != netName) {
                                QSocConsole::warn()
                                    << instanceName << "." << portName << "is wired to both nets"
                                    << firstNetIt.value() << "and" << netName
                                    << "; only the first net keeps the connection";
                                duplicateConnectionNets.insert(netName);
                                duplicateConnectionNets.insert(firstNetIt.value());
                                continue;
                            }
                            instancePortToFirstNet.insert(instancePortKey, netName);
                        }

                        /* Check if this port has invert attribute */
                        bool hasInvert = false;
                        if (netlistData["instance"]
                            && netlistData["instance"][instanceName.toStdString()]
                            && netlistData["instance"][instanceName.toStdString()]["port"]
                            && netlistData["instance"][instanceName.toStdString()]["port"]
                                          [portName.toStdString()]) {
                            auto portNode = netlistData["instance"][instanceName.toStdString()]
                                                       ["port"][portName.toStdString()];
                            if (portNode.IsMap() && portNode["invert"]
                                && portNode["invert"].IsScalar()) {
                                /* Use direct YAML boolean parsing */
                                if (portNode["invert"].as<bool>()) {
                                    hasInvert = true;
                                }
                            }
                        }

                        /* Check if this port has bits selection attribute */
                        QString bitSelect = "";
                        if (connectionNode["bits"] && connectionNode["bits"].IsScalar()) {
                            bitSelect = QSocVerilogUtils::normalizeBitSelect(
                                QString::fromStdString(connectionNode["bits"].as<std::string>()));
                        }

                        bool useTopPort = connectedToTopPort;
                        if (useTopPort && instanceName != "top") {
                            const InstancePortRole role = instancePortRole(instanceName, portName);
                            const QString carrierDirection = QSocGenerateManager::topPortDirection(
                                netlistData, connectedPortName);
                            if (carrierDirection == QStringLiteral("input")
                                && role == InstancePortRole::Output) {
                                useTopPort = false;
                                if (!topLevelPortNames.contains(netName)) {
                                    forcedInternalWireNets.insert(netName);
                                }
                            }
                        }

                        /* If connected to top-level port, use the port name instead of net name */
                        if (useTopPort) {
                            QString select = bitSelect;
                            /* `top` entries are the bindings themselves, not
                               consumers of the net. */
                            if (!connectedPortSlice.isEmpty() && instanceName != "top") {
                                select = connectedPortSlice;
                            }
                            instancePortConnections[instanceName][portName]
                                = hasInvert ? QString("~%1%2").arg(connectedPortName).arg(select)
                                            : QString("%1%2").arg(connectedPortName).arg(select);
                        } else if (
                            instanceName != "top" && topLevelPortNames.contains(netName)
                            && topPortDirection(netlistData, netName) == QStringLiteral("input")
                            && instancePortRole(instanceName, portName)
                                   == InstancePortRole::Output) {
                            instancePortConnections[instanceName][portName] = {};
                            QSocConsole::warn()
                                << "Net" << netName
                                << "cannot carry an instance output without driving the "
                                   "same-named top-level input; leaving"
                                << instanceName << "." << portName << "unconnected";
                        } else {
                            instancePortConnections[instanceName][portName]
                                = hasInvert ? QString("~%1%2").arg(netName).arg(
                                                  bitSelect.isEmpty() ? "" : bitSelect)
                                            : QString("%1%2").arg(netName).arg(
                                                  bitSelect.isEmpty() ? "" : bitSelect);
                        }
                    }
                } else {
                    QSocConsole::warn()
                        << "Net" << netName << "is not in List format, skipping connections";
                }
            } catch (const std::exception &e) {
                QSocConsole::warn() << "failed to process net:" << e.what();
            }
        }
    }

    /* Add connections (wires) section comment */
    out << "    /* Wire declarations */\n";

    /* Generate wire declarations FIRST */
    if (netlistData["net"]) {
        if (!netlistData["net"].IsMap()) {
            QSocConsole::warn() << "'net' section is not a map, skipping wire declarations";
        } else if (netlistData["net"].size() == 0) {
            QSocConsole::warn() << "'net' section is empty, no wire declarations to generate";
        } else {
            /* Collect comb/seq/fsm signals (inputs and outputs) once before the loop */
            const QList<PortDetailInfo> combSeqFsmSignals = collectCombSeqFsmSignals();

            /* Track nets that resolved to scalar wires so per-port [0] / [0:0]
               selects on those nets can be scrubbed before instantiation. */
            QSet<QString> scalarNets;

            for (auto netIter = netlistData["net"].begin(); netIter != netlistData["net"].end();
                 ++netIter) {
                if (!netIter->first.IsScalar()) {
                    QSocConsole::warn() << "Invalid net name, skipping";
                    continue;
                }

                const QString netName = QString::fromStdString(netIter->first.as<std::string>());
                if (!QSocVerilogUtils::isValidVerilogIdentifier(netName)) {
                    QSocConsole::warn() << "Net name" << netName
                                        << "is not a valid Verilog identifier "
                                           "(reserved keyword or illegal character)";
                }

                if (!netIter->second) {
                    QSocConsole::warn() << "Net" << netName << "has null data, skipping";
                    continue;
                }

                /* Net connections should be a sequence (list) of instance-port pairs */
                if (!netIter->second.IsSequence()) {
                    QSocConsole::warn() << "Net" << netName << "is not a sequence, skipping";
                    continue;
                }

                const YAML::Node connections = netIter->second;

                if (connections.size() == 0) {
                    QSocConsole::warn() << "Net" << netName << "has no connections, skipping";
                    continue;
                }

                /* Build a list of instance-port pairs for width check */
                QList<PortConnection> portConnections;
                /* Collect detailed port information for each connection */
                QList<PortDetailInfo> portDetails;

                /* Check if this net is connected to a top-level port */
                const NetTopPorts netTopPorts        = netToTopPortAliases.value(netName);
                const bool        connectedToTopPort = !netTopPorts.members.isEmpty()
                                                       || !netTopPorts.slices.isEmpty();
                QString           connectedPortNameValue;
                QString           connectedHeadSliceValue;
                {
                    bool headFound = false;
                    for (const QString &member : netTopPorts.members) {
                        if (topPortDirection(netlistData, member) == QStringLiteral("output")) {
                            connectedPortNameValue = member;
                            headFound              = true;
                            break;
                        }
                    }
                    if (!headFound) {
                        for (const auto &candidate : netTopPorts.slices) {
                            if (candidate.direction == QStringLiteral("output")) {
                                connectedPortNameValue  = candidate.port;
                                connectedHeadSliceValue = candidate.slice;
                                headFound               = true;
                                break;
                            }
                        }
                    }
                    if (!headFound && !netTopPorts.members.isEmpty()) {
                        connectedPortNameValue = netTopPorts.members.first();
                    } else if (!headFound && !netTopPorts.slices.isEmpty()) {
                        connectedPortNameValue  = netTopPorts.slices.first().port;
                        connectedHeadSliceValue = netTopPorts.slices.first().slice;
                    }
                }
                const QString &connectedPortName     = connectedPortNameValue;
                QString        topLevelPortDirection = "unknown";
                QString reversedDirection = "unknown"; /* Default fallback, defined in outer scope */

                if (connectedToTopPort) {
                    {
                        /* Get the port direction */
                        if (netlistData["port"]
                            && netlistData["port"][connectedPortName.toStdString()]
                            && netlistData["port"][connectedPortName.toStdString()]["direction"]
                            && netlistData["port"][connectedPortName.toStdString()]["direction"]
                                   .IsScalar()) {
                            const QString dirStr
                                = QString::fromStdString(
                                      netlistData["port"][connectedPortName.toStdString()]
                                                 ["direction"]
                                                     .as<std::string>())
                                      .toLower();

                            /* Store original direction for later use */
                            if (dirStr == "out" || dirStr == "output") {
                                topLevelPortDirection = "output";
                            } else if (dirStr == "in" || dirStr == "input") {
                                topLevelPortDirection = "input";
                            } else if (dirStr == "inout") {
                                topLevelPortDirection = "inout";
                            }

                            /* Reverse the direction for internal checking */
                            if (topLevelPortDirection == "output") {
                                reversedDirection
                                    = "input"; /* Top-level output is an input for internal nets */
                            } else if (topLevelPortDirection == "input") {
                                reversedDirection
                                    = "output"; /* Top-level input is an output for internal nets */
                            } else if (topLevelPortDirection == "inout") {
                                reversedDirection = "inout"; /* Bidirectional remains bidirectional */
                            }
                        }

                        /* Add top-level port to connection list */
                        portConnections.append(
                            PortConnection::createTopLevelPort(connectedPortName));

                        /* Get port width */
                        QString portWidthSpec = "";

                        /* Get port width/type from the same node we used for direction */
                        if (netlistData["port"]
                            && netlistData["port"][connectedPortName.toStdString()]) {
                            auto portNode = netlistData["port"][connectedPortName.toStdString()];

                            /* Get port width/type */
                            if (portNode["type"] && portNode["type"].IsScalar()) {
                                portWidthSpec = QString::fromStdString(
                                    portNode["type"].as<std::string>());
                            }
                        }

                        /* Initialize bitSelection as empty string */
                        const QString bitSelection = "";

                        /* CRITICAL FIX: Top-level port direction internal/external viewpoint
                         *
                         * Top-level OUTPUT port:
                         *   - External viewpoint: outputs signal to outside world
                         *   - Internal viewpoint: receives signal from internal logic (acts as INPUT)
                         *   - Internal module OUTPUT drives top-level OUTPUT = VALID (not multidriven)
                         *
                         * Top-level INPUT port:
                         *   - External viewpoint: receives signal from outside world
                         *   - Internal viewpoint: provides signal to internal logic (acts as OUTPUT)
                         *   - Internal module INPUT connects to top-level INPUT = VALID (not undriven)
                         *
                         * Store ORIGINAL direction here - checkPortDirectionConsistencyWithBitOverlap
                         * will handle the internal/external direction conversion uniformly.
                         */
                        /* Add to detailed port information with original direction */
                        portDetails.append(
                            PortDetailInfo::createTopLevelPort(
                                connectedPortName,
                                portWidthSpec,
                                topLevelPortDirection,
                                bitSelection));
                    }

                    /* The remaining members and every slice binding carry the
                       same net; without them an input-slice source is invisible
                       here and the net is misreported as undriven. */
                    bool headDetailSkipped = false;
                    const auto appendTopDetail = [&](const QString &portName, const QString &slice) {
                        if (!headDetailSkipped && portName == connectedPortName
                            && slice == connectedHeadSliceValue) {
                            headDetailSkipped = true;
                            return;
                        }
                        QString widthSpec;
                        if (netlistData["port"] && netlistData["port"][portName.toStdString()]) {
                            const auto portNode = netlistData["port"][portName.toStdString()];
                            if (portNode["type"] && portNode["type"].IsScalar()) {
                                widthSpec = QString::fromStdString(
                                    portNode["type"].as<std::string>());
                            }
                        }
                        portDetails.append(
                            PortDetailInfo::createTopLevelPort(
                                portName, widthSpec, topPortDirection(netlistData, portName), slice));
                    };
                    for (const QString &member : netTopPorts.members) {
                        appendTopDetail(member, QString());
                    }
                    for (const auto &bound : netTopPorts.slices) {
                        appendTopDetail(bound.port, bound.slice);
                    }
                }

                /* Build port connections from netlistData */
                const YAML::Node &netNode = netlistData["net"][netName.toStdString()];
                if (netNode.IsSequence()) {
                    for (const auto &connectionNode : netNode) {
                        if (!connectionNode.IsMap()) {
                            QSocConsole::warn() << "Invalid connection node in net" << netName;
                            continue;
                        }

                        /* Get instance name */
                        if (!connectionNode["instance"] || !connectionNode["instance"].IsScalar()) {
                            QSocConsole::warn()
                                << "No instance name in connection for net" << netName;
                            continue;
                        }
                        const QString instanceName = QString::fromStdString(
                            connectionNode["instance"].as<std::string>());

                        /* Get port name */
                        if (!connectionNode["port"] || !connectionNode["port"].IsScalar()) {
                            QSocConsole::warn() << "No port name in connection for net" << netName;
                            continue;
                        }
                        const QString portName = QString::fromStdString(
                            connectionNode["port"].as<std::string>());

                        if (instanceName != "top"
                            && !emittableInstanceNames.contains(instanceName)) {
                            continue;
                        }

                        /* Create a module port connection */
                        portConnections.append(
                            PortConnection::createModulePort(instanceName, portName));

                        /* Get additional details for this port */
                        QString portWidthSpec = "";
                        QString portDirection = "unknown";

                        /* Check if this connection has preserved type information from bus expansion */
                        if (connectionNode["type"] && connectionNode["type"].IsScalar()) {
                            portWidthSpec = QString::fromStdString(
                                connectionNode["type"].as<std::string>());
                        }

                        /* Check if this port has bits selection attribute */
                        QString bitSelection = "";
                        if (connectionNode["bits"] && connectionNode["bits"].IsScalar()) {
                            bitSelection = QSocVerilogUtils::normalizeBitSelect(
                                QString::fromStdString(connectionNode["bits"].as<std::string>()));
                        }

                        /* Get instance's module */
                        if (netlistData["instance"][instanceName.toStdString()]
                            && netlistData["instance"][instanceName.toStdString()]["module"]
                            && netlistData["instance"][instanceName.toStdString()]["module"]
                                   .IsScalar()) {
                            const QString moduleName = QString::fromStdString(
                                netlistData["instance"][instanceName.toStdString()]["module"]
                                    .as<std::string>());

                            /* Get module definition */
                            if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                                YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                                if (moduleData["port"] && moduleData["port"].IsMap()
                                    && moduleData["port"][portName.toStdString()]) {
                                    /* Get port width only if not already preserved from bus expansion */
                                    if (portWidthSpec.isEmpty()
                                        && moduleData["port"][portName.toStdString()]["type"]
                                        && moduleData["port"][portName.toStdString()]["type"]
                                               .IsScalar()) {
                                        const QString originalType = QString::fromStdString(
                                            moduleData["port"][portName.toStdString()]["type"]
                                                .as<std::string>());
                                        /* Keep original type for width calculation, but clean for display */
                                        portWidthSpec = originalType;
                                    }

                                    /* Get port direction */
                                    if (moduleData["port"][portName.toStdString()]["direction"]
                                        && moduleData["port"][portName.toStdString()]["direction"]
                                               .IsScalar()) {
                                        portDirection = QString::fromStdString(
                                            moduleData["port"][portName.toStdString()]["direction"]
                                                .as<std::string>());
                                        /* Handle both full and abbreviated forms */
                                        if (portDirection == "out" || portDirection == "output") {
                                            portDirection = "output";
                                        } else if (portDirection == "in" || portDirection == "input") {
                                            portDirection = "input";
                                        }
                                    }
                                }
                            }
                        }

                        /* Add to detailed port information, attaching the
                         * instance's macro guard cube so guard-disjoint
                         * drivers are exempted from multi-driver detection. */
                        const auto guardEntry = instanceGuards.value(instanceName);
                        portDetails.append(
                            PortDetailInfo::createModulePort(
                                instanceName,
                                portName,
                                portWidthSpec,
                                portDirection,
                                bitSelection,
                                guardEntry.first,
                                guardEntry.second));
                    }
                }

                /* Add comb/seq/fsm signals that affect this net */
                for (const PortDetailInfo &combSignal : combSeqFsmSignals) {
                    // Check if this comb/seq/fsm signal affects the current net
                    QString signalBaseName  = combSignal.portName;
                    QString signalBitSelect = combSignal.bitSelect;

                    // If signal has bit selection, it affects only part of the net
                    // If no bit selection, it affects the full net
                    bool affectsThisNet = false;

                    if (signalBaseName == netName) {
                        affectsThisNet = true;
                    } else if (connectedToTopPort && signalBaseName == connectedPortName) {
                        affectsThisNet = true;
                    }

                    if (affectsThisNet) {
                        // Add this comb/seq/fsm signal to the connection list
                        // Include bit selection in the signal name if it exists
                        QString fullSignalName = signalBaseName;
                        if (!signalBitSelect.isEmpty()) {
                            fullSignalName = signalBaseName + signalBitSelect;
                        }
                        portConnections.append(PortConnection::createCombSeqFsmPort(fullSignalName));

                        portDetails.append(
                            PortDetailInfo::createCombSeqFsmPort(
                                signalBaseName,
                                combSignal.width,
                                combSignal.direction, // Use actual direction from signal
                                signalBitSelect));
                    }
                }

                /* Check port width consistency */
                const bool hasWidthMismatch = !checkPortWidthConsistency(portConnections);
                if (hasWidthMismatch) {
                    QSocConsole::warn() << "Port width mismatch detected for net" << netName;
                }

                /* Check port direction consistency with bit-level overlap detection */
                const PortDirectionStatus dirStatus = checkPortDirectionConsistencyWithBitOverlap(
                    portDetails);
                const bool isUndriven   = (dirStatus == PortDirectionStatus::Undriven);
                const bool isMultidrive = (dirStatus == PortDirectionStatus::Multidrive);

                if (isUndriven) {
                    QSocConsole::warn()
                        << "Net" << netName << "has only input ports, missing driver";
                } else if (isMultidrive) {
                    QSocConsole::warn() << "Net" << netName << "has multiple output/inout ports";
                }

                const bool hasDuplicateConnection = duplicateConnectionNets.contains(netName);
                if (hasDuplicateConnection) {
                    out << "    /* FIXME: Net " << netName
                        << " has port-routing conflicts (duplicate within the net "
                        << "or the same instance.port wired to multiple nets); "
                        << "only the first connection is kept - check the source netlist */\n";
                }

                /* Generate combined warning comments for the net */
                if (hasWidthMismatch || isUndriven || isMultidrive) {
                    /* Output width mismatch warning if detected */
                    if (hasWidthMismatch) {
                        if (connectedToTopPort) {
                            out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                                << ") width mismatch - please check connected ports:\n";
                        } else {
                            out << "    /* FIXME: Net " << netName
                                << " width mismatch - please check connected ports:\n";
                        }

                        /* Add detailed information for each connected port */
                        for (const auto &detail : portDetails) {
                            /* Clean width information for display */
                            QString displayWidth
                                = detail.width.isEmpty()
                                      ? "default"
                                      : QSocGenerateManager::cleanTypeForWireDeclaration(
                                            detail.width);
                            if (displayWidth.isEmpty() && !detail.width.isEmpty()) {
                                displayWidth
                                    = "default"; /* fallback for single-bit types like "logic" */
                            }

                            if (detail.type == PortType::TopLevel) {
                                /* For top-level ports, display the actual direction used in checking */
                                QString displayDirection = detail.direction;

                                out << "     *   Top-Level Port: " << detail.portName
                                    << ", Direction: " << displayDirection
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else if (detail.type == PortType::CombSeqFsm) {
                                /* Comb/Seq/FSM output */
                                out << "     *   Comb/Seq/FSM Output: " << detail.portName
                                    << ", Direction: " << detail.direction
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else {
                                /* Regular instance port */
                                if (netlistData["instance"][detail.instanceName.toStdString()]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .IsScalar()) {
                                    out << "     *   Module: "
                                        << netlistData["instance"]
                                                      [detail.instanceName.toStdString()]["module"]
                                                          .as<std::string>()
                                                          .c_str()
                                        << ", Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                } else {
                                    /* Handle case where instance data might be invalid */
                                    out << "     *   Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                }
                            }
                        }
                        out << "     */\n";
                    }

                    /* Output undriven warning if detected */
                    if (isUndriven) {
                        if (connectedToTopPort) {
                            out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                                << ") is undriven - missing source:\n";
                        } else {
                            out << "    /* FIXME: Net " << netName
                                << " is undriven - missing source:\n";
                        }

                        /* Add detailed information for each connected port */
                        for (const auto &detail : portDetails) {
                            /* Clean width information for display */
                            QString displayWidth
                                = detail.width.isEmpty()
                                      ? "default"
                                      : QSocGenerateManager::cleanTypeForWireDeclaration(
                                            detail.width);
                            if (displayWidth.isEmpty() && !detail.width.isEmpty()) {
                                displayWidth
                                    = "default"; /* fallback for single-bit types like "logic" */
                            }

                            if (detail.type == PortType::TopLevel) {
                                /* For top-level ports, display the actual direction used in checking */
                                QString displayDirection = detail.direction;

                                out << "     *   Top-Level Port: " << detail.portName
                                    << ", Direction: " << displayDirection
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else if (detail.type == PortType::CombSeqFsm) {
                                /* Comb/Seq/FSM signal */
                                out << "     *   Comb/Seq/FSM Signal: " << detail.portName
                                    << ", Direction: " << detail.direction
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else {
                                /* Regular instance port */
                                if (netlistData["instance"][detail.instanceName.toStdString()]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .IsScalar()) {
                                    out << "     *   Module: "
                                        << netlistData["instance"]
                                                      [detail.instanceName.toStdString()]["module"]
                                                          .as<std::string>()
                                                          .c_str()
                                        << ", Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                } else {
                                    /* Handle case where instance data might be invalid */
                                    out << "     *   Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                }
                            }
                        }
                        out << "     */\n";
                    }

                    /* Output multidrive warning if detected */
                    if (isMultidrive) {
                        if (connectedToTopPort) {
                            out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                                << ") has multiple drivers - potential conflict:\n";
                        } else {
                            out << "    /* FIXME: Net " << netName
                                << " has multiple drivers - potential conflict:\n";
                        }

                        /* Add detailed information for each connected port */
                        for (const auto &detail : portDetails) {
                            /* Clean width information for display */
                            QString displayWidth
                                = detail.width.isEmpty()
                                      ? "default"
                                      : QSocGenerateManager::cleanTypeForWireDeclaration(
                                            detail.width);
                            if (displayWidth.isEmpty() && !detail.width.isEmpty()) {
                                displayWidth
                                    = "default"; /* fallback for single-bit types like "logic" */
                            }

                            if (detail.type == PortType::TopLevel) {
                                /* For top-level ports, display the actual direction used in checking */
                                QString displayDirection = detail.direction;

                                out << "     *   Top-Level Port: " << detail.portName
                                    << ", Direction: " << displayDirection
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else if (detail.type == PortType::CombSeqFsm) {
                                /* Comb/Seq/FSM output */
                                out << "     *   Comb/Seq/FSM Output: " << detail.portName
                                    << ", Direction: " << detail.direction
                                    << ", Width: " << displayWidth
                                    << (detail.bitSelect.isEmpty()
                                            ? ""
                                            : ", Bit Selection: " + detail.bitSelect)
                                    << "\n";
                            } else {
                                /* Regular instance port */
                                if (netlistData["instance"][detail.instanceName.toStdString()]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                    && netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .IsScalar()) {
                                    out << "     *   Module: "
                                        << netlistData["instance"]
                                                      [detail.instanceName.toStdString()]["module"]
                                                          .as<std::string>()
                                                          .c_str()
                                        << ", Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                } else {
                                    /* Handle case where instance data might be invalid */
                                    out << "     *   Instance: " << detail.instanceName
                                        << ", Port: " << detail.portName
                                        << ", Direction: " << detail.direction
                                        << ", Width: " << displayWidth
                                        << (detail.bitSelect.isEmpty()
                                                ? ""
                                                : ", Bit Selection: " + detail.bitSelect)
                                        << "\n";
                                }
                            }
                        }
                        out << "     */\n";
                    }
                }

                /* An instance output cannot use a top-level input as its
                   destination; keep an internal wire for that conflict. */
                if (!connectedToTopPort || forcedInternalWireNets.contains(netName)) {
                    /* Get net width from all ports connected to this net */
                    QString netWidth = "";
                    int     maxWidth = 0;

                    /* Compute the highest bit index any port detail places on this net.
                       For each port detail the requirement is the larger of:
                         - the port's native MSB (when the port drives the whole net), and
                         - the bit-select MSB (when the port lands on a slice of a wider net).
                       Net width is then [maxBitIndex:0]. This is order-independent and
                       handles concatenation patterns like two 4-bit drivers covering an
                       8-bit net via bits "[3:0]" and "[7:4]". */
                    int maxBitIndex = -1;

                    /* Track an explicit non-canonical range like [21:2] so cases that need
                       to preserve the LSB offset still work. Recorded only when its MSB
                       equals the final maxBitIndex; otherwise inferred [maxBitIndex:0] is
                       used so bit-selects can extend the net beyond any single port. */
                    QString preservedRangeType;
                    int     preservedRangeMsb = -1;

                    const QRegularExpression widthRegex(R"(\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])");

                    for (const auto &detail : portDetails) {
                        int requiredMaxBit = -1;

                        /* Port's native width contributes its MSB. */
                        if (!detail.width.isEmpty()) {
                            if (detail.width == "logic" || detail.width == "wire") {
                                requiredMaxBit = 0;
                            } else {
                                const QRegularExpressionMatch match = widthRegex.match(detail.width);
                                if (match.hasMatch()) {
                                    bool      msbOk = false;
                                    bool      lsbOk = false;
                                    const int msb   = match.captured(1).toInt(&msbOk);
                                    const int lsb   = match.captured(2).toInt(&lsbOk);
                                    if (msbOk) {
                                        requiredMaxBit = msb;
                                        /* A packed array like [1:0][3:0] needs the product of
                                           every dimension; the outer range alone would drop
                                           the inner bits from the wire. */
                                        const int packedBits
                                            = QSocGenerateManager::calculatePortWidth(
                                                detail.width.toStdString());
                                        if (packedBits > requiredMaxBit + 1) {
                                            requiredMaxBit     = packedBits - 1;
                                            preservedRangeType = QString();
                                            preservedRangeMsb  = -1;
                                        } else if (lsbOk && lsb > 0 && msb > preservedRangeMsb) {
                                            /* Remember non-canonical range like logic[21:2]. */
                                            preservedRangeType = detail.width;
                                            preservedRangeMsb  = msb;
                                        }
                                    }
                                }
                            }
                        }

                        /* Bit-select MSB places the port at a position on the net and may
                           extend the required net width beyond the port's native width.
                           Take the larger of the two. */
                        if (!detail.bitSelect.isEmpty()) {
                            const QRegularExpressionMatch bitMatch = widthRegex.match(
                                detail.bitSelect);
                            if (bitMatch.hasMatch()) {
                                bool      msbOk = false;
                                const int msb   = bitMatch.captured(1).toInt(&msbOk);
                                if (msbOk && msb > requiredMaxBit) {
                                    requiredMaxBit = msb;
                                }
                            }
                        }

                        if (requiredMaxBit > maxBitIndex) {
                            maxBitIndex = requiredMaxBit;
                        }
                    }

                    /* Pick the wire range. If a non-canonical preserved range fits exactly,
                       keep it; otherwise emit canonical [maxBitIndex:0]. */
                    if (maxBitIndex >= 0) {
                        if (!preservedRangeType.isEmpty() && preservedRangeMsb == maxBitIndex) {
                            netWidth = preservedRangeType;
                        } else if (maxBitIndex == 0) {
                            netWidth = "";
                            scalarNets.insert(netName);
                        } else {
                            netWidth = QString("[%1:0]").arg(maxBitIndex);
                        }
                        maxWidth = maxBitIndex + 1;
                    }

                    /* The schema requires `net.<name>` to be a sequence of
                       connection maps, so a sibling `type:` key on the net
                       is unreachable here. Width must come from port details
                       or the inferred bit-select MSB above. */
                    const QString cleanedNetWidth
                        = QSocGenerateManager::cleanTypeForWireDeclaration(netWidth);
                    /* Add wire declaration for this net with width information if available.
                       A net whose name matches a top-level port is already declared as
                       part of the module header; emitting another `wire` would duplicate
                       the identifier. */
                    if (topLevelPortNames.contains(netName)) {
                        QSocConsole::warn()
                            << "Net" << netName
                            << "shares a name with a top-level port; using the port "
                               "directly. Add 'connect:' to silence this warning";
                    } else {
                        declaredSignalRanges.insert(netName, cleanedNetWidth);
                        if (!cleanedNetWidth.isEmpty()) {
                            out << "    wire " << cleanedNetWidth << " " << netName << ";\n";
                        } else {
                            out << "    wire " << netName << ";\n";
                            scalarNets.insert(netName);
                        }
                    }
                }
            }
            out << "\n";

            /* Strip [0] / [0:0] selects on scalar wires; otherwise the
               instantiation would emit `scalar_wire[0]` which is illegal
               part-select on a non-vector net. */
            if (!scalarNets.isEmpty()) {
                for (auto instIt = instancePortConnections.begin();
                     instIt != instancePortConnections.end();
                     ++instIt) {
                    // cppcheck-suppress constVariableReference
                    QMap<QString, QString>         &portMap = instIt.value();
                    static const QRegularExpression scrubRegex(
                        R"(^(~?)([A-Za-z_][A-Za-z0-9_]*)\[\s*0\s*(?::\s*0\s*)?\]$)");
                    for (auto portIt = portMap.begin(); portIt != portMap.end(); ++portIt) {
                        const QRegularExpressionMatch match = scrubRegex.match(portIt.value());
                        if (match.hasMatch() && scalarNets.contains(match.captured(2))) {
                            portIt.value() = match.captured(1) + match.captured(2);
                        }
                    }
                }
            }
        }
    } else {
        QSocConsole::warn()
            << "No 'net' section in netlist, no wire declarations will be generated";
    }

    /* Add instances section comment */
    out << "    /* Module instantiations */\n";

    /* Generate instance declarations after wire declarations */
    if (netlistData["instance"] && netlistData["instance"].IsMap()) {
        /* yaml-cpp's iterator may visit duplicate keys without merging.
           Track emitted instance names so we never produce two `module
           inst (...)` blocks with the same identifier (illegal Verilog). */
        QSet<QString> emittedInstances;
        for (auto instanceIter = netlistData["instance"].begin();
             instanceIter != netlistData["instance"].end();
             ++instanceIter) {
            /* Check if the instance name is a scalar */
            if (!instanceIter->first.IsScalar()) {
                QSocConsole::warn() << "Invalid instance name, skipping";
                continue;
            }

            const QString instanceName = QString::fromStdString(
                instanceIter->first.as<std::string>());
            if (!QSocVerilogUtils::isValidVerilogIdentifier(instanceName)) {
                QSocConsole::warn() << "Instance name" << instanceName
                                    << "is not a valid Verilog identifier "
                                       "(reserved keyword or illegal character)";
            }
            if (emittedInstances.contains(instanceName)) {
                QSocConsole::warn() << "Duplicate instance name" << instanceName
                                    << "; only the first definition is emitted";
                continue;
            }
            emittedInstances.insert(instanceName);

            /* Check if the instance data is valid */
            if (!instanceIter->second || !instanceIter->second.IsMap()) {
                QSocConsole::warn()
                    << "Invalid instance data for" << instanceName << "(not a map), skipping";
                continue;
            }

            const YAML::Node instanceData = instanceIter->second;

            if (!instanceData["module"] || !instanceData["module"].IsScalar()) {
                QSocConsole::warn() << "Invalid module name for instance" << instanceName;
                continue;
            }

            const QString moduleName = QString::fromStdString(
                instanceData["module"].as<std::string>());

            /* Use the validated conditional compilation directives. */
            if (!emittableInstanceNames.contains(instanceName)) {
                continue;
            }
            const auto  guardEntry = instanceGuards.value(instanceName);
            QStringList ifdefList  = guardEntry.first;
            QStringList ifndefList = guardEntry.second;

            /* Write ifdef/ifndef begin directives if present */
            if (!ifdefList.isEmpty() || !ifndefList.isEmpty()) {
                writeIfdefBegin(out, ifdefList, ifndefList);
            }

            /* Generate instance declaration with parameters if any */
            out << "    " << moduleName << " ";

            /* Add parameters if they exist */
            if (instanceData["parameter"]) {
                /* Hoist the node so a yaml-cpp re-subscription on a scalar
                   value cannot abort the run with `BadSubscript`. Pre-fix any
                   instance with a `parameter:` section crashed the generator. */
                const YAML::Node parameterNode = instanceData["parameter"];
                if (!parameterNode.IsMap()) {
                    QSocConsole::warn() << "'parameter' section for instance" << instanceName
                                        << "is not a map, ignoring";
                } else if (parameterNode.size() == 0) {
                    QSocConsole::warn() << "'parameter' section for instance" << instanceName
                                        << "is empty, ignoring";
                } else {
                    out << "#(\n";

                    QStringList paramList;
                    try {
                        for (auto paramIter = parameterNode.begin();
                             paramIter != parameterNode.end();
                             ++paramIter) {
                            if (!paramIter->first.IsScalar()) {
                                QSocConsole::warn()
                                    << "Invalid parameter name in instance" << instanceName;
                                continue;
                            }

                            if (!paramIter->second.IsScalar()) {
                                QSocConsole::warn()
                                    << "Parameter"
                                    << QString::fromStdString(paramIter->first.as<std::string>())
                                    << "in instance" << instanceName
                                    << "has a non-scalar value, skipping";
                                continue;
                            }

                            const QString paramName = QString::fromStdString(
                                paramIter->first.as<std::string>());
                            if (!QSocVerilogUtils::isValidVerilogIdentifier(paramName)) {
                                QSocConsole::warn() << "Parameter name" << paramName
                                                    << "in instance" << instanceName
                                                    << "is not a valid Verilog identifier "
                                                       "(reserved keyword or illegal character)";
                            }
                            const QString paramValue = QString::fromStdString(
                                paramIter->second.as<std::string>());

                            paramList.append(
                                QString("        .%1(%2)").arg(paramName).arg(paramValue));
                        }
                    } catch (const YAML::Exception &e) {
                        QSocConsole::warn() << "Failed to read parameters for instance"
                                            << instanceName << ":" << e.what();
                    }

                    out << paramList.join(",\n") << "\n    ) ";
                }
            }

            out << instanceName << " (\n";

            /* Get the port connections for this instance */
            QStringList portConnections;

            /* Get module definition to ensure all ports are listed */
            if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                if (moduleData["port"] && moduleData["port"].IsMap()) {
                    /* Get the existing connections map for this instance */
                    QMap<QString, QString> portMap;
                    if (instancePortConnections.contains(instanceName)) {
                        portMap = instancePortConnections[instanceName];
                    }

                    /* Iterate through all ports in the module definition */
                    for (auto portIter = moduleData["port"].begin();
                         portIter != moduleData["port"].end();
                         ++portIter) {
                        if (!portIter->first.IsScalar()) {
                            QSocConsole::warn() << "Invalid port name in module" << moduleName;
                            continue;
                        }

                        const QString portName = QString::fromStdString(
                            portIter->first.as<std::string>());

                        /* Check if this port has a connection */
                        if (portMap.contains(portName)) {
                            QString wireConnection = portMap[portName];

                            /* `invert: true` on an output (or inout) port
                               would emit `.port(~wire)` - illegal because
                               you cannot invert an output destination, and
                               nonsensical because the value comes FROM the
                               instance. Strip the `~` and warn. */
                            if (wireConnection.startsWith('~') && portIter->second
                                && portIter->second["direction"]
                                && portIter->second["direction"].IsScalar()) {
                                const QString portDir
                                    = QString::fromStdString(
                                          portIter->second["direction"].as<std::string>())
                                          .toLower();
                                if (portDir == "out" || portDir == "output" || portDir == "inout") {
                                    QSocConsole::warn()
                                        << "'invert: true' on" << instanceName << "." << portName
                                        << "(direction:" << portDir
                                        << ") cannot be applied to an output destination; "
                                           "ignoring the invert";
                                    wireConnection.remove(0, 1);
                                }
                            }

                            portConnections.append(
                                QString("        .%1(%2)").arg(portName).arg(wireConnection));
                        } else {
                            /* Port exists in module but has no connection */
                            QString direction = "signal";
                            QString displayWidth;

                            if (portIter->second && portIter->second["direction"]
                                && portIter->second["direction"].IsScalar()) {
                                direction = QString::fromStdString(
                                    portIter->second["direction"].as<std::string>());
                            }

                            /* Get port width/type */
                            std::optional<int> provenPortWidth{1};
                            if (portIter->second && portIter->second["type"]
                                && portIter->second["type"].IsScalar()) {
                                const QString rawType = QString::fromStdString(
                                    portIter->second["type"].as<std::string>());
                                provenPortWidth = provenBuiltInWidth(rawType);

                                const QString cleanType
                                    = QSocGenerateManager::cleanTypeForWireDeclaration(rawType);
                                static const QRegularExpression widthRegex(
                                    R"(\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])");
                                const QRegularExpressionMatch match = widthRegex.match(cleanType);
                                if (match.hasMatch()) {
                                    displayWidth = match.captured(0);
                                }
                            }

                            /* An overridden parameter can change any recorded
                               width, so nothing recorded stays proven. */
                            if (instanceData["parameter"] && instanceData["parameter"].IsMap()
                                && instanceData["parameter"].size() > 0) {
                                provenPortWidth.reset();
                            }

                            /* Check for tie attribute in instance's port */
                            bool    hasTie = false;
                            QString tieValue;

                            /* Check if this port is already connected to any net in the design */
                            bool isConnectedToNet = false;
                            if (netlistData["net"] && netlistData["net"].IsMap()) {
                                for (auto netIter = netlistData["net"].begin();
                                     netIter != netlistData["net"].end() && !isConnectedToNet;
                                     ++netIter) {
                                    if (netIter->second.IsSequence()) {
                                        for (const auto &connectionNode : netIter->second) {
                                            if (connectionNode.IsMap() && connectionNode["instance"]
                                                && connectionNode["instance"].IsScalar()
                                                && connectionNode["port"]
                                                && connectionNode["port"].IsScalar()) {
                                                const QString connectedInstance
                                                    = QString::fromStdString(
                                                        connectionNode["instance"].as<std::string>());
                                                const QString connectedPort = QString::fromStdString(
                                                    connectionNode["port"].as<std::string>());

                                                if (connectedInstance == instanceName
                                                    && connectedPort == portName) {
                                                    isConnectedToNet = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            /* Only proceed with tie if port is not connected to a net */
                            if (!isConnectedToNet) {
                                /* Check if instance has tie attribute for this port */
                                if (netlistData["instance"]
                                    && netlistData["instance"][instanceName.toStdString()]
                                    && netlistData["instance"][instanceName.toStdString()]["port"]
                                    && netlistData["instance"][instanceName.toStdString()]["port"]
                                                  [portName.toStdString()]) {
                                    auto portNode
                                        = netlistData["instance"][instanceName.toStdString()]
                                                     ["port"][portName.toStdString()];

                                    /* Check for tie attribute (only if portNode is a map) */
                                    if (portNode.IsMap() && portNode["tie"]
                                        && portNode["tie"].IsScalar()
                                        && !QString::fromStdString(portNode["tie"].as<std::string>())
                                                .trimmed()
                                                .isEmpty()) {
                                        const QString tieStr = QString::fromStdString(
                                                                   portNode["tie"].as<std::string>())
                                                                   .trimmed();

                                        const TieClassification tieClassification = classifyTie(
                                            tieStr);
                                        if (tieClassification.kind
                                            == QSocNumberInfo::NumericTextKind::Reject) {
                                            /* The port keeps the standard
                                               unconnected handling, so it stays
                                               visible in the report. */
                                            QSocConsole::warn()
                                                << "'tie' on" << instanceName << "." << portName
                                                << "ignored: value" << tieDiagnosticText(tieStr)
                                                << "is not a valid number";
                                        } else if (
                                            tieClassification.kind
                                            == QSocNumberInfo::NumericTextKind::PassThrough) {
                                            if (direction.toLower() == "input"
                                                || direction.toLower() == "in") {
                                                hasTie   = true;
                                                tieValue = tieClassification.emittedText;
                                                if (portNode["invert"]
                                                    && portNode["invert"].IsScalar()
                                                    && portNode["invert"].as<bool>()) {
                                                    tieValue = QString("~(%1)").arg(tieValue);
                                                }
                                                portConnections.append(QString("        .%1(%2)")
                                                                           .arg(portName)
                                                                           .arg(tieValue));
                                                continue;
                                            }
                                            /* The port keeps the standard
                                               unconnected handling, so it stays
                                               visible in the report. */
                                            QSocConsole::warn()
                                                << "'tie' on" << instanceName << "." << portName
                                                << "ignored:" << direction.toLower()
                                                << "ports cannot be tied";
                                        } else {
                                            /* Parse the tie value using our number parser */
                                            const QSocNumberInfo numInfo
                                                = QSocNumberInfo::parseNumber(tieStr);

                                            /* Only apply tie to input ports */
                                            if (direction.toLower() == "input"
                                                || direction.toLower() == "in") {
                                                hasTie = true;

                                                /* An unproven port width would
                                                   mask the value with garbage;
                                                   keep the literal exactly as
                                                   written. */
                                                if (!provenPortWidth.has_value()) {
                                                    /* C spellings are not
                                                       Verilog; re-express them
                                                       with the value and base
                                                       kept, width left to the
                                                       context. */
                                                    if (tieClassification.spelling
                                                            == QSocNumberInfo::Spelling::CStyle
                                                        && !numInfo.errorDetected) {
                                                        tieValue = numInfo.formatVerilog();
                                                        tieValue.remove(
                                                            QRegularExpression(R"(^\d+)"));
                                                    } else {
                                                        tieValue = tieClassification.emittedText;
                                                    }
                                                    if (portNode["invert"]
                                                        && portNode["invert"].IsScalar()
                                                        && portNode["invert"].as<bool>()) {
                                                        tieValue = QString("~(%1)").arg(tieValue);
                                                    }
                                                    portConnections.append(QString("        .%1(%2)")
                                                                               .arg(portName)
                                                                               .arg(tieValue));
                                                    continue;
                                                }

                                                const int portWidth = provenPortWidth.value();

                                                /* Format the tie value */
                                                /* Create a copy of numInfo with adjusted width */
                                                QSocNumberInfo adjustedInfo = numInfo;

                                                /* Special handling for overflow detection */
                                                if (numInfo.errorDetected) {
                                                    /* For overflow values, keep the original string representation */
                                                    if (numInfo.width > portWidth) {
                                                        tieValue
                                                            = QString(
                                                                  "%1 /* FIXME: Value width %2 "
                                                                  "bits "
                                                                  "exceeds port width %3 bits */")
                                                                  .arg(numInfo.originalString)
                                                                  .arg(numInfo.width)
                                                                  .arg(portWidth);
                                                    } else {
                                                        tieValue = numInfo.originalString;
                                                    }
                                                } else {
                                                    /* A literal is bounded by its
                                                       declared width before any
                                                       port adaptation: 4'h1F is
                                                       4'hF. */
                                                    if (numInfo.hasExplicitWidth
                                                        && numInfo.width > 0) {
                                                        const BigUnsigned declaredMask
                                                            = (BigUnsigned(1) << numInfo.width)
                                                              - BigUnsigned(1);
                                                        if (adjustedInfo.value.getSign()
                                                            == BigInteger::negative) {
                                                            adjustedInfo.value = BigInteger(
                                                                adjustedInfo.value.getMagnitude()
                                                                    & declaredMask,
                                                                BigInteger::negative);
                                                        } else {
                                                            adjustedInfo.value = BigInteger(
                                                                adjustedInfo.value.getMagnitude()
                                                                & declaredMask);
                                                        }
                                                    }

                                                    /* Normal handling for regular values */
                                                    adjustedInfo.width            = portWidth;
                                                    adjustedInfo.hasExplicitWidth = true;

                                                    /* Create a mask for the width */
                                                    BigUnsigned mask = BigUnsigned(0);
                                                    if (portWidth > 0) {
                                                        mask = (BigUnsigned(1) << portWidth)
                                                               - BigUnsigned(1);
                                                    }
                                                    /* Apply mask to truncate the value */
                                                    if (adjustedInfo.value.getSign()
                                                        == BigInteger::negative) {
                                                        /* For negative numbers, apply mask to magnitude and maintain sign */
                                                        const BigUnsigned result
                                                            = adjustedInfo.value.getMagnitude()
                                                              & mask;
                                                        adjustedInfo.value = BigInteger(
                                                            result, BigInteger::negative);
                                                    } else {
                                                        /* For non-negative numbers, just apply the mask */
                                                        adjustedInfo.value = BigInteger(
                                                            adjustedInfo.value.getMagnitude()
                                                            & mask);
                                                    }

                                                    if (numInfo.width > portWidth) {
                                                        /* Value is wider than port - show FIXME comment but use proper width */
                                                        tieValue
                                                            = QString(
                                                                  "%1 /* FIXME: Value %2 wider "
                                                                  "than "
                                                                  "port width %3 bits */")
                                                                  .arg(adjustedInfo.formatVerilog())
                                                                  .arg(numInfo.formatVerilog())
                                                                  .arg(portWidth);
                                                    } else {
                                                        /* Use adjusted formatting with correct width, preserving original base */
                                                        tieValue = adjustedInfo.formatVerilog();
                                                    }
                                                }

                                                /* Check for invert attribute */
                                                if (portNode.IsMap() && portNode["invert"]
                                                    && portNode["invert"].IsScalar()) {
                                                    /* Use direct YAML boolean parsing instead of string conversion */
                                                    if (portNode["invert"].as<bool>()) {
                                                        /* If we need to invert, apply logical NOT (~) to the value */
                                                        tieValue = QString("~(%1)").arg(tieValue);
                                                    }
                                                }
                                            } else {
                                                /* Add warning for non-input ports with tie */
                                                tieValue = QString(
                                                               "/* FIXME: 'tie' attribute for %1 "
                                                               "port %2 "
                                                               "ignored */")
                                                               .arg(direction.toLower())
                                                               .arg(portName);
                                            }
                                        }
                                    }
                                    /* If no tie but has invert attribute on an input port, warn about missing tie */
                                    else if (
                                        portNode.IsMap() && portNode["invert"]
                                        && portNode["invert"].IsScalar()
                                        && (direction.toLower() == "input"
                                            || direction.toLower() == "in")) {
                                        tieValue = QString(
                                                       "/* FIXME: 'invert' attribute on %1 port %2 "
                                                       "without 'tie' attribute */")
                                                       .arg(direction.toLower())
                                                       .arg(portName);
                                    }
                                }
                            }

                            /* Format port connection based on connection status and tie attribute */
                            if (hasTie
                                && (direction.toLower() == "input" || direction.toLower() == "in")) {
                                portConnections.append(
                                    QString("        .%1(%2)").arg(portName).arg(tieValue));
                            } else if (
                                isConnectedToNet && instancePortConnections.contains(instanceName)
                                && instancePortConnections[instanceName].contains(portName)) {
                                /* Use connection from instancePortConnections if port is connected to a net */
                                const QString connectionValue
                                    = instancePortConnections[instanceName][portName];
                                portConnections.append(
                                    QString("        .%1(%2)").arg(portName).arg(connectionValue));
                            } else {
                                /* Collect unconnected port information for reporting */
                                QSocGenerateReportUnconnected::UnconnectedPortInfo portInfo;
                                portInfo.instanceName = instanceName;
                                portInfo.moduleName   = moduleName;
                                portInfo.portName     = portName;
                                portInfo.direction    = direction;

                                /* Clean the type for reporting - combine width and base type */
                                if (displayWidth.isEmpty()) {
                                    portInfo.type = "logic";
                                } else {
                                    portInfo.type = QString("logic%1").arg(displayWidth);
                                }

                                unconnectedPortReporter.addUnconnectedPort(portInfo);

                                /* Format FIXME message with width if available */
                                if (displayWidth.isEmpty()) {
                                    portConnections.append(
                                        QString("        .%1(/* FIXME: %2 %3 missing */)")
                                            .arg(portName)
                                            .arg(direction)
                                            .arg(portName));
                                } else {
                                    portConnections.append(
                                        QString("        .%1(/* FIXME: %2 %3 %4 missing */)")
                                            .arg(portName)
                                            .arg(direction)
                                            .arg(displayWidth)
                                            .arg(portName));
                                }
                            }
                        }
                    }
                } else {
                    QSocConsole::warn() << "Module" << moduleName << "has no valid port section";
                }
            } else {
                QSocConsole::warn() << "Failed to get module definition for" << moduleName;

                /* Fall back to existing connections if module definition not available */
                if (instancePortConnections.contains(instanceName)) {
                    const QMap<QString, QString>  &portMap = instancePortConnections[instanceName];
                    QMapIterator<QString, QString> portIter(portMap);
                    while (portIter.hasNext()) {
                        portIter.next();
                        portConnections.append(
                            QString("        .%1(%2)").arg(portIter.key()).arg(portIter.value()));
                    }
                }
            }

            if (portConnections.isEmpty()) {
                /* No port connections found for this instance */
                out << "        /* No port connections found for this instance */\n";
            } else {
                out << portConnections.join(",\n") << "\n";
            }

            out << "    );\n";

            /* Write endif directives if conditional compilation was used */
            if (!ifdefList.isEmpty() || !ifndefList.isEmpty()) {
                writeIfdefEnd(out, ifdefList, ifndefList);
            }
        }
    }

    /* Generate combinational logic after module instantiations */
    if (!generateCombPrimitive(netlistData, declaredSignalRanges, out)) {
        QSocConsole::warn() << "Failed to generate combinational logic primitives";
        return false;
    }

    /* Generate sequential logic after combinational logic */
    if (!generateSeqPrimitive(netlistData, declaredSignalRanges, out)) {
        QSocConsole::warn() << "Failed to generate sequential logic primitives";
        return false;
    }

    /* NOTE: FSM, reset, and clock primitive modules are now generated
     * at file level before the top-level module. No inline generation needed. */

    /* When several top-level ports share one internal net, route drivers by
       direction and fan out compatible output sinks from the selected source.
       Ambiguous ownership is diagnosed without inventing a connection. */
    const auto topPortDirection = [&](const QString &portName) {
        return QSocGenerateManager::topPortDirection(netlistData, portName);
    };

    /* Ranges that comb, seq, or an instance already drive, per resolved port.
       An empty range means
       the whole port. Conflicts are judged on bit ranges: a driver on one
       slice does not collide with an alias assignment onto a disjoint one. */
    QMap<QString, QStringList> drivenRangesByPort;
    QSet<QString>              processDrivenNets;
    QSet<QString>              instanceDrivenNets;
    QSet<QString>              inoutConnectionNets;
    QSet<QString>              unknownOwnershipNets;
    const auto                 rangesOverlap = [](const QString &lhs, const QString &rhs) {
        return lhs.isEmpty() || rhs.isEmpty() || QSocGenerateManager::doBitRangesOverlap(lhs, rhs);
    };
    const auto collidesWithDriver = [&drivenRangesByPort,
                                     &rangesOverlap](const QString &port, const QString &slice) {
        for (const QString &driven : drivenRangesByPort.value(port)) {
            if (rangesOverlap(driven, slice)) {
                return true;
            }
        }
        return false;
    };
    {
        const QMap<QString, QSocGenerateManager::TopPortBinding> redirect
            = QSocGenerateManager::buildTopPortRedirect(netlistData);
        const auto collect = [&](const char *section, const char *key, bool combSection) {
            if (!netlistData[section] || !netlistData[section].IsSequence()) {
                return;
            }
            for (const auto &item : netlistData[section]) {
                if (!item.IsMap() || !item[key] || !item[key].IsScalar()) {
                    continue;
                }
                if (combSection) {
                    const bool hasExpression = item["expr"] && item["expr"].IsScalar();
                    const bool hasIf         = item["if"] && item["if"].IsSequence();
                    const bool hasCase = item["case"] && item["case"].IsScalar() && item["cases"]
                                         && item["cases"].IsMap();
                    if (!hasExpression && !hasIf && !hasCase) {
                        continue;
                    }
                }
                const QString name   = QString::fromStdString(item[key].as<std::string>());
                const auto    parsed = parseSignalBitSelect(name);
                const QSocGenerateManager::TopPortBinding target
                    = redirect.value(parsed.first, {parsed.first, QString()});
                if (netlistData["port"] && netlistData["port"][target.port.toStdString()]
                    && netlistData["port"][target.port.toStdString()]["direction"]
                    && netlistData["port"][target.port.toStdString()]["direction"].IsScalar()) {
                    const QString direction
                        = QString::fromStdString(
                              netlistData["port"][target.port.toStdString()]["direction"]
                                  .as<std::string>())
                              .toLower();
                    if (direction == QStringLiteral("in") || direction == QStringLiteral("input")) {
                        continue;
                    }
                }
                QString ownSlice = parsed.second;
                if (combSection && item["bits"] && item["bits"].IsScalar()) {
                    ownSlice = QSocVerilogUtils::normalizeBitSelect(
                        QString::fromStdString(item["bits"].as<std::string>()));
                }
                /* The inner select is relative to the bound slice; record the
                   composed port range or the collision check compares
                   mismatched coordinate systems. */
                const QString range = QSocGenerateManager::composeBitSelect(
                    target.slice, ownSlice, QStringLiteral("driver record ") + parsed.first);
                drivenRangesByPort[target.port].append(range);
                if (netToTopPortAliases.contains(parsed.first)) {
                    processDrivenNets.insert(parsed.first);
                }
            }
        };
        collect("comb", "out", true);
        collect("seq", "reg", false);

        /* An instance output reaching a member through a net that carries the
           member's name drives it just as a process would. */
        if (netlistData["net"] && netlistData["net"].IsMap()) {
            for (const auto &netEntry : netlistData["net"]) {
                if (!netEntry.first.IsScalar() || !netEntry.second.IsSequence()) {
                    continue;
                }
                const QString netName = QString::fromStdString(netEntry.first.as<std::string>());
                if (!topLevelPortNames.contains(netName) && !netToTopPortAliases.contains(netName)) {
                    continue;
                }
                for (const auto &connectionNode : netEntry.second) {
                    if (!connectionNode.IsMap() || !connectionNode["instance"]
                        || !connectionNode["instance"].IsScalar()) {
                        continue;
                    }
                    const QString instanceName = QString::fromStdString(
                        connectionNode["instance"].as<std::string>());
                    if (instanceName == "top") {
                        continue;
                    }
                    /* Only a driving instance port collides with the alias
                       assignment; an instance input just reads the net. */
                    if (!connectionNode["port"] || !connectionNode["port"].IsScalar()) {
                        continue;
                    }
                    const QString portName = QString::fromStdString(
                        connectionNode["port"].as<std::string>());
                    const InstancePortRole role = instancePortRole(instanceName, portName);
                    if (role == InstancePortRole::Input || role == InstancePortRole::Dropped) {
                        continue;
                    }
                    if (role == InstancePortRole::Unowned) {
                        unknownOwnershipNets.insert(netName);
                        continue;
                    }
                    if (role == InstancePortRole::Inout) {
                        instanceDrivenNets.insert(netName);
                        inoutConnectionNets.insert(netName);
                        continue;
                    }
                    instanceDrivenNets.insert(netName);
                    QString connection = instancePortConnections.value(instanceName).value(portName);
                    if (connection.startsWith('~')) {
                        connection.remove(0, 1);
                    }
                    const auto target = parseSignalBitSelect(connection);
                    if (!target.first.isEmpty()) {
                        drivenRangesByPort[target.first].append(target.second);
                    }
                }
            }
        }
    }

    bool       aliasHeaderEmitted = false;
    const auto emitAliasHeader    = [&]() {
        if (!aliasHeaderEmitted) {
            out << "\n    /* Top-level port aliases (multiple ports share one net) */\n";
            aliasHeaderEmitted = true;
        }
    };
    /* Statically known bit widths for the alias mismatch diagnostic;
       -1 when the width cannot be proven from the netlist alone. */
    const auto sliceWidthOf = [](const QString &slice) -> int {
        static const QRegularExpression rangeRegex(R"(^\[(\d+)(?::(\d+))?\]$)");
        const QRegularExpressionMatch   match = rangeRegex.match(slice);
        if (!match.hasMatch()) {
            return -1;
        }
        if (match.capturedLength(2) == 0) {
            return 1;
        }
        const int msb = match.captured(1).toInt();
        const int lsb = match.captured(2).toInt();
        return (msb >= lsb ? msb - lsb : lsb - msb) + 1;
    };
    const auto boundWidthOf = [&](const QString &portName, const QString &slice) -> int {
        if (!slice.isEmpty()) {
            return sliceWidthOf(slice);
        }
        if (!netlistData["port"] || !netlistData["port"][portName.toStdString()]) {
            return -1;
        }
        const auto portNode = netlistData["port"][portName.toStdString()];
        if (!portNode["type"] || !portNode["type"].IsScalar()) {
            return -1;
        }
        const QString typeStr = QString::fromStdString(portNode["type"].as<std::string>());
        static const QRegularExpression typeRangeRegex(
            R"(^\s*(?:logic|wire|reg|bit)(?:\s+(?:signed|unsigned))?\s*(\[(\d+):(\d+)\])?\s*$)");
        const QRegularExpressionMatch match = typeRangeRegex.match(typeStr);
        if (!match.hasMatch()) {
            return -1;
        }
        if (match.capturedLength(1) == 0) {
            return 1;
        }
        const int msb = match.captured(2).toInt();
        const int lsb = match.captured(3).toInt();
        return (msb >= lsb ? msb - lsb : lsb - msb) + 1;
    };
    const auto warnAliasWidthMismatch = [&](const QString &sinkPort,
                                            const QString &sinkSlice,
                                            const QString &srcPort,
                                            const QString &srcSlice) {
        const int sinkWidth = boundWidthOf(sinkPort, sinkSlice);
        const int srcWidth  = boundWidthOf(srcPort, srcSlice);
        if (sinkWidth > 0 && srcWidth > 0 && sinkWidth != srcWidth) {
            emitAliasHeader();
            out << "    /* FIXME: " << sinkPort << sinkSlice << " is " << sinkWidth
                << " bits but its source " << srcPort << srcSlice << " is " << srcWidth
                << " bits - width mismatch */\n";
            QSocConsole::warn() << sinkPort + sinkSlice << "is" << sinkWidth
                                << "bits but its source" << srcPort + srcSlice << "is" << srcWidth
                                << "bits; width mismatch";
        }
    };
    const auto emitAssign = [&](const QString &lhsPort,
                                const QString &lhsSlice,
                                const QString &rhsPort,
                                const QString &rhsSlice) {
        emitAliasHeader();
        warnAliasWidthMismatch(lhsPort, lhsSlice, rhsPort, rhsSlice);
        if (collidesWithDriver(lhsPort, lhsSlice)) {
            QSocConsole::warn() << "alias and another driver both reach" << lhsPort
                                << "- multi-driver conflict in synth";
            out << "    /* FIXME: another driver also reaches " << lhsPort
                << " - multi-driver conflict */\n";
        }
        out << "    assign " << lhsPort << lhsSlice << " = " << rhsPort << rhsSlice << ";\n";
    };

    /* Every spelling of one connected component names one signal, so its
       endpoints are judged together: whole members and slice bindings of
       every net name in the component form one list, direction fixes each
       endpoint's role, and exactly one source may drive the sinks. */
    struct NetEndpoint
    {
        QString port;
        QString slice; /* empty for a whole binding */
        QString direction;
    };
    struct NetComponent
    {
        QStringList        netNames;
        QStringList        members;
        QList<NetEndpoint> endpoints;
    };
    QMap<QString, NetComponent> componentsByKey;
    QStringList                 componentOrder;
    for (auto aliasIt = netToTopPortAliases.constBegin(); aliasIt != netToTopPortAliases.constEnd();
         ++aliasIt) {
        const NetTopPorts &entry = aliasIt.value();
        if (entry.members.isEmpty() && entry.slices.isEmpty()) {
            continue;
        }
        const QString key = entry.members.isEmpty()
                                ? QStringLiteral("net:") + aliasIt.key()
                                : QStringLiteral("grp:") + entry.members.first();
        if (!componentsByKey.contains(key)) {
            componentOrder.append(key);
            NetComponent component;
            component.members = entry.members;
            for (const QString &member : entry.members) {
                component.endpoints.append({member, QString(), topPortDirection(member)});
            }
            componentsByKey.insert(key, component);
        }
        NetComponent &component = componentsByKey[key];
        component.netNames.append(aliasIt.key());
        for (const auto &bound : entry.slices) {
            component.endpoints.append({bound.port, bound.slice, bound.direction});
        }
    }

    for (const QString &componentKey : componentOrder) {
        const NetComponent &component = componentsByKey.value(componentKey);
        const QString      &netLabel  = component.netNames.first();

        QList<NetEndpoint> sinks;
        QList<NetEndpoint> sources;
        bool               unowned                  = false;
        bool               unownedDiagnosticEmitted = false;
        for (const auto &endpoint : component.endpoints) {
            if (endpoint.direction == QStringLiteral("output")) {
                sinks.append(endpoint);
            } else if (endpoint.direction == QStringLiteral("input")) {
                sources.append(endpoint);
            } else {
                unowned = true;
                if (component.endpoints.size() >= 2) {
                    /* A lone unowned endpoint needs no wiring decision; only a
                       component that actually fans out is worth a diagnostic. */
                    emitAliasHeader();
                    out << "    /* FIXME: net " << netLabel << " binds " << endpoint.port
                        << endpoint.slice << " whose direction gives no ownership - not wired */\n";
                    QSocConsole::warn()
                        << "Net" << netLabel << "binds" << endpoint.port << endpoint.slice
                        << "whose direction gives no ownership; not wired";
                    unownedDiagnosticEmitted = true;
                }
            }
        }
        bool hasUnownedConnection = false;
        for (const QString &netName : component.netNames) {
            if (component.endpoints.size() >= 2
                && (unknownOwnershipNets.contains(netName)
                    || inoutConnectionNets.contains(netName))) {
                hasUnownedConnection = true;
                break;
            }
        }
        if (hasUnownedConnection) {
            unowned = true;
            if (!unownedDiagnosticEmitted) {
                emitAliasHeader();
                out << "    /* FIXME: net " << netLabel
                    << " has a connection whose direction gives no ownership - not wired */\n";
                QSocConsole::warn() << "Net" << netLabel
                                    << "has a connection whose direction gives no ownership; "
                                       "not wired";
            }
        }
        if (unowned) {
            continue;
        }

        /* Only a real driver counts: a process or instance output reaching
           any spelling or endpoint port. Membership alone proves nothing. */
        static const QRegularExpression numericRange(R"(^\[\s*(\d+)\s*(?::\s*(\d+)\s*)?\]$)");
        const auto numericRangeContains = [&](const QString &outer, const QString &inner) {
            const QRegularExpressionMatch outerMatch = numericRange.match(outer);
            const QRegularExpressionMatch innerMatch = numericRange.match(inner);
            if (!outerMatch.hasMatch() || !innerMatch.hasMatch()) {
                return false;
            }
            bool      outerFirstOk  = false;
            bool      outerSecondOk = false;
            bool      innerFirstOk  = false;
            bool      innerSecondOk = false;
            const int outerFirst    = outerMatch.captured(1).toInt(&outerFirstOk);
            const int outerSecond   = outerMatch.capturedLength(2) > 0
                                          ? outerMatch.captured(2).toInt(&outerSecondOk)
                                          : outerFirst;
            const int innerFirst    = innerMatch.captured(1).toInt(&innerFirstOk);
            const int innerSecond   = innerMatch.capturedLength(2) > 0
                                          ? innerMatch.captured(2).toInt(&innerSecondOk)
                                          : innerFirst;
            outerSecondOk = outerMatch.capturedLength(2) == 0 ? outerFirstOk : outerSecondOk;
            innerSecondOk = innerMatch.capturedLength(2) == 0 ? innerFirstOk : innerSecondOk;
            if (!outerFirstOk || !outerSecondOk || !innerFirstOk || !innerSecondOk) {
                return false;
            }
            const int outerLo = qMin(outerFirst, outerSecond);
            const int outerHi = qMax(outerFirst, outerSecond);
            const int innerLo = qMin(innerFirst, innerSecond);
            const int innerHi = qMax(innerFirst, innerSecond);
            return outerLo <= innerLo && outerHi >= innerHi;
        };
        const auto endpointHasDriver = [&](const NetEndpoint &endpoint) {
            for (const QString &driverRange : drivenRangesByPort.value(endpoint.port)) {
                const bool bothNumeric = numericRange.match(driverRange).hasMatch()
                                         && numericRange.match(endpoint.slice).hasMatch();
                if (driverRange.isEmpty() || endpoint.slice.isEmpty() || !bothNumeric
                    || QSocGenerateManager::doBitRangesOverlap(driverRange, endpoint.slice)) {
                    return true;
                }
            }
            return false;
        };
        const auto endpointCarriesDriver = [&](const NetEndpoint &endpoint) {
            for (const QString &driverRange : drivenRangesByPort.value(endpoint.port)) {
                if (driverRange.isEmpty()
                    && (endpoint.slice.isEmpty() || numericRange.match(endpoint.slice).hasMatch())) {
                    return true;
                }
                if (!endpoint.slice.isEmpty() && numericRangeContains(driverRange, endpoint.slice)) {
                    return true;
                }
            }
            return false;
        };
        bool netDriven = false;
        for (const auto &endpoint : component.endpoints) {
            if (endpointHasDriver(endpoint)) {
                netDriven = true;
                break;
            }
        }
        if (sinks.isEmpty()) {
            for (const QString &netName : component.netNames) {
                if (processDrivenNets.contains(netName) || instanceDrivenNets.contains(netName)) {
                    netDriven = true;
                    break;
                }
            }
        }

        if (sinks.isEmpty() && sources.isEmpty()) {
            continue;
        }

        if (netDriven) {
            /* The net's own driver reaches the head sink through routing;
               the remaining sinks follow it, and every bound input is a
               second source. */
            QList<int> carrierIndices;
            for (int sinkIdx = 0; sinkIdx < sinks.size(); ++sinkIdx) {
                if (endpointCarriesDriver(sinks.at(sinkIdx))) {
                    carrierIndices.append(sinkIdx);
                }
            }
            if (carrierIndices.size() > 1) {
                emitAliasHeader();
                out << "    /* FIXME: net " << netLabel << " has multiple driven sinks ("
                    << carrierIndices.size() << ") - not wired */\n";
                QSocConsole::warn() << "Net" << netLabel << "has multiple driven sinks"
                                    << carrierIndices.size() << "; not wired";
            } else {
                const int carrierIndex = carrierIndices.isEmpty() ? 0 : carrierIndices.first();
                for (int sinkIdx = 0; sinkIdx < sinks.size(); ++sinkIdx) {
                    if (sinkIdx == carrierIndex) {
                        continue;
                    }
                    const auto &bound = sinks.at(sinkIdx);
                    emitAssign(
                        bound.port,
                        bound.slice,
                        sinks.at(carrierIndex).port,
                        sinks.at(carrierIndex).slice);
                }
            }
            for (const auto &bound : sources) {
                emitAliasHeader();
                out << "    /* FIXME: net " << netLabel << " is already driven but binds "
                    << bound.port << bound.slice << " - multi-driver conflict */\n";
                QSocConsole::warn() << "Net" << netLabel << "is already driven but binds"
                                    << bound.port << bound.slice << "- multi-driver conflict";
            }
        } else if (sources.size() == 1) {
            for (const auto &bound : sinks) {
                emitAssign(bound.port, bound.slice, sources.first().port, sources.first().slice);
            }
        } else if (sources.size() >= 2) {
            emitAliasHeader();
            out << "    /* FIXME: net " << netLabel << " binds " << sources.size()
                << " sources - ambiguous driver, not wired */\n";
            QSocConsole::warn() << "Net" << netLabel << "binds" << sources.size()
                                << "sources; ambiguous driver, not wired";
        } else {
            /* No source at all: keep the sink chain so the head's undriven
               state stays visible to the existing diagnostics. */
            for (int sinkIdx = 1; sinkIdx < sinks.size(); ++sinkIdx) {
                const auto &bound = sinks.at(sinkIdx);
                emitAssign(bound.port, bound.slice, sinks.first().port, sinks.first().slice);
            }
        }
    }

    /* Close module */
    out << "\nendmodule\n";

    outputFile.close();
    QSocConsole::info() << "Successfully generated Verilog file:" << outputFilePath;

    /* Generate unconnected port report if we have unconnected ports */
    if (unconnectedPortReporter.getUnconnectedPortCount() > 0) {
        const QString reportOutputPath = projectManager->getOutputPath();
        if (unconnectedPortReporter.generateReport(reportOutputPath, outputFileName)) {
            QSocConsole::info() << "Successfully generated unconnected port report:"
                                << QDir(reportOutputPath).filePath(outputFileName + ".nc.rpt");
        } else {
            QSocConsole::warn() << "Failed to generate unconnected port report";
        }
    }

    return !primitiveFailed;
}

bool QSocGenerateManager::formatVerilogFile(const QString &filePath)
{
    const QString formatterPath = QStandardPaths::findExecutable("verible-verilog-format");
    if (formatterPath.isEmpty()) {
        QSocConsole::warn() << "Verilog formatter not found.";
        return false;
    }

    const QFileInfo fileInfo(filePath);
    const auto      artifactPath
        = QSocPaths::resolveArtifactPath(fileInfo.absolutePath(), fileInfo.fileName());
    if (!artifactPath.isValid()) {
        QSocConsole::warn() << artifactPath.error;
        return false;
    }

    QSocConsole::info() << "Formatting Verilog file...";

    QProcess formatter;
    /* clang-format off */
    const QString argsStr = QStaticStringWeaver::stripCommonLeadingWhitespace(R"(
        --inplace
        --column_limit 119
        --indentation_spaces 4
        --line_break_penalty 4
        --wrap_spaces 4
        --port_declarations_alignment align
        --port_declarations_indentation indent
        --formal_parameters_alignment align
        --formal_parameters_indentation indent
        --assignment_statement_alignment align
        --enum_assignment_statement_alignment align
        --class_member_variable_alignment align
        --module_net_variable_alignment align
        --named_parameter_alignment align
        --named_parameter_indentation indent
        --named_port_alignment align
        --named_port_indentation indent
        --struct_union_members_alignment align
    )");
    /* clang-format on */

    QStringList args = argsStr.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    args << artifactPath.path;

    formatter.start(formatterPath, args);
    if (!formatter.waitForStarted()) {
        QSocConsole::warn() << "failed to start Verilog formatter:" << formatter.errorString();
        return false;
    }
    if (!formatter.waitForFinished()) {
        formatter.kill();
        formatter.waitForFinished();
        QSocConsole::warn() << "Verilog formatter timed out.";
        return false;
    }

    if (formatter.exitStatus() == QProcess::NormalExit && formatter.exitCode() == 0) {
        QSocConsole::info() << "Successfully formatted Verilog file";
        return true;
    }
    if (formatter.exitStatus() != QProcess::NormalExit) {
        QSocConsole::warn() << "Verilog formatter terminated abnormally:"
                            << formatter.errorString();
        return false;
    }
    const QString standardError = QString::fromUtf8(formatter.readAllStandardError()).trimmed();
    QSocConsole::warn() << "Verilog formatter failed with exit code" << formatter.exitCode()
                        << (standardError.isEmpty() ? QString() : ": " + standardError);
    return false;
}

bool QSocGenerateManager::generateCombPrimitive(
    const YAML::Node             &netlistData,
    const QMap<QString, QString> &declaredSignalRanges,
    QTextStream                  &out)
{
    if (!combPrimitive) {
        QSocConsole::warn() << "Comb primitive generator not initialized";
        return false;
    }

    return combPrimitive->generateCombLogic(netlistData, declaredSignalRanges, out);
}

/**
 * @brief Generate FSM Verilog code for a single FSM
 * @param fsmItem The YAML node containing the FSM specification
 * @param out Output text stream
 */
bool QSocGenerateManager::generateFSMPrimitive(const YAML::Node &fsmNode, QTextStream &out)
{
    if (!fsmPrimitive) {
        QSocConsole::warn() << "FSM primitive generator not initialized";
        return false;
    }

    return fsmPrimitive->generateFSMVerilog(fsmNode, out);
}

bool QSocGenerateManager::generateResetPrimitive(const YAML::Node &resetNode, QTextStream &out)
{
    if (!resetPrimitive) {
        QSocConsole::warn() << "Reset primitive generator not initialized";
        return false;
    }

    return resetPrimitive->generateResetController(resetNode, out);
}

bool QSocGenerateManager::generateClockPrimitive(const YAML::Node &clockNode, QTextStream &out)
{
    if (!clockPrimitive) {
        QSocConsole::warn() << "Clock primitive generator not initialized";
        return false;
    }

    return clockPrimitive->generateClockController(clockNode, out);
}

bool QSocGenerateManager::generatePowerPrimitive(const YAML::Node &powerNode, QTextStream &out)
{
    if (!powerPrimitive) {
        QSocConsole::warn() << "Power primitive generator not initialized";
        return false;
    }

    return powerPrimitive->generatePowerController(powerNode, out);
}

bool QSocGenerateManager::generateSeqPrimitive(
    const YAML::Node             &netlistData,
    const QMap<QString, QString> &declaredSignalRanges,
    QTextStream                  &out)
{
    if (!seqPrimitive) {
        QSocConsole::warn() << "Seq primitive generator not initialized";
        return false;
    }

    return seqPrimitive->generateSeqLogicWithRanges(netlistData, declaredSignalRanges, out);
}

bool QSocGenerateManager::parseMacroCondition(
    const YAML::Node &instanceData,
    const QString    &instanceName,
    QStringList      &outIfdef,
    QStringList      &outIfndef)
{
    outIfdef.clear();
    outIfndef.clear();

    /* Verilog identifier regex: start with letter/underscore, then alphanumeric/underscore */
    static const QRegularExpression idRegex("^[a-zA-Z_][a-zA-Z0-9_]*$");

    /* Parse ifdef list */
    if (instanceData["ifdef"]) {
        if (!instanceData["ifdef"].IsSequence()) {
            QSocConsole::warn() << "'ifdef' field for instance" << instanceName
                                << "is not a list, ignoring";
        } else {
            for (const auto &node : instanceData["ifdef"]) {
                if (!node.IsScalar()) {
                    QSocConsole::warn() << "Non-scalar macro in 'ifdef' for instance"
                                        << instanceName << ", skipping";
                    continue;
                }
                QString macro = QString::fromStdString(node.as<std::string>()).trimmed();
                if (macro.isEmpty()) {
                    QSocConsole::warn()
                        << "Empty macro in 'ifdef' for instance" << instanceName << ", skipping";
                    continue;
                }
                if (!idRegex.match(macro).hasMatch()) {
                    QSocConsole::warn()
                        << "Invalid macro identifier" << macro << "in 'ifdef' for instance"
                        << instanceName << ", skipping";
                    continue;
                }
                if (!outIfdef.contains(macro)) {
                    outIfdef.append(macro);
                }
            }
        }
    }

    /* Parse ifndef list */
    if (instanceData["ifndef"]) {
        if (!instanceData["ifndef"].IsSequence()) {
            QSocConsole::warn() << "'ifndef' field for instance" << instanceName
                                << "is not a list, ignoring";
        } else {
            for (const auto &node : instanceData["ifndef"]) {
                if (!node.IsScalar()) {
                    QSocConsole::warn() << "Non-scalar macro in 'ifndef' for instance"
                                        << instanceName << ", skipping";
                    continue;
                }
                QString macro = QString::fromStdString(node.as<std::string>()).trimmed();
                if (macro.isEmpty()) {
                    QSocConsole::warn()
                        << "Empty macro in 'ifndef' for instance" << instanceName << ", skipping";
                    continue;
                }
                if (!idRegex.match(macro).hasMatch()) {
                    QSocConsole::warn()
                        << "Invalid macro identifier" << macro << "in 'ifndef' for instance"
                        << instanceName << ", skipping";
                    continue;
                }
                if (!outIfndef.contains(macro)) {
                    outIfndef.append(macro);
                }
            }
        }
    }

    /* Check conflict: ifdef ∩ ifndef must be empty */
    for (const QString &macro : outIfdef) {
        if (outIfndef.contains(macro)) {
            QSocConsole::error() << "Macro" << macro
                                 << "appears in both 'ifdef' and 'ifndef' for instance"
                                 << instanceName << ", skipping instance";
            return false;
        }
    }

    /* Sort alphabetically for deterministic output */
    outIfdef.sort(Qt::CaseInsensitive);
    outIfndef.sort(Qt::CaseInsensitive);

    return true;
}

void QSocGenerateManager::writeIfdefBegin(
    QTextStream &out, const QStringList &ifdef, const QStringList &ifndef, const QString &indent)
{
    /* Output ifdef directives first (alphabetically nested) */
    for (const QString &macro : ifdef) {
        out << indent << "`ifdef " << macro << "\n";
    }

    /* Output ifndef directives second (alphabetically nested) */
    for (const QString &macro : ifndef) {
        out << indent << "`ifndef " << macro << "\n";
    }
}

void QSocGenerateManager::writeIfdefEnd(
    QTextStream &out, const QStringList &ifdef, const QStringList &ifndef, const QString &indent)
{
    /* Close in reverse order: ifndef first, then ifdef */
    for (int i = ifndef.size() - 1; i >= 0; --i) {
        out << indent << "`endif /*  !" << ifndef[i] << " */\n";
    }

    for (int i = ifdef.size() - 1; i >= 0; --i) {
        out << indent << "`endif /*  " << ifdef[i] << " */\n";
    }
}
