// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"
#include "common/qstaticstringweaver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <fstream>
#include <iostream>

bool QSocGenerateManager::loadNetlist(const QString &netlistFilePath)
{
    /* Check if the file exists */
    if (!QFile::exists(netlistFilePath)) {
        qCritical() << "Error: Netlist file does not exist:" << netlistFilePath;
        return false;
    }

    /* Open the YAML file */
    std::ifstream fileStream(netlistFilePath.toStdString());
    if (!fileStream.is_open()) {
        qCritical() << "Error: Unable to open netlist file:" << netlistFilePath;
        return false;
    }

    try {
        /* Load YAML content into netlistData */
        netlistData = YAML::Load(fileStream);

        /* Validate basic netlist structure */
        if (!netlistData["instance"]) {
            qCritical() << "Error: Invalid netlist format, missing 'instance' section";
            return false;
        }

        if (!netlistData["instance"].IsMap() || netlistData["instance"].size() == 0) {
            qCritical()
                << "Error: Invalid netlist format, 'instance' section is empty or not a map";
            return false;
        }

        /* Validate net and bus sections if they exist */
        if ((netlistData["net"] && !netlistData["net"].IsMap())
            || (netlistData["bus"] && !netlistData["bus"].IsMap())) {
            qCritical() << "Error: Invalid netlist format, invalid 'net' or 'bus' section";
            return false;
        }

        qInfo() << "Successfully loaded netlist file:" << netlistFilePath;
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "Error parsing YAML file:" << netlistFilePath << ":" << e.what();
        return false;
    }
}

bool QSocGenerateManager::setNetlistData(const YAML::Node &netlistData)
{
    try {
        /* Validate basic netlist structure */
        if (!netlistData["instance"]) {
            qCritical() << "Error: Invalid netlist format, missing 'instance' section";
            return false;
        }

        if (!netlistData["instance"].IsMap() || netlistData["instance"].size() == 0) {
            qCritical()
                << "Error: Invalid netlist format, 'instance' section is empty or not a map";
            return false;
        }

        /* Validate net and bus sections if they exist */
        if ((netlistData["net"] && !netlistData["net"].IsMap())
            || (netlistData["bus"] && !netlistData["bus"].IsMap())) {
            qCritical() << "Error: Invalid netlist format, invalid 'net' or 'bus' section";
            return false;
        }

        /* Set the netlist data */
        this->netlistData = netlistData;

        qInfo() << "Successfully set netlist data";
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "Error validating YAML data:" << e.what();
        return false;
    }
}

