// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qslangdriver.h"
#include "common/qsocgeneratemanager.h"
#include "common/qstaticlog.h"
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
        // Check if instance section exists and is valid when present
        if (netlistData["instance"] && !netlistData["instance"].IsMap()) {
            qCritical() << "Error: Invalid netlist format, 'instance' section is not a map";
            return false;
        }

        // Allow empty or missing instance section if comb, seq, or fsm section exists
        bool hasInstances = netlistData["instance"] && netlistData["instance"].IsMap()
                            && netlistData["instance"].size() > 0;
        bool hasCombSeqFsm = netlistData["comb"] || netlistData["seq"] || netlistData["fsm"];
        bool hasReset      = netlistData["reset"] && netlistData["reset"].IsSequence()
                        && netlistData["reset"].size() > 0;
        bool hasClock = netlistData["clock"] && netlistData["clock"].IsSequence()
                        && netlistData["clock"].size() > 0;
        bool hasPower = netlistData["power"] && netlistData["power"].IsSequence()
                        && netlistData["power"].size() > 0;

        if (!hasInstances && !hasCombSeqFsm && !hasReset && !hasClock && !hasPower) {
            qCritical() << "Error: Invalid netlist format, no 'instance' section and no 'comb', "
                           "'seq', 'fsm', 'reset', 'clock', or 'power' section found";
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
        /* Validate basic netlist structure - allow missing instance if primitives exist */
        bool hasInstances  = netlistData["instance"] && netlistData["instance"].IsMap();
        bool hasPrimitives = netlistData["comb"] || netlistData["seq"] || netlistData["fsm"]
                             || netlistData["reset"] || netlistData["clock"];

        if (!hasInstances && !hasPrimitives) {
            qCritical() << "Error: Invalid netlist format, missing 'instance' section and no "
                           "primitives found";
            return false;
        }

        if (netlistData["instance"] && !netlistData["instance"].IsMap()) {
            qCritical() << "Error: Invalid netlist format, 'instance' section is not a map";
            return false;
        }

        // Allow empty instance section if any primitives exist
        if (hasInstances && netlistData["instance"].size() == 0 && !hasPrimitives) {
            qCritical() << "Error: Invalid netlist format, 'instance' section is empty and no "
                           "primitives found";
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
        /* Check if netlistData is valid - allow missing instance if primitives exist */
        bool hasInstances  = netlistData["instance"] && netlistData["instance"].IsMap();
        bool hasPrimitives = netlistData["comb"] || netlistData["seq"] || netlistData["fsm"]
                             || netlistData["reset"] || netlistData["clock"]
                             || netlistData["power"];

        if (!hasInstances && !hasPrimitives) {
            qCritical() << "Error: Invalid netlist data, missing 'instance' section and no "
                           "primitives found, call loadNetlist() first";
            return false;
        }

        /* Expand bus links before processing */
        if (!expandBusLink()) {
            qCritical() << "Error: Failed to expand bus links";
            return false;
        }

        /* Expand bus uplinks before processing */
        if (!expandBusUplink()) {
            qCritical() << "Error: Failed to expand bus uplinks";
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

                    /* Get bus connections (should be a sequence/list) */
                    if (!busTypePair.second.IsSequence()) {
                        qWarning() << "Warning: Bus" << busTypeName.c_str()
                                   << "is not a sequence, skipping";
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
                    for (const auto &connectionNode : busConnections) {
                        try {
                            if (!connectionNode.IsMap() || !connectionNode["instance"]
                                || !connectionNode["instance"].IsScalar()) {
                                qWarning() << "Warning: Invalid instance specification, skipping";
                                continue;
                            }
                            const auto instanceName = connectionNode["instance"].as<std::string>();

                            if (!connectionNode["port"] || !connectionNode["port"].IsScalar()) {
                                qWarning() << "Warning: Invalid port specification for instance"
                                           << instanceName.c_str();
                                continue;
                            }
                            const auto portName = connectionNode["port"].as<std::string>();

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

                        /* Create a net for this signal using List format for consistency */
                        netlistData["net"][netName] = YAML::Node(YAML::NodeType::Sequence);

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
                                YAML::Node connectionNode  = YAML::Node(YAML::NodeType::Map);
                                connectionNode["instance"] = conn.instanceName;
                                connectionNode["port"]     = mappedPortName;

                                /* Preserve port type/width information from module definition */
                                if (moduleData["port"] && moduleData["port"].IsMap()
                                    && moduleData["port"][mappedPortName]
                                    && moduleData["port"][mappedPortName]["type"]
                                    && moduleData["port"][mappedPortName]["type"].IsScalar()) {
                                    connectionNode["type"]
                                        = moduleData["port"][mappedPortName]["type"]
                                              .as<std::string>();
                                }

                                /* Add connection to the net using List format */
                                netlistData["net"][netName].push_back(connectionNode);

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

        /* Process combinational logic section */
        if (!processCombLogic()) {
            qCritical() << "Error: Failed to process combinational logic";
            return false;
        }

        /* Process sequential logic section */
        if (!processSeqLogic()) {
            qCritical() << "Error: Failed to process sequential logic";
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

bool QSocGenerateManager::expandBusLink()
{
    try {
        /* Check if instance section exists */
        if (!netlistData["instance"] || !netlistData["instance"].IsMap()) {
            /* No instance section, nothing to expand */
            return true;
        }

        /* Create bus section if it doesn't exist */
        if (!netlistData["bus"]) {
            netlistData["bus"] = YAML::Node(YAML::NodeType::Map);
        }

        /* Iterate through all instances */
        for (auto instancePair : netlistData["instance"]) {
            if (!instancePair.first.IsScalar()) {
                continue;
            }

            const std::string instanceName = instancePair.first.as<std::string>();
            YAML::Node        instanceNode = instancePair.second;

            /* Check if instance has bus section */
            if (!instanceNode["bus"] || !instanceNode["bus"].IsMap()) {
                continue;
            }

            /* Process each bus in the instance */
            for (auto busPair : instanceNode["bus"]) {
                if (!busPair.first.IsScalar()) {
                    continue;
                }

                const std::string busPortName = busPair.first.as<std::string>();
                YAML::Node        busNode     = busPair.second;

                /* Check if bus has link attribute */
                if (!busNode["link"] || !busNode["link"].IsScalar()) {
                    continue;
                }

                const std::string linkName = busNode["link"].as<std::string>();
                qInfo() << "Expanding bus link:" << instanceName.c_str() << "."
                        << busPortName.c_str() << " -> " << linkName.c_str();

                /* Create or update the bus in netlist bus section */
                if (!netlistData["bus"][linkName]) {
                    netlistData["bus"][linkName] = YAML::Node(YAML::NodeType::Sequence);
                }

                /* Add connection to the bus */
                YAML::Node connection;
                connection["instance"] = instanceName;
                connection["port"]     = busPortName;

                /* Check if this connection already exists to avoid duplicates */
                bool exists = false;
                for (const auto &existingConn : netlistData["bus"][linkName]) {
                    if (existingConn["instance"] && existingConn["port"]
                        && existingConn["instance"].as<std::string>() == instanceName
                        && existingConn["port"].as<std::string>() == busPortName) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    netlistData["bus"][linkName].push_back(connection);
                }

                /* Remove the link attribute from instance bus node (keep other attributes) */
                busNode.remove("link");
            }
        }

        qInfo() << "Bus link expansion completed successfully";
        return true;

    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in expandBusLink:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in expandBusLink:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in expandBusLink";
        return false;
    }
}

bool QSocGenerateManager::expandBusUplink()
{
    try {
        /* Check if instance section exists */
        if (!netlistData["instance"] || !netlistData["instance"].IsMap()) {
            /* No instance section, nothing to expand */
            return true;
        }

        qInfo() << "Processing bus uplink expansion...";

        /* Iterate through all instances */
        for (auto instancePair : netlistData["instance"]) {
            if (!instancePair.first.IsScalar()) {
                continue;
            }

            const std::string instanceName = instancePair.first.as<std::string>();
            YAML::Node        instanceNode = instancePair.second;

            /* Check if instance has bus section */
            if (!instanceNode["bus"] || !instanceNode["bus"].IsMap()) {
                continue;
            }

            /* Get module name for port mapping */
            if (!instanceNode["module"] || !instanceNode["module"].IsScalar()) {
                continue;
            }
            const std::string moduleName = instanceNode["module"].as<std::string>();

            /* Get module data */
            if (!moduleManager
                || !moduleManager->isModuleExist(QString::fromStdString(moduleName))) {
                continue;
            }

            YAML::Node moduleData;
            try {
                moduleData = moduleManager->getModuleYaml(QString::fromStdString(moduleName));
            } catch (const YAML::Exception &e) {
                qWarning() << "Error getting module data for bus uplink:" << e.what();
                continue;
            }

            /* Process each bus in the instance */
            for (auto busPair : instanceNode["bus"]) {
                if (!busPair.first.IsScalar()) {
                    continue;
                }

                const std::string busPortName = busPair.first.as<std::string>();
                YAML::Node        busNode     = busPair.second;

                /* Check if bus has uplink attribute */
                if (!busNode["uplink"] || !busNode["uplink"].IsScalar()) {
                    continue;
                }

                const std::string uplinkName = busNode["uplink"].as<std::string>();
                qInfo() << "Expanding bus uplink:" << instanceName.c_str() << "."
                        << busPortName.c_str() << " -> " << uplinkName.c_str();

                /* Get bus type from module definition */
                if (!moduleData["bus"] || !moduleData["bus"].IsMap()
                    || !moduleData["bus"][busPortName]) {
                    qWarning() << "Warning: Bus port" << busPortName.c_str()
                               << "not found in module" << moduleName.c_str();
                    continue;
                }

                const YAML::Node &moduleBusNode = moduleData["bus"][busPortName];
                if (!moduleBusNode["bus"] || !moduleBusNode["bus"].IsScalar()) {
                    qWarning() << "Warning: No bus type for port" << busPortName.c_str();
                    continue;
                }

                const std::string busType = moduleBusNode["bus"].as<std::string>();

                /* Get bus definition */
                if (!busManager || !busManager->isBusExist(QString::fromStdString(busType))) {
                    qWarning() << "Warning: Bus type" << busType.c_str() << "not found";
                    continue;
                }

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

                /* Get mapping from module bus definition */
                if (!moduleBusNode["mapping"] || !moduleBusNode["mapping"].IsMap()) {
                    qWarning() << "Warning: No mapping for bus port" << busPortName.c_str();
                    continue;
                }

                const YAML::Node &mappingNode = moduleBusNode["mapping"];

                /* Create port uplink for each bus signal */
                for (const auto &busSignalPair : busDefinition["port"]) {
                    if (!busSignalPair.first.IsScalar()) {
                        continue;
                    }

                    const std::string signalName = busSignalPair.first.as<std::string>();

                    /* Find mapped port name */
                    if (!mappingNode[signalName] || !mappingNode[signalName].IsScalar()) {
                        qWarning() << "Warning: No mapping for signal" << signalName.c_str()
                                   << "in bus port" << busPortName.c_str();
                        continue;
                    }

                    const std::string mappedPortName   = mappingNode[signalName].as<std::string>();
                    const std::string toplevelPortName = uplinkName + "_" + signalName;

                    qInfo() << "Creating port uplink:" << instanceName.c_str() << "."
                            << mappedPortName.c_str() << " -> " << toplevelPortName.c_str();

                    /* Create or update the port uplink in instance */
                    if (!instanceNode["port"]) {
                        instanceNode["port"] = YAML::Node(YAML::NodeType::Map);
                    }

                    if (!instanceNode["port"][mappedPortName]) {
                        instanceNode["port"][mappedPortName] = YAML::Node(YAML::NodeType::Map);
                    }

                    instanceNode["port"][mappedPortName]["uplink"] = toplevelPortName;
                }

                /* Remove the uplink attribute from instance bus node */
                busNode.remove("uplink");
            }
        }

        qInfo() << "Bus uplink expansion completed successfully";
        return true;

    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in expandBusUplink:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in expandBusUplink:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in expandBusUplink";
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
        QString direction;      /**< Port direction (input/output/inout) */
        int     effectiveWidth; /**< Calculated effective width considering bit selection */
    };
    QMap<QPair<QString, QString>, PortWidthInfo> portWidthInfos;

    for (const auto &conn : connections) {
        const QString instanceName = conn.instanceName;
        const QString portName     = conn.portName;
        PortWidthInfo widthInfo;
        widthInfo.originalWidth  = "";
        widthInfo.bitSelect      = "";
        widthInfo.direction      = "";
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

                /* Get port direction from netlist data */
                if (netlistData["port"][portName.toStdString()]["direction"]
                    && netlistData["port"][portName.toStdString()]["direction"].IsScalar()) {
                    widthInfo.direction = QString::fromStdString(
                        netlistData["port"][portName.toStdString()]["direction"].as<std::string>());
                }

                /* Check for bit selection in net connections */
                for (const auto &netIter : netlistData["net"]) {
                    if (netIter.second.IsSequence()) {
                        for (const auto &connectionNode : netIter.second) {
                            if (connectionNode.IsMap() && connectionNode["port"]
                                && connectionNode["port"].IsScalar()) {
                                const QString connPortName = QString::fromStdString(
                                    connectionNode["port"].as<std::string>());

                                if (connPortName == portName) {
                                    /* Found a connection to this port, check for bits attribute */
                                    if (connectionNode["bits"]
                                        && connectionNode["bits"].IsScalar()) {
                                        widthInfo.bitSelect = QString::fromStdString(
                                            connectionNode["bits"].as<std::string>());

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
        } else if (conn.type == PortType::CombSeqFsm) {
            /* Handle comb/seq/fsm output */
            /* For comb/seq/fsm outputs, the portName contains the signal with bit selection */
            auto    parsed    = parseSignalBitSelect(portName);
            QString baseName  = parsed.first;
            QString bitSelect = parsed.second;

            widthInfo.bitSelect = bitSelect;
            widthInfo.direction = "output"; /* Comb/seq/fsm are always output drivers */

            /* Find the port width from the top-level port definition */
            if (netlistData["port"] && netlistData["port"][baseName.toStdString()]) {
                if (netlistData["port"][baseName.toStdString()]["type"]
                    && netlistData["port"][baseName.toStdString()]["type"].IsScalar()) {
                    QString width = QString::fromStdString(
                        netlistData["port"][baseName.toStdString()]["type"].as<std::string>());
                    width                   = cleanTypeForWireDeclaration(width);
                    widthInfo.originalWidth = width;

                    /* Calculate effective width based on bit selection */
                    if (!bitSelect.isEmpty()) {
                        const int selectWidth = calculateBitSelectWidth(bitSelect);
                        if (selectWidth > 0) {
                            widthInfo.effectiveWidth = selectWidth;
                        }
                    } else {
                        /* Calculate full width if no bit selection */
                        const QRegularExpression widthRegex(R"(\[(\d+)(?::(\d+))?\])");
                        auto                     match = widthRegex.match(width);
                        if (match.hasMatch()) {
                            bool      msb_ok = false;
                            const int msb    = match.captured(1).toInt(&msb_ok);
                            if (msb_ok) {
                                if (match.capturedLength(2) > 0) {
                                    bool      lsb_ok = false;
                                    const int lsb    = match.captured(2).toInt(&lsb_ok);
                                    if (lsb_ok) {
                                        widthInfo.effectiveWidth = qAbs(msb - lsb) + 1;
                                    }
                                } else {
                                    widthInfo.effectiveWidth = msb + 1;
                                }
                            }
                        } else {
                            widthInfo.effectiveWidth = 1;
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

                        /* Get port direction from module definition */
                        if (moduleData["port"][portName.toStdString()]["direction"]
                            && moduleData["port"][portName.toStdString()]["direction"].IsScalar()) {
                            widthInfo.direction = QString::fromStdString(
                                moduleData["port"][portName.toStdString()]["direction"]
                                    .as<std::string>());
                        }
                    }
                }

                /* Check if this instance-port has a bit selection in the netlist */
                for (const auto &netIter : netlistData["net"]) {
                    if (netIter.second.IsSequence()) {
                        for (const auto &connectionNode : netIter.second) {
                            if (connectionNode.IsMap() && connectionNode["instance"]
                                && connectionNode["instance"].IsScalar() && connectionNode["port"]
                                && connectionNode["port"].IsScalar()) {
                                const QString connInstanceName = QString::fromStdString(
                                    connectionNode["instance"].as<std::string>());
                                const QString connPortName = QString::fromStdString(
                                    connectionNode["port"].as<std::string>());

                                if (connInstanceName == instanceName && connPortName == portName) {
                                    /* Found a connection to this port, check for bits attribute */
                                    if (connectionNode["bits"]
                                        && connectionNode["bits"].IsScalar()) {
                                        widthInfo.bitSelect = QString::fromStdString(
                                            connectionNode["bits"].as<std::string>());

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
        }

        portWidthInfos[qMakePair(instanceName, portName)] = widthInfo;
    }

    /* Use original width comparison logic but add special handling for bit selections */
    bool hasBitSelect = false;

    /* Check if any port has bit selection */
    for (auto it = portWidthInfos.constBegin(); it != portWidthInfos.constEnd(); ++it) {
        const PortWidthInfo &info = it.value();
        if (!info.bitSelect.isEmpty()) {
            hasBitSelect = true;
            break;
        }
    }

    /* Special case: Handle the scenario where bit selections provide complete coverage.
     * Check if there's a common target width and bit selections provide full coverage. */
    if (hasBitSelect) {
        /* Find target width from ports WITHOUT bit selections (full-width connections) */
        int targetWidth = 0;
        for (auto it = portWidthInfos.constBegin(); it != portWidthInfos.constEnd(); ++it) {
            const PortWidthInfo &info = it.value();
            /* Only consider ports without bit selections as potential target width */
            if (info.bitSelect.isEmpty() && info.effectiveWidth > targetWidth) {
                targetWidth = info.effectiveWidth;
            }
        }

        /* Collect all bit selections */
        QStringList allBitSelections;
        for (auto it = portWidthInfos.constBegin(); it != portWidthInfos.constEnd(); ++it) {
            const PortWidthInfo &info = it.value();
            if (!info.bitSelect.isEmpty()) {
                allBitSelections.append(info.bitSelect);
            }
        }

        /* Check if bit selections provide complete coverage of the target width */
        if (targetWidth > 0 && !allBitSelections.isEmpty()) {
            bool fullCoverage = doBitRangesProvideFullCoverage(allBitSelections, targetWidth);
            if (fullCoverage) {
                /* Complete bit coverage found - no width mismatch */
                return true;
            }
        }
    }

    /* Default behavior: compare all effective widths for consistency */
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

    /* All effective widths are consistent */
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

        if (conn.type == PortType::CombSeqFsm) {
            /* Comb/seq/fsm outputs are always output drivers */
            direction = "output";
        } else if (conn.type == PortType::TopLevel) {
            /* For top-level ports, we need to reverse the direction for internal net perspective
             * e.g., a top-level output is an input from the internal net's perspective (receives from internal)
             * e.g., a top-level input is an output from the internal net's perspective (provides to internal) */
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

    /* Check for true conflicts:
     * - Multiple outputs (always a conflict)
     * - Output + inout (potential conflict)
     * Pure inout connections are normal (e.g., top-level inout to IO cell PAD)
     */
    if (outputCount > 1 || (outputCount > 0 && inoutCount > 0)) {
        /* Multiple output ports or output + inout - potential conflict */
        return PortDirectionStatus::Multidrive;
    }

    /* Normal cases:
     * - One output + multiple inputs
     * - Pure inout connections (bidirectional buses, IO connections)
     * - One inout + multiple inputs
     */
    return PortDirectionStatus::Valid;
}

QSocGenerateManager::PortDirectionStatus
QSocGenerateManager::checkPortDirectionConsistencyWithBitOverlap(
    const QList<PortDetailInfo> &portDetails)
{
    if (portDetails.isEmpty()) {
        return PortDirectionStatus::Valid;
    }

    /* Separate ports by direction */
    QList<PortDetailInfo> outputPorts;
    QList<PortDetailInfo> inputPorts;
    QList<PortDetailInfo> inoutPorts;

    for (const auto &port : portDetails) {
        QString direction = port.direction.toLower();

        /* Handle direction reversal for top-level ports */
        if (port.type == PortType::TopLevel) {
            /* CRITICAL FIX: Top-level port internal/external direction conversion
             *
             * The fundamental issue:
             * - Top-level ports are defined from EXTERNAL perspective (chip interface)
             * - Internal net checking requires INTERNAL perspective (inside the chip)
             * - These two perspectives have OPPOSITE directions for the same port
             *
             * Examples:
             * 1. jtag_tck (top-level OUTPUT):
             *    - External: "output" (chip outputs clock signal)
             *    - Internal: "input" (internal logic feeds the top-level port)
             *    - Check: internal module output + internal input = VALID (not multidriven)
             *
             * 2. jtag_tdo (top-level INPUT):
             *    - External: "input" (chip receives data signal)
             *    - Internal: "output" (top-level port feeds internal logic)
             *    - Check: internal input + internal output = VALID (not undriven)
             *
             * Without this conversion, we get false warnings:
             * - Top-level OUTPUT + Module OUTPUT = "multiple drivers" (WRONG)
             * - Top-level INPUT + Module INPUT = "undriven" (WRONG)
             */
            if (direction == "out" || direction == "output") {
                direction = "input"; /* Top-level output is an input for internal nets */
            } else if (direction == "in" || direction == "input") {
                direction = "output"; /* Top-level input is an output for internal nets */
            }
        }

        if (direction == "output") {
            outputPorts.append(port);
        } else if (direction == "input") {
            inputPorts.append(port);
        } else if (direction == "inout") {
            inoutPorts.append(port);
        }
    }

    /* Check for undriven nets */
    if (outputPorts.isEmpty() && inoutPorts.isEmpty()) {
        return PortDirectionStatus::Undriven;
    }

    /* Check for bit-level conflicts among output ports */
    if (outputPorts.size() > 1) {
        /* Check if any output ports have overlapping bit selections */
        for (int i = 0; i < outputPorts.size(); ++i) {
            for (int j = i + 1; j < outputPorts.size(); ++j) {
                const QString &bitSelect1 = outputPorts[i].bitSelect;
                const QString &bitSelect2 = outputPorts[j].bitSelect;

                /* If both ports have bit selections, check for overlap */
                if (!bitSelect1.isEmpty() && !bitSelect2.isEmpty()) {
                    if (doBitRangesOverlap(bitSelect1, bitSelect2)) {
                        return PortDirectionStatus::Multidrive;
                    }
                } else if (bitSelect1.isEmpty() && bitSelect2.isEmpty()) {
                    /* Both ports drive the entire net - definite conflict */
                    return PortDirectionStatus::Multidrive;
                } else {
                    /* One port has bit selection, other drives entire net - conflict */
                    return PortDirectionStatus::Multidrive;
                }
            }
        }
    }

    /* Check for conflicts between output and inout ports */
    if (!outputPorts.isEmpty() && !inoutPorts.isEmpty()) {
        /* Check if any output ports overlap with inout ports */
        for (const auto &outputPort : outputPorts) {
            for (const auto &inoutPort : inoutPorts) {
                const QString &outputBitSelect = outputPort.bitSelect;
                const QString &inoutBitSelect  = inoutPort.bitSelect;

                /* If both have bit selections, check for overlap */
                if (!outputBitSelect.isEmpty() && !inoutBitSelect.isEmpty()) {
                    if (doBitRangesOverlap(outputBitSelect, inoutBitSelect)) {
                        return PortDirectionStatus::Multidrive;
                    }
                } else if (outputBitSelect.isEmpty() && inoutBitSelect.isEmpty()) {
                    /* Both affect the entire net - conflict */
                    return PortDirectionStatus::Multidrive;
                } else {
                    /* One has bit selection, other affects entire net - conflict */
                    return PortDirectionStatus::Multidrive;
                }
            }
        }
    }

    /* No conflicts found */
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
            const auto        instanceName = instancePair.first.as<std::string>();
            const YAML::Node &instanceNode = instancePair.second;

            if (!instanceNode.IsMap() || !instanceNode["port"] || !instanceNode["port"].IsMap()) {
                continue;
            }

            /* Get module name */
            if (!instanceNode["module"] || !instanceNode["module"].IsScalar()) {
                continue;
            }
            const auto moduleName = instanceNode["module"].as<std::string>();

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
                const auto        portName = portPair.first.as<std::string>();
                const YAML::Node &portNode = portPair.second;

                /* Check for link attribute */
                if (portNode["link"] && portNode["link"].IsScalar()) {
                    const auto netName = portNode["link"].as<std::string>();

                    if (!processLinkConnection(
                            instanceName, portName, netName, moduleName, moduleData)) {
                        return false;
                    }
                }

                /* Check for uplink attribute */
                if (portNode["uplink"] && portNode["uplink"].IsScalar()) {
                    const auto netName = portNode["uplink"].as<std::string>();

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
    const std::string & /*moduleName*/,
    const YAML::Node & /*moduleData*/)
{
    try {
        qInfo() << "Processing link connection:" << instanceName.c_str() << "." << portName.c_str()
                << "-> link:" << netName.c_str();

        /* Parse the link value to extract net name and bit selection */
        const auto [cleanNetName, bitSelection] = parseLinkValue(netName);

        qInfo() << "Parsed link - net:" << cleanNetName.c_str()
                << (bitSelection.empty() ? "" : (", bits:" + bitSelection).c_str());

        /* Step 1: Check if net exists, create if not */
        if (!netlistData["net"][cleanNetName]) {
            netlistData["net"][cleanNetName] = YAML::Node(YAML::NodeType::Sequence);
            qInfo() << "Created new net:" << cleanNetName.c_str();
        }

        /* Step 2: Check if net is empty or has existing connections */
        YAML::Node netNode = netlistData["net"][cleanNetName];

        /* Step 3: Check for duplicate connections */
        bool isDuplicate = false;

        /* Check existing connections in List format */
        if (netNode.size() > 0 && netNode.IsSequence()) {
            for (const auto &connection : netNode) {
                if (connection.IsMap() && connection["instance"]
                    && connection["instance"].IsScalar() && connection["port"]
                    && connection["port"].IsScalar()) {
                    const std::string existingInstance = connection["instance"].as<std::string>();
                    const std::string existingPort     = connection["port"].as<std::string>();

                    if (existingInstance == instanceName && existingPort == portName) {
                        /* Same instance and port - check bit selection */
                        std::string existingBits = "";
                        if (connection["bits"] && connection["bits"].IsScalar()) {
                            existingBits = connection["bits"].as<std::string>();
                        }

                        if (existingBits == bitSelection) {
                            /* Exact duplicate - ignore */
                            isDuplicate = true;
                            qInfo()
                                << "Ignoring duplicate connection:" << instanceName.c_str() << "."
                                << portName.c_str() << " to net:" << cleanNetName.c_str();
                            break;
                        }
                    }
                }
            }
        }

        /* Step 4: Add connection only if not duplicate */
        if (!isDuplicate) {
            /* Always use List format for consistency */
            YAML::Node connectionNode  = YAML::Node(YAML::NodeType::Map);
            connectionNode["instance"] = instanceName;
            connectionNode["port"]     = portName;

            /* Add bit selection if present */
            if (!bitSelection.empty()) {
                connectionNode["bits"] = bitSelection;
            }

            /* Ensure the net node is a sequence (list) */
            if (!netlistData["net"][cleanNetName]
                || !netlistData["net"][cleanNetName].IsSequence()) {
                netlistData["net"][cleanNetName] = YAML::Node(YAML::NodeType::Sequence);
            }

            /* Add to the net's connection list */
            netlistData["net"][cleanNetName].push_back(connectionNode);

            qInfo() << "Added connection:" << instanceName.c_str() << "." << portName.c_str()
                    << " to net:" << cleanNetName.c_str()
                    << (bitSelection.empty() ? "" : (" with bits:" + bitSelection).c_str());
        }

        qInfo() << "Successfully created link connection for net:" << cleanNetName.c_str();
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
                << portName.c_str() << "-> top-level port:" << netName.c_str();

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
        auto modulePortDirection = modulePortNode["direction"].as<std::string>();

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

        /* Calculate top-level port direction (same as module port) */
        std::string topLevelDirection;
        if (modulePortDirection == "input" || modulePortDirection == "in") {
            topLevelDirection = "input";
        } else if (modulePortDirection == "output" || modulePortDirection == "out") {
            topLevelDirection = "output";
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

            const bool directionCompatible
                = (topLevelDirection == "inout" || existingDirection == "inout"
                   || topLevelDirection == existingDirection);

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
                const int moduleWidth   = calculatePortWidth(modulePortType);
                const int existingWidth = calculatePortWidth(existingType);

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
            topLevelPortNode["connect"]   = netName; /* Add connect attribute to link port to net */

            netlistData["port"][netName] = topLevelPortNode;

            qInfo() << "Created new top-level port:" << netName.c_str()
                    << ", direction:" << topLevelDirection.c_str()
                    << ", type:" << modulePortType.c_str()
                    << ", connected to net:" << netName.c_str();
        }

        /* For uplink, directly connect module port to top-level port - NO intermediate net */
        /* Find or create the net for this top-level port */
        if (!netlistData["net"][netName] || !netlistData["net"][netName].IsSequence()) {
            netlistData["net"][netName] = YAML::Node(YAML::NodeType::Sequence);
        }

        /* Connect module instance port directly to the top-level port via the net */
        YAML::Node connectionNode  = YAML::Node(YAML::NodeType::Map);
        connectionNode["instance"] = instanceName;
        connectionNode["port"]     = portName;
        netlistData["net"][netName].push_back(connectionNode);

        /* The top-level port is implicitly connected to the net with the same name */
        /* We don't add an explicit "top_level" connection since the net name matches the port name */

        qInfo() << "Successfully created uplink connection for port:" << netName.c_str();
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
 * @brief Parse link value to extract net name and bit selection
 * @param linkValue The link value (e.g., "bus_data[7:0]", "clk_signal[3]", "simple_net")
 * @return A pair containing the net name and bit selection (empty if no selection)
 */
std::pair<std::string, std::string> QSocGenerateManager::parseLinkValue(const std::string &linkValue)
{
    const QString linkStr = QString::fromStdString(linkValue);

    /* Regex to match net_name[bit_selection] format */
    const QRegularExpression      linkRegex(R"(^([^[]+)\s*(\[[^\]]+\])?\s*$)");
    const QRegularExpressionMatch match = linkRegex.match(linkStr);

    if (match.hasMatch()) {
        const QString netName      = match.captured(1).trimmed();
        const QString bitSelection = match.captured(2).trimmed();

        return std::make_pair(netName.toStdString(), bitSelection.toStdString());
    }

    /* If regex doesn't match, treat entire string as net name */
    return std::make_pair(linkValue, std::string());
}

/**
 * @brief Calculate the width of a port from its type string
 * @param portType The port type string (e.g., "wire [7:0]", "reg [15:0]", "wire")
 * @return The width in bits, or -1 if cannot be determined
 */
int QSocGenerateManager::calculatePortWidth(const std::string &portType)
{
    const QString type = QString::fromStdString(portType);

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

/**
 * @brief Process combinational logic section in the netlist
 * @return true if successful, false on error
 */
bool QSocGenerateManager::processCombLogic()
{
    try {
        /* Check if comb section exists */
        if (!netlistData["comb"]) {
            qInfo() << "No combinational logic section found, skipping";
            return true;
        }

        if (!netlistData["comb"].IsSequence()) {
            qCritical() << "Error: 'comb' section must be a sequence/list";
            return false;
        }

        qInfo() << "Processing combinational logic section...";

        /* Iterate through each combinational logic item */
        for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
            const YAML::Node &combItem = netlistData["comb"][i];

            if (!combItem.IsMap()) {
                qWarning() << "Warning: Combinational logic item" << i << "is not a map, skipping";
                continue;
            }

            /* Validate that 'out' field exists */
            if (!combItem["out"] || !combItem["out"].IsScalar()) {
                qWarning() << "Warning: Combinational logic item" << i
                           << "missing required 'out' field, skipping";
                continue;
            }

            const QString outputSignal = QString::fromStdString(combItem["out"].as<std::string>());

            /* Check for conflicts between different logic types */
            int logicTypeCount = 0;
            if (combItem["expr"])
                logicTypeCount++;
            if (combItem["if"])
                logicTypeCount++;
            if (combItem["case"])
                logicTypeCount++;

            if (logicTypeCount == 0) {
                qWarning() << "Warning: Combinational logic item" << i << "for output"
                           << outputSignal
                           << "has no logic specification (expr, if, or case), skipping";
                continue;
            }

            if (logicTypeCount > 1) {
                qWarning() << "Warning: Combinational logic item" << i << "for output"
                           << outputSignal
                           << "has multiple logic types specified, using first found";
            }

            /* Validate logic types */
            if (combItem["expr"]) {
                /* Simple assignment - validate expr field */
                if (!combItem["expr"].IsScalar()) {
                    qWarning() << "Warning: 'expr' field for output" << outputSignal
                               << "is not a scalar, skipping";
                    continue;
                }
            } else if (combItem["if"]) {
                /* Conditional logic - validate if field */
                if (!combItem["if"].IsSequence()) {
                    qWarning() << "Warning: 'if' field for output" << outputSignal
                               << "is not a sequence, skipping";
                    continue;
                }

                /* Validate each if condition */
                bool validIfBlock = true;
                for (const auto &ifCondition : combItem["if"]) {
                    if (!ifCondition.IsMap() || !ifCondition["cond"] || !ifCondition["then"]) {
                        qWarning() << "Warning: Invalid if condition for output" << outputSignal
                                   << ", each condition must have 'cond' and 'then' fields";
                        validIfBlock = false;
                        break;
                    }

                    if (!ifCondition["cond"].IsScalar()) {
                        qWarning()
                            << "Warning: 'cond' field must be a scalar for output" << outputSignal;
                        validIfBlock = false;
                        break;
                    }

                    /* 'then' can be either scalar (simple value) or map (nested structure) */
                    if (!ifCondition["then"].IsScalar() && !ifCondition["then"].IsMap()) {
                        qWarning() << "Warning: 'then' field must be scalar or map for output"
                                   << outputSignal;
                        validIfBlock = false;
                        break;
                    }

                    /* If 'then' is a map, validate nested case structure */
                    if (ifCondition["then"].IsMap()) {
                        const YAML::Node &thenNode = ifCondition["then"];

                        /* Check if it's a nested case structure */
                        if (thenNode["case"]) {
                            if (!thenNode["case"].IsScalar()) {
                                qWarning()
                                    << "Warning: Nested 'case' field must be scalar for output"
                                    << outputSignal;
                                validIfBlock = false;
                                break;
                            }

                            if (!thenNode["cases"] || !thenNode["cases"].IsMap()) {
                                qWarning() << "Warning: Nested 'cases' field missing or not a map "
                                              "for output"
                                           << outputSignal;
                                validIfBlock = false;
                                break;
                            }

                            /* Validate nested case entries */
                            for (const auto &caseEntry : thenNode["cases"]) {
                                if (!caseEntry.first.IsScalar() || !caseEntry.second.IsScalar()) {
                                    qWarning() << "Warning: Nested case entries must have scalar "
                                                  "keys and values for output"
                                               << outputSignal;
                                    validIfBlock = false;
                                    break;
                                }
                            }
                        } else {
                            qWarning() << "Warning: Nested structure in 'then' field not supported "
                                          "for output"
                                       << outputSignal;
                            validIfBlock = false;
                            break;
                        }
                    }
                }

                if (!validIfBlock) {
                    continue;
                }

                /* Check for default value */
                if (!combItem["default"]) {
                    qWarning() << "Warning: Missing 'default' field for conditional logic output"
                               << outputSignal << ", may cause latches";
                }
            } else if (combItem["case"]) {
                /* Case statement - validate case and cases fields */
                if (!combItem["case"].IsScalar()) {
                    qWarning() << "Warning: 'case' field for output" << outputSignal
                               << "is not a scalar, skipping";
                    continue;
                }

                if (!combItem["cases"] || !combItem["cases"].IsMap()) {
                    qWarning() << "Warning: 'cases' field for output" << outputSignal
                               << "is missing or not a map, skipping";
                    continue;
                }

                /* Validate case entries */
                bool validCaseBlock = true;
                for (const auto &caseEntry : combItem["cases"]) {
                    if (!caseEntry.first.IsScalar() || !caseEntry.second.IsScalar()) {
                        qWarning()
                            << "Warning: Case entries must have scalar keys and values for output"
                            << outputSignal;
                        validCaseBlock = false;
                        break;
                    }
                }

                if (!validCaseBlock) {
                    continue;
                }

                /* Check for default value */
                if (!combItem["default"]) {
                    qWarning() << "Warning: Missing 'default' field for case statement output"
                               << outputSignal << ", may cause latches";
                }
            }

            qInfo() << "Validated combinational logic item" << i << "for output" << outputSignal;
        }

        qInfo() << "Successfully processed combinational logic section";
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processCombLogic:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processCombLogic:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in processCombLogic";
        return false;
    }
}

bool QSocGenerateManager::processSeqLogic()
{
    try {
        if (!netlistData["seq"]) {
            qInfo() << "No sequential logic section found, skipping";
            return true;
        }

        if (!netlistData["seq"].IsSequence()) {
            qCritical() << "Error: 'seq' section must be a sequence";
            return false;
        }

        qInfo() << "Processing sequential logic section...";

        for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
            const YAML::Node &seqItem = netlistData["seq"][i];

            /* Validate that each item is a map */
            if (!seqItem.IsMap()) {
                qWarning() << "Warning: Sequential logic item" << i << "is not a map, skipping";
                continue;
            }

            /* Validate required fields */
            if (!seqItem["reg"]) {
                qWarning() << "Warning: Sequential logic item" << i
                           << "has no 'reg' field, skipping";
                continue;
            }

            if (!seqItem["clk"]) {
                qWarning() << "Warning: Sequential logic item" << i
                           << "has no 'clk' field, skipping";
                continue;
            }

            /* Check that reg and clk are scalar values */
            if (!seqItem["reg"].IsScalar()) {
                qWarning() << "Warning: 'reg' field must be a scalar for item" << i;
                continue;
            }

            if (!seqItem["clk"].IsScalar()) {
                qWarning() << "Warning: 'clk' field must be a scalar for item" << i;
                continue;
            }

            const QString regName = QString::fromStdString(seqItem["reg"].as<std::string>());

            /* Validate edge type (if present) */
            if (seqItem["edge"]) {
                if (!seqItem["edge"].IsScalar()) {
                    qWarning() << "Warning: 'edge' field must be a scalar for register" << regName;
                    continue;
                }
                const QString edge = QString::fromStdString(seqItem["edge"].as<std::string>());
                if (edge != "pos" && edge != "neg") {
                    qWarning() << "Warning: 'edge' field must be 'pos' or 'neg' for register"
                               << regName << ", got:" << edge;
                    continue;
                }
            }

            /* Validate reset fields (if present) */
            if (seqItem["rst"]) {
                if (!seqItem["rst"].IsScalar()) {
                    qWarning() << "Warning: 'rst' field must be a scalar for register" << regName;
                    continue;
                }

                /* Reset value is required when reset is present */
                if (!seqItem["rst_val"]) {
                    qWarning()
                        << "Warning: 'rst_val' is required when 'rst' is present for register"
                        << regName;
                    continue;
                }

                if (!seqItem["rst_val"].IsScalar()) {
                    qWarning() << "Warning: 'rst_val' field must be a scalar for register"
                               << regName;
                    continue;
                }
            }

            /* Validate enable field (if present) */
            if (seqItem["enable"] && !seqItem["enable"].IsScalar()) {
                qWarning() << "Warning: 'enable' field must be a scalar for register" << regName;
                continue;
            }

            /* Validate logic specification: must have either 'next' or 'if' */
            bool hasNext = seqItem["next"] && seqItem["next"].IsScalar();
            bool hasIf   = seqItem["if"] && seqItem["if"].IsSequence();

            if (!hasNext && !hasIf) {
                qWarning() << "Warning: Register" << regName
                           << "has no logic specification ('next' or 'if'), skipping";
                continue;
            }

            if (hasNext && hasIf) {
                qWarning() << "Warning: Register" << regName
                           << "has both 'next' and 'if' specifications, skipping";
                continue;
            }

            /* Validate 'if' logic structure */
            if (hasIf) {
                bool validIfBlock = true;
                for (const auto &ifEntry : seqItem["if"]) {
                    if (!ifEntry.IsMap() || !ifEntry["cond"] || !ifEntry["then"]) {
                        qWarning() << "Warning: 'if' entries must have 'cond' and 'then' fields "
                                      "for register"
                                   << regName;
                        validIfBlock = false;
                        break;
                    }

                    if (!ifEntry["cond"].IsScalar() || !ifEntry["then"].IsScalar()) {
                        qWarning()
                            << "Warning: 'cond' and 'then' fields must be scalars for register"
                            << regName;
                        validIfBlock = false;
                        break;
                    }
                }

                if (!validIfBlock) {
                    continue;
                }

                /* Check for default value */
                if (!seqItem["default"]) {
                    qWarning() << "Warning: Missing 'default' field for 'if' logic register"
                               << regName << ", may cause latches";
                }
            }

            qInfo() << "Validated sequential logic item" << i << "for register" << regName;
        }

        qInfo() << "Successfully processed sequential logic section";
        return true;
    } catch (const YAML::Exception &e) {
        qCritical() << "YAML exception in processSeqLogic:" << e.what();
        return false;
    } catch (const std::exception &e) {
        qCritical() << "Standard exception in processSeqLogic:" << e.what();
        return false;
    } catch (...) {
        qCritical() << "Unknown exception in processSeqLogic";
        return false;
    }
}

QPair<QString, QString> QSocGenerateManager::parseSignalBitSelect(const QString &signalName)
{
    const QRegularExpression      bitSelectRegex(R"(^([^[]+)(\[\s*\d+\s*(?::\s*\d+)?\s*\])?\s*$)");
    const QRegularExpressionMatch match = bitSelectRegex.match(signalName);

    if (match.hasMatch()) {
        QString baseName  = match.captured(1).trimmed();
        QString bitSelect = match.captured(2);
        return qMakePair(baseName, bitSelect);
    }

    return qMakePair(signalName, QString());
}

QList<QSocGenerateManager::PortDetailInfo> QSocGenerateManager::collectCombSeqFsmSignals()
{
    QList<PortDetailInfo> signalList;
    QSlangDriver          driver; // Create once for all signal extractions

    try {
        // Collect comb outputs and inputs
        if (netlistData["comb"] && netlistData["comb"].IsSequence()) {
            for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
                const YAML::Node &combItem = netlistData["comb"][i];
                if (combItem.IsMap() && combItem["out"] && combItem["out"].IsScalar()) {
                    const QString outputSignal = QString::fromStdString(
                        combItem["out"].as<std::string>());

                    auto    parsed    = parseSignalBitSelect(outputSignal);
                    QString baseName  = parsed.first;
                    QString bitSelect = parsed.second;

                    // Check if bits attribute exists and override bitSelect if present
                    if (combItem["bits"] && combItem["bits"].IsScalar()) {
                        bitSelect = QString::fromStdString(combItem["bits"].as<std::string>());
                        qDebug() << "Using bits attribute for comb output:" << baseName
                                 << "bits:" << bitSelect;
                    }

                    // Find port width for this output
                    QString width = "";
                    if (netlistData["port"] && netlistData["port"].IsMap()) {
                        for (const auto &portEntry : netlistData["port"]) {
                            if (portEntry.first.IsScalar()
                                && QString::fromStdString(portEntry.first.as<std::string>())
                                       == baseName) {
                                if (portEntry.second.IsMap() && portEntry.second["type"]
                                    && portEntry.second["type"].IsScalar()) {
                                    QString portType = QString::fromStdString(
                                        portEntry.second["type"].as<std::string>());
                                    width = cleanTypeForWireDeclaration(portType);
                                    break;
                                }
                            }
                        }
                    }

                    signalList.append(
                        PortDetailInfo::createCombSeqFsmPort(baseName, width, "output", bitSelect));

                    /* Extract input signals from expr field */
                    if (combItem["expr"] && combItem["expr"].IsScalar()) {
                        const QString expr = QString::fromStdString(
                            combItem["expr"].as<std::string>());

                        /* Wrap as assignment statement: assign <out> = <expr>; */
                        QString wrappedExpr = QString("assign %1 = %2;").arg(baseName, expr);

                        /* Use slang to parse and extract input signals */
                        if (driver.parseVerilogSnippet(wrappedExpr, true)) {
                            /* Extract all signals referenced in the expression (excluding the output) */
                            QSet<QString> inputSignals = driver.extractSignalReferences({baseName});

                            /* Add each input signal to the list */
                            for (const QString &inputSignal : inputSignals) {
                                /* Find port width for this input */
                                QString inputWidth = "";
                                if (netlistData["port"] && netlistData["port"].IsMap()) {
                                    for (const auto &portEntry : netlistData["port"]) {
                                        if (portEntry.first.IsScalar()
                                            && QString::fromStdString(
                                                   portEntry.first.as<std::string>())
                                                   == inputSignal) {
                                            if (portEntry.second.IsMap() && portEntry.second["type"]
                                                && portEntry.second["type"].IsScalar()) {
                                                QString portType = QString::fromStdString(
                                                    portEntry.second["type"].as<std::string>());
                                                inputWidth = cleanTypeForWireDeclaration(portType);
                                                break;
                                            }
                                        }
                                    }
                                }

                                /* Add input signal with direction="input" */
                                signalList.append(
                                    PortDetailInfo::createCombSeqFsmPort(
                                        inputSignal, inputWidth, "input", ""));
                            }
                        } else {
                            /* Graceful degradation: log warning but continue */
                            QStaticLog::logW(
                                Q_FUNC_INFO,
                                "Failed to parse comb expr for input extraction: " + expr);
                        }
                    }
                }
            }
        }

        // Collect seq outputs
        if (netlistData["seq"] && netlistData["seq"].IsSequence()) {
            for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
                const YAML::Node &seqItem = netlistData["seq"][i];
                if (seqItem.IsMap() && seqItem["reg"] && seqItem["reg"].IsScalar()) {
                    const QString regName = QString::fromStdString(seqItem["reg"].as<std::string>());

                    auto    parsed    = parseSignalBitSelect(regName);
                    QString baseName  = parsed.first;
                    QString bitSelect = parsed.second;

                    // Check if bits attribute exists and override bitSelect if present
                    if (seqItem["bits"] && seqItem["bits"].IsScalar()) {
                        bitSelect = QString::fromStdString(seqItem["bits"].as<std::string>());
                        qDebug() << "Using bits attribute for seq output:" << baseName
                                 << "bits:" << bitSelect;
                    }

                    // Find port width for this output
                    QString width = "";
                    if (netlistData["port"] && netlistData["port"].IsMap()) {
                        for (const auto &portEntry : netlistData["port"]) {
                            if (portEntry.first.IsScalar()
                                && QString::fromStdString(portEntry.first.as<std::string>())
                                       == baseName) {
                                if (portEntry.second.IsMap() && portEntry.second["type"]
                                    && portEntry.second["type"].IsScalar()) {
                                    QString portType = QString::fromStdString(
                                        portEntry.second["type"].as<std::string>());
                                    width = cleanTypeForWireDeclaration(portType);
                                    break;
                                }
                            }
                        }
                    }

                    signalList.append(
                        PortDetailInfo::createCombSeqFsmPort(baseName, width, "output", bitSelect));

                    /* Extract input signals from next field */
                    if (seqItem["next"] && seqItem["next"].IsScalar()) {
                        const QString nextExpr = QString::fromStdString(
                            seqItem["next"].as<std::string>());

                        /* Wrap as assignment statement: assign <out> = <next_expr>; */
                        QString wrappedNextExpr = QString("assign %1 = %2;").arg(baseName, nextExpr);

                        /* Use slang to parse and extract input signals */
                        if (driver.parseVerilogSnippet(wrappedNextExpr, true)) {
                            /* Extract all signals referenced in next expression (excluding the output reg) */
                            QSet<QString> inputSignals = driver.extractSignalReferences({baseName});

                            /* Add each input signal to the list */
                            for (const QString &inputSignal : inputSignals) {
                                /* Find port width for this input */
                                QString inputWidth = "";
                                if (netlistData["port"] && netlistData["port"].IsMap()) {
                                    for (const auto &portEntry : netlistData["port"]) {
                                        if (portEntry.first.IsScalar()
                                            && QString::fromStdString(
                                                   portEntry.first.as<std::string>())
                                                   == inputSignal) {
                                            if (portEntry.second.IsMap() && portEntry.second["type"]
                                                && portEntry.second["type"].IsScalar()) {
                                                QString portType = QString::fromStdString(
                                                    portEntry.second["type"].as<std::string>());
                                                inputWidth = cleanTypeForWireDeclaration(portType);
                                                break;
                                            }
                                        }
                                    }
                                }

                                /* Add input signal with direction="input" */
                                signalList.append(
                                    PortDetailInfo::createCombSeqFsmPort(
                                        inputSignal, inputWidth, "input", ""));
                            }
                        } else {
                            /* Graceful degradation: log warning but continue */
                            QStaticLog::logW(
                                Q_FUNC_INFO,
                                "Failed to parse seq next expr for input extraction: " + nextExpr);
                        }
                    }

                    /* Extract clock signal */
                    if (seqItem["clk"] && seqItem["clk"].IsScalar()) {
                        const QString clkSignal = QString::fromStdString(
                            seqItem["clk"].as<std::string>());
                        signalList.append(
                            PortDetailInfo::createCombSeqFsmPort(clkSignal, "", "input", ""));
                    }

                    /* Extract reset signal */
                    if (seqItem["rst"] && seqItem["rst"].IsScalar()) {
                        const QString rstSignal = QString::fromStdString(
                            seqItem["rst"].as<std::string>());
                        signalList.append(
                            PortDetailInfo::createCombSeqFsmPort(rstSignal, "", "input", ""));
                    }
                }
            }
        }

        // Collect FSM outputs
        if (netlistData["fsm"] && netlistData["fsm"].IsSequence()) {
            for (size_t i = 0; i < netlistData["fsm"].size(); ++i) {
                const YAML::Node &fsmItem = netlistData["fsm"][i];
                if (fsmItem.IsMap() && fsmItem["moore"] && fsmItem["moore"].IsMap()) {
                    QSet<QString> fsmOutputs;

                    // Collect all Moore outputs
                    for (const auto &mooreEntry : fsmItem["moore"]) {
                        if (mooreEntry.second.IsMap()) {
                            for (const auto &output : mooreEntry.second) {
                                if (output.first.IsScalar()) {
                                    const QString outputName = QString::fromStdString(
                                        output.first.as<std::string>());
                                    fsmOutputs.insert(outputName);
                                }
                            }
                        }
                    }

                    // Add each unique FSM output
                    for (const QString &outputSignal : fsmOutputs) {
                        auto    parsed    = parseSignalBitSelect(outputSignal);
                        QString baseName  = parsed.first;
                        QString bitSelect = parsed.second;

                        // Find port width for this output
                        QString width = "";
                        if (netlistData["port"] && netlistData["port"].IsMap()) {
                            for (const auto &portEntry : netlistData["port"]) {
                                if (portEntry.first.IsScalar()
                                    && QString::fromStdString(portEntry.first.as<std::string>())
                                           == baseName) {
                                    if (portEntry.second.IsMap() && portEntry.second["type"]
                                        && portEntry.second["type"].IsScalar()) {
                                        QString portType = QString::fromStdString(
                                            portEntry.second["type"].as<std::string>());
                                        width = cleanTypeForWireDeclaration(portType);
                                        break;
                                    }
                                }
                            }
                        }

                        signalList.append(
                            PortDetailInfo::createTopLevelPort(baseName, width, "output", bitSelect));
                    }
                }
            }
        }

    } catch (const YAML::Exception &e) {
        qWarning() << "YAML exception in collectCombSeqFsmSignals:" << e.what();
    } catch (const std::exception &e) {
        qWarning() << "Standard exception in collectCombSeqFsmSignals:" << e.what();
    } catch (...) {
        qWarning() << "Unknown exception in collectCombSeqFsmSignals";
    }

    return signalList;
}

bool QSocGenerateManager::doBitRangesOverlap(const QString &range1, const QString &range2)
{
    // If either range is empty, it means the port uses the entire signal
    // An empty range overlaps with any other range (empty or specific)
    if (range1.isEmpty() || range2.isEmpty()) {
        return true;
    }

    const QRegularExpression bitSelectRegex(R"(\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])");

    // Parse range1
    const QRegularExpressionMatch match1 = bitSelectRegex.match(range1);
    if (!match1.hasMatch()) {
        return false;
    }

    bool ok1;
    int  msb1 = match1.captured(1).toInt(&ok1);
    if (!ok1) {
        return false;
    }

    int lsb1 = msb1; // Default to single bit
    if (match1.capturedLength(2) > 0) {
        lsb1 = match1.captured(2).toInt(&ok1);
        if (!ok1) {
            return false;
        }
    }

    // Ensure msb >= lsb for range1
    if (msb1 < lsb1) {
        qSwap(msb1, lsb1);
    }

    // Parse range2
    const QRegularExpressionMatch match2 = bitSelectRegex.match(range2);
    if (!match2.hasMatch()) {
        return false;
    }

    bool ok2;
    int  msb2 = match2.captured(1).toInt(&ok2);
    if (!ok2) {
        return false;
    }

    int lsb2 = msb2; // Default to single bit
    if (match2.capturedLength(2) > 0) {
        lsb2 = match2.captured(2).toInt(&ok2);
        if (!ok2) {
            return false;
        }
    }

    // Ensure msb >= lsb for range2
    if (msb2 < lsb2) {
        qSwap(msb2, lsb2);
    }

    // Check for overlap: ranges [a:b] and [c:d] overlap if they have any common bits
    // Two ranges DON'T overlap if one range ends before the other starts
    int min1 = qMin(msb1, lsb1);
    int max1 = qMax(msb1, lsb1);
    int min2 = qMin(msb2, lsb2);
    int max2 = qMax(msb2, lsb2);

    // No overlap if range1 ends before range2 starts, or range2 ends before range1 starts
    return !(max1 < min2 || max2 < min1);
}

bool QSocGenerateManager::doBitRangesProvideFullCoverage(const QStringList &ranges, int signalWidth)
{
    if (ranges.isEmpty()) {
        return false;
    }

    // Convert signal width to bit range (e.g., 8 -> [7:0])
    int expectedMsb = (signalWidth <= 1) ? 0 : signalWidth - 1;
    int expectedLsb = 0;

    QVector<bool> coverage(expectedMsb - expectedLsb + 1, false);

    const QRegularExpression bitSelectRegex(R"(\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])");

    for (const QString &range : ranges) {
        if (range.isEmpty()) {
            // Empty range covers full signal if signal is single bit
            if (signalWidth <= 1) {
                coverage[0] = true;
            }
            continue;
        }

        const QRegularExpressionMatch match = bitSelectRegex.match(range);
        if (!match.hasMatch()) {
            continue;
        }

        bool ok;
        int  msb = match.captured(1).toInt(&ok);
        if (!ok) {
            continue;
        }

        int lsb = msb; // Default to single bit
        if (match.capturedLength(2) > 0) {
            lsb = match.captured(2).toInt(&ok);
            if (!ok) {
                continue;
            }
        }

        // Ensure msb >= lsb
        if (msb < lsb) {
            qSwap(msb, lsb);
        }

        // Mark coverage for this range
        for (int bit = lsb; bit <= msb; ++bit) {
            if (bit >= expectedLsb && bit <= expectedMsb) {
                coverage[bit - expectedLsb] = true;
            }
        }
    }

    // Check if all bits are covered
    for (bool covered : coverage) {
        if (!covered) {
            return false;
        }
    }

    return true;
}