bool QSocGenerateManager::processNetlist()
{
    try {
        /* Check if netlistData is valid */
        if (!netlistData["instance"]) {
            qCritical() << "Error: Invalid netlist data, missing 'instance' section, call "
                           "loadNetlist() first";
            return false;
        }

        /* Create net section if it doesn't exist */
        if (!netlistData["net"]) {
            netlistData["net"] = YAML::Node(YAML::NodeType::Map);
        }

        /* Process bus section if it exists */
        if (!netlistData["bus"] || !netlistData["bus"].IsMap() || netlistData["bus"].size() == 0) {
            qInfo() << "No bus section found or empty, skipping bus processing";
        } else {
            /* Process each bus type (e.g., biu_bus) */
            for (const auto &busTypePair : netlistData["bus"]) {
                try {
                    /* Get bus type name */
                    if (!busTypePair.first.IsScalar()) {
                        qWarning() << "Warning: Bus type name is not a scalar, skipping";
                        continue;
                    }
                    const auto busTypeName = busTypePair.first.as<std::string>();
                    qInfo() << "Processing bus:" << busTypeName.c_str();

                    /* Get bus connections (should be a map) */
                    if (!busTypePair.second.IsMap()) {
                        qWarning()
                            << "Warning: Bus" << busTypeName.c_str() << "is not a map, skipping";
                        continue;
                    }
                    const YAML::Node &busConnections = busTypePair.second;
                    qInfo() << "Found" << busConnections.size() << "connections for bus"
                            << busTypeName.c_str();

                    /* Collect all valid connections */
                    struct Connection
                    {
                        std::string instanceName;
                        std::string portName;
                        std::string moduleName;
                        std::string busType;
                    };

                    std::vector<Connection> validConnections;
                    /* Will be determined from the first valid connection */
                    std::string busType;

                    /* Step 1: Validate all connections first */
                    for (const auto &connectionPair : busConnections) {
                        try {
                            if (!connectionPair.first.IsScalar()) {
                                qWarning() << "Warning: Instance name is not a scalar, skipping";
                                continue;
                            }
                            const auto instanceName = connectionPair.first.as<std::string>();

                            if (!connectionPair.second.IsMap() || !connectionPair.second["port"]
                                || !connectionPair.second["port"].IsScalar()) {
                                qWarning() << "Warning: Invalid port specification for instance"
                                           << instanceName.c_str();
                                continue;
                            }
                            const auto portName = connectionPair.second["port"].as<std::string>();

                            qInfo() << "Validating connection:" << instanceName.c_str() << "."
                                    << portName.c_str();

                            /* Validate the instance exists */
                            if (!netlistData["instance"][instanceName]) {
                                qWarning() << "Warning: Instance" << instanceName.c_str()
                                           << "not found in netlist";
                                continue;
                            }

                            /* Check for module name */
                            if (!netlistData["instance"][instanceName]["module"]
                                || !netlistData["instance"][instanceName]["module"].IsScalar()) {
                                qWarning() << "Warning: Invalid module for instance"
                                           << instanceName.c_str();
                                continue;
                            }

                            const auto moduleName
                                = netlistData["instance"][instanceName]["module"].as<std::string>();

                            /* Check if module exists */
                            if (!moduleManager
                                || !moduleManager->isModuleExist(
                                    QString::fromStdString(moduleName))) {
                                qWarning()
                                    << "Warning: Module" << moduleName.c_str() << "not found";
                                continue;
                            }

                            /* Get module data */
                            YAML::Node moduleData;
                            try {
                                moduleData = moduleManager->getModuleYaml(
                                    QString::fromStdString(moduleName));
                            } catch (const YAML::Exception &e) {
                                qWarning() << "Error getting module data:" << e.what();
                                continue;
                            }

                            /* Check if port exists in bus section */
                            if (!moduleData["bus"] || !moduleData["bus"].IsMap()) {
                                qWarning()
                                    << "Warning: No bus section in module" << moduleName.c_str();
                                continue;
                            }

                            /* Check if port exists in bus section - try multiple name variations */
                            bool portFound = false;
                            if (moduleData["bus"][portName]
                                || (portName.starts_with("pad_")
                                    && moduleData["bus"][portName.substr(4)])
                                || moduleData["bus"]["pad_" + portName]) {
                                portFound = true;
                            }

                            if (!portFound) {
                                qWarning() << "Warning: Port" << portName.c_str()
                                           << "not found in module" << moduleName.c_str();
                                continue;
                            }

                            /* Check bus type */
                            std::string currentBusType;

                            /* Try to find bus type declaration in module */
                            if (moduleData["bus"][portName] && moduleData["bus"][portName]["bus"]
                                && moduleData["bus"][portName]["bus"].IsScalar()) {
                                currentBusType
                                    = moduleData["bus"][portName]["bus"].as<std::string>();
                            } else if (
                                portName.starts_with("pad_")
                                && moduleData["bus"][portName.substr(4)]
                                && moduleData["bus"][portName.substr(4)]["bus"]
                                && moduleData["bus"][portName.substr(4)]["bus"].IsScalar()) {
                                currentBusType
                                    = moduleData["bus"][portName.substr(4)]["bus"].as<std::string>();
                            } else if (
                                moduleData["bus"]["pad_" + portName]
                                && moduleData["bus"]["pad_" + portName]["bus"]
                                && moduleData["bus"]["pad_" + portName]["bus"].IsScalar()) {
                                currentBusType
                                    = moduleData["bus"]["pad_" + portName]["bus"].as<std::string>();
                            } else {
                                qWarning() << "Warning: No bus type for port" << portName.c_str();
                                continue;
                            }

                            /* Check if this bus type exists */
                            if (!busManager
                                || !busManager->isBusExist(QString::fromStdString(currentBusType))) {
                                qWarning()
                                    << "Warning: Bus type" << currentBusType.c_str() << "not found";
                                continue;
                            }

                            /* For the first connection, record the bus type */
                            if (validConnections.empty()) {
                                busType = currentBusType;
                            }
                            /* For subsequent connections, ensure bus type is consistent */
                            else if (currentBusType != busType) {
                                qWarning() << "Warning: Mixed bus types" << busType.c_str() << "and"
                                           << currentBusType.c_str()
                                           << ", skipping inconsistent connection";
                                continue;
                            }

                            /* Add to valid connections */
                            Connection conn;
                            conn.instanceName = instanceName;
                            conn.portName     = portName;
                            conn.moduleName   = moduleName;
                            conn.busType      = currentBusType;
                            validConnections.push_back(conn);

                        } catch (const YAML::Exception &e) {
                            qWarning() << "YAML exception validating connection:" << e.what();
                            continue;
                        } catch (const std::exception &e) {
                            qWarning() << "Exception validating connection:" << e.what();
                            continue;
                        }
                    }

                    qInfo() << "Found" << validConnections.size() << "valid connections";

                    /* If no valid connections, skip */
                    if (validConnections.empty()) {
                        qWarning()
                            << "Warning: No valid connections for bus" << busTypeName.c_str();
                        continue;
                    }

                    /* Step 2: Get bus definition */
                    YAML::Node busDefinition;
                    try {
                        busDefinition = busManager->getBusYaml(QString::fromStdString(busType));
                    } catch (const YAML::Exception &e) {
                        qWarning() << "Error getting bus definition:" << e.what();
                        continue;
                    }

                    if (!busDefinition["port"] || !busDefinition["port"].IsMap()) {
                        qWarning() << "Warning: Invalid port section in bus definition for"
                                   << busType.c_str();
                        continue;
                    }

                    qInfo() << "Processing" << busDefinition["port"].size()
                            << "signals for bus type" << busType.c_str();

                    /* Step 3: Create nets for each bus signal */
                    for (const auto &portPair : busDefinition["port"]) {
                        if (!portPair.first.IsScalar()) {
                            qWarning() << "Warning: Invalid port name in bus definition, skipping";
                            continue;
                        }

                        const auto  signalName = portPair.first.as<std::string>();
                        std::string netName    = busTypeName;
                        netName += "_";
                        netName += signalName;

                        qInfo() << "Creating net for bus signal:" << signalName.c_str();

                        /* Create a net for this signal as a map (not sequence) */
                        netlistData["net"][netName] = YAML::Node(YAML::NodeType::Map);

                        /* Add each connection to this net */
                        for (const Connection &conn : validConnections) {
                            try {
                                /* Skip if module definition not available */
                                if (!moduleManager->isModuleExist(conn.moduleName.c_str())) {
                                    qWarning() << "Warning: Module" << conn.moduleName.c_str()
                                               << "not found, skipping";
                                    continue;
                                }

                                YAML::Node moduleData = moduleManager->getModuleYaml(
                                    QString::fromStdString(conn.moduleName));

                                if (!moduleData["bus"] || !moduleData["bus"].IsMap()) {
                                    qWarning() << "Warning: No bus section in module"
                                               << conn.moduleName.c_str() << ", skipping";
                                    continue;
                                }

                                /* Find the mapped port for this signal */
                                std::string mappedPortName;
                                bool        mappingFound = false;

                                /* Try with direct port name */
                                if (moduleData["bus"][conn.portName]
                                    && moduleData["bus"][conn.portName]["mapping"]
                                    && moduleData["bus"][conn.portName]["mapping"].IsMap()
                                    && moduleData["bus"][conn.portName]["mapping"][signalName]
                                    && moduleData["bus"][conn.portName]["mapping"][signalName]
                                           .IsScalar()) {
                                    mappedPortName
                                        = moduleData["bus"][conn.portName]["mapping"][signalName]
                                              .as<std::string>();
                                    mappingFound = true;
                                }
                                /* Try with pad_ stripped port name */
                                else if (
                                    conn.portName.starts_with("pad_")
                                    && moduleData["bus"][conn.portName.substr(4)]
                                    && moduleData["bus"][conn.portName.substr(4)]["mapping"]
                                    && moduleData["bus"][conn.portName.substr(4)]["mapping"].IsMap()
                                    && moduleData["bus"][conn.portName.substr(4)]["mapping"]
                                                 [signalName]
                                    && moduleData["bus"][conn.portName.substr(4)]["mapping"]
                                                 [signalName]
                                                     .IsScalar()) {
                                    mappedPortName = moduleData["bus"][conn.portName.substr(4)]
                                                               ["mapping"][signalName]
                                                                   .as<std::string>();
                                    mappingFound = true;
                                }
                                /* Try with prefixed port name (with pad_ prefix) */
                                else if (
                                    moduleData["bus"]["pad_" + conn.portName]
                                    && moduleData["bus"]["pad_" + conn.portName]["mapping"]
                                    && moduleData["bus"]["pad_" + conn.portName]["mapping"].IsMap()
                                    && moduleData["bus"]["pad_" + conn.portName]["mapping"]
                                                 [signalName]
                                    && moduleData["bus"]["pad_" + conn.portName]["mapping"]
                                                 [signalName]
                                                     .IsScalar()) {
                                    mappedPortName = moduleData["bus"]["pad_" + conn.portName]
                                                               ["mapping"][signalName]
                                                                   .as<std::string>();
                                    mappingFound = true;
                                }

                                if (!mappingFound || mappedPortName.empty()) {
                                    /* Skip this signal for this connection */
                                    continue;
                                }

                                /* Create the connection node with proper structure */
                                YAML::Node portNode = YAML::Node(YAML::NodeType::Map);
                                portNode["port"]    = mappedPortName;

                                /* Add instance->port mapping to the net */
                                netlistData["net"][netName][conn.instanceName] = portNode;

                                /* Debug the structure we just created */
                                qDebug() << "Added connection to net:" << netName.c_str()
                                         << "instance:" << conn.instanceName.c_str()
                                         << "port:" << mappedPortName.c_str();

                            } catch (const YAML::Exception &e) {
                                qWarning()
                                    << "YAML exception adding connection to net:" << e.what();
                                continue;
                            } catch (const std::exception &e) {
                                qWarning() << "Exception adding connection to net:" << e.what();
                                continue;
                            }
                        }

                        /* If no connections were added to this net, remove it */
                        if (netlistData["net"][netName].size() == 0) {
                            netlistData["net"].remove(netName);
                        }
                        /* Add debug output to verify structure */
                        else {
                            qDebug() << "Created net:" << netName.c_str() << "with structure:";
                            for (auto connIter = netlistData["net"][netName].begin();
                                 connIter != netlistData["net"][netName].end();
                                 ++connIter) {
                                if (connIter->first.IsScalar()) {
                                    qDebug()
                                        << "  Instance:"
                                        << QString::fromStdString(connIter->first.as<std::string>());
                                    if (connIter->second.IsMap() && connIter->second["port"]
                                        && connIter->second["port"].IsScalar()) {
                                        qDebug() << "    Port:"
                                                 << QString::fromStdString(
                                                        connIter->second["port"].as<std::string>());
                                    }
                                }
                            }
                        }
                    }

                } catch (const YAML::Exception &e) {
                    qCritical() << "YAML exception processing bus type:" << e.what();
                    continue;
                } catch (const std::exception &e) {
                    qCritical() << "Standard exception processing bus type:" << e.what();
                    continue;
                }
            }
        }

        /* Clean up by removing the bus section */
        netlistData.remove("bus");

        /* Process link and uplink connections */
        if (!processLinkConnections()) {
            qCritical() << "Error: Failed to process link and uplink connections";
            return false;
        }

        qInfo() << "Netlist processed successfully";
        std::cout << "Expanded Netlist:\n" << netlistData << '\n';
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processNetlist:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processNetlist:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in processNetlist";
        return false;
    }
}

/**
 * @brief Calculate the width of a bit selection expression
 * @param bitSelect Bit selection string (e.g. "[3:2]", "[5]")
 * @return The width of the bit selection (e.g. 2 for "[3:2]", 1 for "[5]")
 */
int QSocGenerateManager::calculateBitSelectWidth(const QString &bitSelect)
{
    if (bitSelect.isEmpty()) {
        /* No bit selection */
        return 0;
    }

    /* Handle range selection like [3:2] */
    const QRegularExpression      rangeRegex(R"(\[\s*(\d+)\s*:\s*(\d+)\s*\])");
    const QRegularExpressionMatch rangeMatch = rangeRegex.match(bitSelect);
    if (rangeMatch.hasMatch()) {
        bool      msb_ok = false;
        bool      lsb_ok = false;
        const int msb    = rangeMatch.captured(1).toInt(&msb_ok);
        const int lsb    = rangeMatch.captured(2).toInt(&lsb_ok);

        if (msb_ok && lsb_ok) {
            /* e.g., [3:2] has width 2 */
            return qAbs(msb - lsb) + 1;
        }
    }

    /* Handle single bit selection like [5] */
    const QRegularExpression      singleBitRegex(R"(\[\s*(\d+)\s*\])");
    const QRegularExpressionMatch singleBitMatch = singleBitRegex.match(bitSelect);
    if (singleBitMatch.hasMatch()) {
        /* Single bit has width 1 */
        return 1;
    }

    /* Unknown format, default to 0 */
    return 0;
}

/**
 * @brief Check port width consistency
 * @param portConnections   List of port connections to check
 * @return  true if consistent, false if mismatch detected
 */
bool QSocGenerateManager::checkPortWidthConsistency(const QList<PortConnection> &connections)
{
    /* If there's only 0 or 1 port, it's trivially consistent */
    if (connections.size() <= 1) {
        return true;
    }

    /* Get port widths and bit selections for all connections */
    struct PortWidthInfo
    {
        QString originalWidth;  /**< Original port width string (e.g. "[7:0]") */
        QString bitSelect;      /**< Bit selection if any (e.g. "[3:2]") */
        int     effectiveWidth; /**< Calculated effective width considering bit selection */
    };
    QMap<QPair<QString, QString>, PortWidthInfo> portWidthInfos;

    for (const auto &conn : connections) {
        const QString instanceName = conn.instanceName;
        const QString portName     = conn.portName;
        PortWidthInfo widthInfo;
        widthInfo.originalWidth  = "";
        widthInfo.bitSelect      = "";
        widthInfo.effectiveWidth = 0;

        if (conn.type == PortType::TopLevel) {
            /* Handle top-level port */
            if (netlistData["port"] && netlistData["port"][portName.toStdString()]) {
                /* Get port width from netlist data */
                if (netlistData["port"][portName.toStdString()]["type"]
                    && netlistData["port"][portName.toStdString()]["type"].IsScalar()) {
                    QString width = QString::fromStdString(
                        netlistData["port"][portName.toStdString()]["type"].as<std::string>());
                    /* Clean type for Verilog 2001 compatibility */
                    width = QSocGenerateManager::cleanTypeForWireDeclaration(width);
                    widthInfo.originalWidth = width;

                    /* Calculate width in bits */
                    const QRegularExpression widthRegex(R"(\[(\d+)(?::(\d+))?\])");
                    auto                     match = widthRegex.match(width);
                    if (match.hasMatch()) {
                        bool      msb_ok = false;
                        const int msb    = match.captured(1).toInt(&msb_ok);

                        if (msb_ok) {
                            if (match.capturedLength(2) > 0) {
                                /* Case with specified LSB, e.g. [7:3] */
                                bool      lsb_ok = false;
                                const int lsb    = match.captured(2).toInt(&lsb_ok);
                                if (lsb_ok) {
                                    widthInfo.effectiveWidth = qAbs(msb - lsb) + 1;
                                }
                            } else {
                                /* Case with only MSB specified, e.g. [7] */
                                widthInfo.effectiveWidth = msb + 1;
                            }
                        }
                    } else {
                        /* Default to 1-bit if no width specified */
                        widthInfo.effectiveWidth = 1;
                    }
                }

                /* Check for bit selection in net connections */
                for (const auto &netIter : netlistData["net"]) {
                    if (netIter.second.IsMap()) {
                        for (const auto &instIter : netIter.second) {
                            if (instIter.second.IsMap() && instIter.second["port"]
                                && instIter.second["port"].IsScalar()) {
                                const QString connPortName = QString::fromStdString(
                                    instIter.second["port"].as<std::string>());

                                if (connPortName == portName) {
                                    /* Found a connection to this port, check for bits attribute */
                                    if (instIter.second["bits"]
                                        && instIter.second["bits"].IsScalar()) {
                                        widthInfo.bitSelect = QString::fromStdString(
                                            instIter.second["bits"].as<std::string>());

                                        /* Update effective width based on bit selection */
                                        const int selectWidth = calculateBitSelectWidth(
                                            widthInfo.bitSelect);
                                        if (selectWidth > 0) {
                                            widthInfo.effectiveWidth = selectWidth;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            /* Handle module port */
            if (netlistData["instance"][conn.instanceName.toStdString()]
                && netlistData["instance"][conn.instanceName.toStdString()]["module"]
                && netlistData["instance"][conn.instanceName.toStdString()]["module"].IsScalar()) {
                const QString moduleName = QString::fromStdString(
                    netlistData["instance"][conn.instanceName.toStdString()]["module"]
                        .as<std::string>());

                /* Get port width from module definition */
                if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                    YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                    if (moduleData["port"] && moduleData["port"].IsMap()
                        && moduleData["port"][portName.toStdString()]["type"]
                        && moduleData["port"][portName.toStdString()]["type"].IsScalar()) {
                        QString width = QString::fromStdString(
                            moduleData["port"][portName.toStdString()]["type"].as<std::string>());
                        /* Clean type for Verilog 2001 compatibility */
                        width = QSocGenerateManager::cleanTypeForWireDeclaration(width);
                        widthInfo.originalWidth = width;

                        /* Calculate width in bits */
                        const QRegularExpression widthRegex(R"(\[(\d+)(?::(\d+))?\])");
                        auto                     match = widthRegex.match(width);
                        if (match.hasMatch()) {
                            bool      msb_ok = false;
                            const int msb    = match.captured(1).toInt(&msb_ok);

                            if (msb_ok) {
                                if (match.capturedLength(2) > 0) {
                                    /* Case with specified LSB, e.g. [7:3] */
                                    bool      lsb_ok = false;
                                    const int lsb    = match.captured(2).toInt(&lsb_ok);
                                    if (lsb_ok) {
                                        widthInfo.effectiveWidth = qAbs(msb - lsb) + 1;
                                    }
                                } else {
                                    /* Case with only MSB specified, e.g. [7] */
                                    widthInfo.effectiveWidth = msb + 1;
                                }
                            }
                        } else {
                            /* Default to 1-bit if no width specified */
                            widthInfo.effectiveWidth = 1;
                        }
                    }
                }

                /* Check if this instance-port has a bit selection in the netlist */
                for (const auto &netIter : netlistData["net"]) {
                    if (netIter.second.IsMap() && netIter.second[instanceName.toStdString()]) {
                        auto instNode = netIter.second[instanceName.toStdString()];
                        if (instNode.IsMap() && instNode["port"] && instNode["port"].IsScalar()) {
                            const QString connPortName = QString::fromStdString(
                                instNode["port"].as<std::string>());

                            if (connPortName == portName) {
                                /* Found a connection to this port, check for bits attribute */
                                if (instNode["bits"] && instNode["bits"].IsScalar()) {
                                    widthInfo.bitSelect = QString::fromStdString(
                                        instNode["bits"].as<std::string>());

                                    /* Update effective width based on bit selection */
                                    const int selectWidth = calculateBitSelectWidth(
                                        widthInfo.bitSelect);
                                    if (selectWidth > 0) {
                                        widthInfo.effectiveWidth = selectWidth;
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        portWidthInfos[qMakePair(instanceName, portName)] = widthInfo;
    }

    /* Compare effective widths for consistency */
    int  referenceWidth = -1;
    bool firstPort      = true;

    for (auto it = portWidthInfos.constBegin(); it != portWidthInfos.constEnd(); ++it) {
        const PortWidthInfo &info = it.value();

        /* Only consider ports with a valid width */
        if (info.effectiveWidth > 0) {
            if (firstPort) {
                referenceWidth = info.effectiveWidth;
                firstPort      = false;
            } else if (info.effectiveWidth != referenceWidth) {
                /* Width mismatch detected */
                return false;
            }
        }
    }

    /* All effective widths are consistent or some are unspecified */
    return true;
}

/**
 * @brief Check port direction consistency
 * @param portConnections   List of port connections to check
 * @return  PortDirectionStatus indicating the status (OK, Undriven, or Multidrive)
 */
QSocGenerateManager::PortDirectionStatus QSocGenerateManager::checkPortDirectionConsistency(
    const QList<PortConnection> &connections)
{
    int outputCount  = 0;
    int inputCount   = 0;
    int inoutCount   = 0;
    int unknownCount = 0;

    /* Count input/output ports */
    for (const auto &conn : connections) {
        QString direction = "unknown";

        if (conn.type == PortType::TopLevel) {
            /* For top-level ports, we need to reverse the direction for internal net perspective
             * e.g., a top-level output is actually an input from the internal net's perspective */
            if (netlistData["port"] && netlistData["port"][conn.portName.toStdString()]
                && netlistData["port"][conn.portName.toStdString()]["direction"]
                && netlistData["port"][conn.portName.toStdString()]["direction"].IsScalar()) {
                const QString dirStr
                    = QString::fromStdString(
                          netlistData["port"][conn.portName.toStdString()]["direction"]
                              .as<std::string>())
                          .toLower();

                /* Reverse direction for internal net perspective */
                if (dirStr == "out" || dirStr == "output") {
                    direction = "input"; /* Top-level output is an input for internal nets */
                } else if (dirStr == "in" || dirStr == "input") {
                    direction = "output"; /* Top-level input is an output for internal nets */
                } else if (dirStr == "inout") {
                    direction = "inout";
                }
            }
        } else {
            /* Regular module port */
            if (netlistData["instance"][conn.instanceName.toStdString()]
                && netlistData["instance"][conn.instanceName.toStdString()]["module"]
                && netlistData["instance"][conn.instanceName.toStdString()]["module"].IsScalar()) {
                const QString moduleName = QString::fromStdString(
                    netlistData["instance"][conn.instanceName.toStdString()]["module"]
                        .as<std::string>());

                /* Get port direction from module definition */
                if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                    YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                    if (moduleData["port"] && moduleData["port"].IsMap()
                        && moduleData["port"][conn.portName.toStdString()]["direction"]
                        && moduleData["port"][conn.portName.toStdString()]["direction"].IsScalar()) {
                        direction = QString::fromStdString(
                                        moduleData["port"][conn.portName.toStdString()]["direction"]
                                            .as<std::string>())
                                        .toLower();

                        /* Handle both full and abbreviated forms */
                        if (direction == "out" || direction == "output") {
                            direction = "output";
                        } else if (direction == "in" || direction == "input") {
                            direction = "input";
                        } else if (direction == "inout") {
                            direction = "inout";
                        }
                    }
                }
            }
        }

        /* Count directions */
        if (direction == "input") {
            inputCount++;
        } else if (direction == "output") {
            outputCount++;
        } else if (direction == "inout") {
            inoutCount++;
        } else {
            unknownCount++;
        }
    }

    /* Check for errors */
    if (outputCount == 0 && inoutCount == 0) {
        /* No output/inout, only inputs or unknowns - net is undriven */
        return PortDirectionStatus::Undriven;
    }
    if (outputCount + inoutCount > 1) {
        /* Multiple output or inout ports - potential conflict */
        return PortDirectionStatus::Multidrive;
    }
    /* Normal case: one driver, multiple inputs */
    return PortDirectionStatus::Valid;
}

/**
 * @brief Process link and uplink connections in the netlist
 * @return true if successful, false on error
 */
bool QSocGenerateManager::processLinkConnections()
{
    try {
        /* Ensure net section exists */
        if (!netlistData["net"]) {
            netlistData["net"] = YAML::Node(YAML::NodeType::Map);
        }

        /* Ensure port section exists */
        if (!netlistData["port"]) {
            netlistData["port"] = YAML::Node(YAML::NodeType::Map);
        }

        qInfo() << "Processing link and uplink connections...";

        /* Process each instance */
        for (const auto &instancePair : netlistData["instance"]) {
            if (!instancePair.first.IsScalar()) {
                continue;
            }
            const std::string instanceName = instancePair.first.as<std::string>();
            const YAML::Node &instanceNode = instancePair.second;

            if (!instanceNode.IsMap() || !instanceNode["port"] || !instanceNode["port"].IsMap()) {
                continue;
            }

            /* Get module name */
            if (!instanceNode["module"] || !instanceNode["module"].IsScalar()) {
                continue;
            }
            const std::string moduleName = instanceNode["module"].as<std::string>();

            /* Check if module exists */
            if (!moduleManager
                || !moduleManager->isModuleExist(QString::fromStdString(moduleName))) {
                continue;
            }

            /* Get module data for port information */
            YAML::Node moduleData;
            try {
                moduleData = moduleManager->getModuleYaml(QString::fromStdString(moduleName));
            } catch (const YAML::Exception &e) {
                qWarning() << "Error getting module data for" << moduleName.c_str() << ":"
                           << e.what();
                continue;
            }

            /* Process each port in the instance */
            for (const auto &portPair : instanceNode["port"]) {
                if (!portPair.first.IsScalar() || !portPair.second.IsMap()) {
                    continue;
                }
                const std::string portName = portPair.first.as<std::string>();
                const YAML::Node &portNode = portPair.second;

                /* Check for link attribute */
                if (portNode["link"] && portNode["link"].IsScalar()) {
                    const std::string netName = portNode["link"].as<std::string>();

                    if (!processLinkConnection(
                            instanceName, portName, netName, moduleName, moduleData)) {
                        return false;
                    }
                }

                /* Check for uplink attribute */
                if (portNode["uplink"] && portNode["uplink"].IsScalar()) {
                    const std::string netName = portNode["uplink"].as<std::string>();

                    if (!processUplinkConnection(
                            instanceName, portName, netName, moduleName, moduleData)) {
                        return false;
                    }
                }
            }
        }

        qInfo() << "Successfully processed link and uplink connections";
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processLinkConnections:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processLinkConnections:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in processLinkConnections";
        return false;
    }
}

/**
 * @brief Process a single link connection
 * @param instanceName The instance name
 * @param portName The port name
 * @param netName The net name to create/connect to
 * @param moduleName The module name
 * @param moduleData The module YAML data
 * @return true if successful, false on error
 */
bool QSocGenerateManager::processLinkConnection(
    const std::string &instanceName,
    const std::string &portName,
    const std::string &netName,
    const std::string &moduleName,
    const YAML::Node  &moduleData)
{
    try {
        qInfo() << "Processing link connection:" << instanceName.c_str() << "." << portName.c_str()
                << "-> net:" << netName.c_str();

        /* Create or get the net */
        if (!netlistData["net"][netName]) {
            netlistData["net"][netName] = YAML::Node(YAML::NodeType::Map);
        }

        /* Create port connection node */
        YAML::Node portConnectionNode = YAML::Node(YAML::NodeType::Map);
        portConnectionNode["port"]    = portName;

        /* Add the connection to the net */
        netlistData["net"][netName][instanceName] = portConnectionNode;

        qInfo() << "Successfully created link connection for net:" << netName.c_str();
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processLinkConnection:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processLinkConnection:" << e.what();
        return false;
    }
}

/**
 * @brief Process a single uplink connection
 * @param instanceName The instance name
 * @param portName The port name
 * @param netName The net name to create/connect to
 * @param moduleName The module name
 * @param moduleData The module YAML data
 * @return true if successful, false on error
 */
bool QSocGenerateManager::processUplinkConnection(
    const std::string &instanceName,
    const std::string &portName,
    const std::string &netName,
    const std::string &moduleName,
    const YAML::Node  &moduleData)
{
    try {
        qInfo() << "Processing uplink connection:" << instanceName.c_str() << "."
                << portName.c_str() << "-> net:" << netName.c_str()
                << ", top-level port:" << netName.c_str();

        /* Get port information from module */
        if (!moduleData["port"] || !moduleData["port"].IsMap()) {
            qWarning() << "Warning: No port section in module" << moduleName.c_str();
            return false;
        }

        if (!moduleData["port"][portName] || !moduleData["port"][portName].IsMap()) {
            qWarning() << "Warning: Port" << portName.c_str() << "not found in module"
                       << moduleName.c_str();
            return false;
        }

        const YAML::Node &modulePortNode = moduleData["port"][portName];

        /* Get port direction */
        if (!modulePortNode["direction"] || !modulePortNode["direction"].IsScalar()) {
            qWarning() << "Warning: No direction for port" << portName.c_str() << "in module"
                       << moduleName.c_str();
            return false;
        }
        std::string modulePortDirection = modulePortNode["direction"].as<std::string>();

        /* Convert to lowercase for consistency */
        std::transform(
            modulePortDirection.begin(),
            modulePortDirection.end(),
            modulePortDirection.begin(),
            ::tolower);

        /* Get port type/width */
        std::string modulePortType = "wire"; /* default type */
        if (modulePortNode["type"] && modulePortNode["type"].IsScalar()) {
            modulePortType = modulePortNode["type"].as<std::string>();
        }

        /* Calculate top-level port direction (opposite of module port, except for inout) */
        std::string topLevelDirection;
        if (modulePortDirection == "input" || modulePortDirection == "in") {
            topLevelDirection = "output";
        } else if (modulePortDirection == "output" || modulePortDirection == "out") {
            topLevelDirection = "input";
        } else if (modulePortDirection == "inout") {
            topLevelDirection = "inout";
        } else {
            qWarning() << "Warning: Unknown port direction" << modulePortDirection.c_str()
                       << "for port" << portName.c_str();
            return false;
        }

        /* Check if top-level port already exists */
        if (netlistData["port"][netName]) {
            /* Port exists, check compatibility */
            const YAML::Node &existingPortNode = netlistData["port"][netName];

            /* Check direction compatibility */
            std::string existingDirection;
            if (existingPortNode["direction"] && existingPortNode["direction"].IsScalar()) {
                existingDirection = existingPortNode["direction"].as<std::string>();
                std::transform(
                    existingDirection.begin(),
                    existingDirection.end(),
                    existingDirection.begin(),
                    ::tolower);
            }

            bool directionCompatible = false;
            if (topLevelDirection == "inout" || existingDirection == "inout") {
                /* inout is compatible with any direction */
                directionCompatible = true;
            } else if (topLevelDirection == existingDirection) {
                directionCompatible = true;
            }

            if (!directionCompatible) {
                qCritical() << "Error: Direction mismatch for uplink port" << netName.c_str()
                            << ". Expected:" << topLevelDirection.c_str()
                            << ", existing:" << existingDirection.c_str();
                return false;
            }

            /* Check type/width compatibility */
            std::string existingType;
            if (existingPortNode["type"] && existingPortNode["type"].IsScalar()) {
                existingType = existingPortNode["type"].as<std::string>();
            }

            if (!existingType.empty() && existingType != modulePortType) {
                /* Calculate widths for comparison */
                int moduleWidth   = calculatePortWidth(modulePortType);
                int existingWidth = calculatePortWidth(existingType);

                if (moduleWidth > 0 && existingWidth > 0 && moduleWidth != existingWidth) {
                    qCritical() << "Error: Type/width mismatch for uplink port" << netName.c_str()
                                << ". Expected width:" << moduleWidth
                                << ", existing width:" << existingWidth;
                    return false;
                }
            }

            qInfo() << "Uplink port" << netName.c_str() << "already exists and is compatible";
        } else {
            /* Create new top-level port */
            YAML::Node topLevelPortNode   = YAML::Node(YAML::NodeType::Map);
            topLevelPortNode["direction"] = topLevelDirection;
            topLevelPortNode["type"]      = modulePortType;

            netlistData["port"][netName] = topLevelPortNode;

            qInfo() << "Created new top-level port:" << netName.c_str()
                    << ", direction:" << topLevelDirection.c_str()
                    << ", type:" << modulePortType.c_str();
        }

        /* Create or get the net */
        if (!netlistData["net"][netName]) {
            netlistData["net"][netName] = YAML::Node(YAML::NodeType::Map);
        }

        /* Add module instance connection to the net */
        YAML::Node portConnectionNode             = YAML::Node(YAML::NodeType::Map);
        portConnectionNode["port"]                = portName;
        netlistData["net"][netName][instanceName] = portConnectionNode;

        /* Add top-level port connection to the net */
        YAML::Node topLevelConnectionNode        = YAML::Node(YAML::NodeType::Map);
        topLevelConnectionNode["port"]           = netName;
        netlistData["net"][netName]["top_level"] = topLevelConnectionNode;

        qInfo() << "Successfully created uplink connection for net:" << netName.c_str();
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processUplinkConnection:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processUplinkConnection:" << e.what();
        return false;
    }
}

/**
 * @brief Calculate the width of a port from its type string
 * @param portType The port type string (e.g., "wire [7:0]", "reg [15:0]", "wire")
 * @return The width in bits, or -1 if cannot be determined
 */
int QSocGenerateManager::calculatePortWidth(const std::string &portType)
{
    QString type = QString::fromStdString(portType);

    /* Handle range specification like [7:0] or [15:8] */
    const QRegularExpression      rangeRegex(R"(\[(\d+):(\d+)\])");
    const QRegularExpressionMatch rangeMatch = rangeRegex.match(type);
    if (rangeMatch.hasMatch()) {
        bool      msb_ok = false;
        bool      lsb_ok = false;
        const int msb    = rangeMatch.captured(1).toInt(&msb_ok);
        const int lsb    = rangeMatch.captured(2).toInt(&lsb_ok);

        if (msb_ok && lsb_ok) {
            return qAbs(msb - lsb) + 1;
        }
    }

    /* Handle single bit specification like [5] */
    const QRegularExpression      singleBitRegex(R"(\[(\d+)\])");
    const QRegularExpressionMatch singleBitMatch = singleBitRegex.match(type);
    if (singleBitMatch.hasMatch()) {
        bool      msb_ok = false;
        const int msb    = singleBitMatch.captured(1).toInt(&msb_ok);
        if (msb_ok) {
            return msb + 1; /* [5] means 6 bits: [5:0] */
        }
    }

    /* Default single bit */
    return 1;
}
