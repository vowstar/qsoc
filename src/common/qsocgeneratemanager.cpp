#include "common/qsocgeneratemanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <fstream>
#include <iostream>

QSoCGenerateManager::QSoCGenerateManager(
    QObject            *parent,
    QSocProjectManager *projectManager,
    QSocModuleManager  *moduleManager,
    QSocBusManager     *busManager,
    QLLMService        *llmService)
    : QObject(parent)
{
    /* Set projectManager */
    setProjectManager(projectManager);
    /* Set moduleManager */
    setModuleManager(moduleManager);
    /* Set busManager */
    setBusManager(busManager);
    /* Set llmService */
    setLLMService(llmService);
}

void QSoCGenerateManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

void QSoCGenerateManager::setModuleManager(QSocModuleManager *moduleManager)
{
    /* Set moduleManager */
    if (moduleManager) {
        this->moduleManager = moduleManager;
    }
}

void QSoCGenerateManager::setBusManager(QSocBusManager *busManager)
{
    /* Set busManager */
    if (busManager) {
        this->busManager = busManager;
    }
}

void QSoCGenerateManager::setLLMService(QLLMService *llmService)
{
    /* Set llmService */
    if (llmService) {
        this->llmService = llmService;
    }
}

QSocProjectManager *QSoCGenerateManager::getProjectManager()
{
    return projectManager;
}

QSocModuleManager *QSoCGenerateManager::getModuleManager()
{
    return moduleManager;
}

QSocBusManager *QSoCGenerateManager::getBusManager()
{
    return busManager;
}

QLLMService *QSoCGenerateManager::getLLMService()
{
    return llmService;
}

bool QSoCGenerateManager::loadNetlist(const QString &netlistFilePath)
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

bool QSoCGenerateManager::processNetlist()
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

        /* Skip if no bus section */
        if (!netlistData["bus"] || !netlistData["bus"].IsMap() || netlistData["bus"].size() == 0) {
            qInfo() << "No bus section found or empty, skipping bus processing";
            return true;
        }

        /* Process each bus type (e.g., biu_bus) */
        for (const auto &busTypePair : netlistData["bus"]) {
            try {
                /* Get bus type name */
                if (!busTypePair.first.IsScalar()) {
                    qWarning() << "Warning: Bus type name is not a scalar, skipping";
                    continue;
                }
                const std::string busTypeName = busTypePair.first.as<std::string>();
                qInfo() << "Processing bus:" << busTypeName.c_str();

                /* Get bus connections (should be a map) */
                if (!busTypePair.second.IsMap()) {
                    qWarning() << "Warning: Bus" << busTypeName.c_str() << "is not a map, skipping";
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
                        const std::string instanceName = connectionPair.first.as<std::string>();

                        if (!connectionPair.second.IsMap() || !connectionPair.second["port"]
                            || !connectionPair.second["port"].IsScalar()) {
                            qWarning() << "Warning: Invalid port specification for instance"
                                       << instanceName.c_str();
                            continue;
                        }
                        const std::string portName = connectionPair.second["port"].as<std::string>();

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
                            qWarning()
                                << "Warning: Invalid module for instance" << instanceName.c_str();
                            continue;
                        }

                        const std::string moduleName
                            = netlistData["instance"][instanceName]["module"].as<std::string>();

                        /* Check if module exists */
                        if (!moduleManager
                            || !moduleManager->isModuleExist(QString::fromStdString(moduleName))) {
                            qWarning() << "Warning: Module" << moduleName.c_str() << "not found";
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
                            qWarning() << "Warning: No bus section in module" << moduleName.c_str();
                            continue;
                        }

                        /* Try exact port name */
                        bool portFound = false;
                        if (moduleData["bus"][portName]) {
                            portFound = true;
                        }
                        /* Try with pad_ prefix if not found */
                        else if (
                            portName.compare(0, 4, "pad_") == 0
                            && moduleData["bus"][portName.substr(4)]) {
                            portFound = true;
                        }
                        /* Try adding pad_ prefix */
                        else if (moduleData["bus"]["pad_" + portName]) {
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
                            currentBusType = moduleData["bus"][portName]["bus"].as<std::string>();
                        } else if (
                            portName.compare(0, 4, "pad_") == 0
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
                            qWarning()
                                << "Warning: Mixed bus types" << busType.c_str() << "and"
                                << currentBusType.c_str() << ", skipping inconsistent connection";
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
                    qWarning() << "Warning: No valid connections for bus" << busTypeName.c_str();
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

                qInfo() << "Processing" << busDefinition["port"].size() << "signals for bus type"
                        << busType.c_str();

                /* Step 3: Create nets for each bus signal */
                for (const auto &portPair : busDefinition["port"]) {
                    if (!portPair.first.IsScalar()) {
                        qWarning() << "Warning: Invalid port name in bus definition, skipping";
                        continue;
                    }

                    const std::string signalName = portPair.first.as<std::string>();
                    const std::string netName    = busTypeName + "_" + signalName;

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
                                conn.portName.compare(0, 4, "pad_") == 0
                                && moduleData["bus"][conn.portName.substr(4)]
                                && moduleData["bus"][conn.portName.substr(4)]["mapping"]
                                && moduleData["bus"][conn.portName.substr(4)]["mapping"].IsMap()
                                && moduleData["bus"][conn.portName.substr(4)]["mapping"][signalName]
                                && moduleData["bus"][conn.portName.substr(4)]["mapping"][signalName]
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
                                && moduleData["bus"]["pad_" + conn.portName]["mapping"][signalName]
                                && moduleData["bus"]["pad_" + conn.portName]["mapping"][signalName]
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
                            qWarning() << "YAML exception adding connection to net:" << e.what();
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

        /* Clean up by removing the bus section */
        netlistData.remove("bus");

        qInfo() << "Netlist processed successfully";
        std::cout << "Expanded Netlist:\n" << netlistData << std::endl;
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

int QSoCGenerateManager::getPortWidth(const YAML::Node &portData)
{
    /* Default width is 1 if no width information is available */
    int width = 1;

    if (portData["type"] && portData["type"].IsScalar()) {
        QString typeString = QString::fromStdString(portData["type"].as<std::string>());

        /* Extract width information from type string (e.g. "logic[39:0]") */
        QRegularExpression widthRegex("\\[(\\d+):(\\d+)\\]");
        if (widthRegex.match(typeString).hasMatch()) {
            int msb = widthRegex.match(typeString).captured(1).toInt();
            int lsb = widthRegex.match(typeString).captured(2).toInt();
            width   = qAbs(msb - lsb) + 1;
        }
    }

    return width;
}

bool QSoCGenerateManager::checkPortWidthConsistency(const QList<QPair<QString, QString>> &connections)
{
    if (connections.isEmpty()) {
        return true; /* No connections to check */
    }

    int expectedWidth = -1;

    /* Check each connection's port width */
    for (const auto &conn : connections) {
        const QString &instanceName = conn.first;
        const QString &portName     = conn.second;

        /* Get instance's module name */
        if (!netlistData["instance"][instanceName.toStdString()]
            || !netlistData["instance"][instanceName.toStdString()]["module"]
            || !netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
            continue; /* Skip invalid instance */
        }

        const QString moduleName = QString::fromStdString(
            netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

        /* Get module definition */
        if (!moduleManager || !moduleManager->isModuleExist(moduleName)) {
            continue; /* Skip if module not found */
        }

        YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);
        if (!moduleData["port"] || !moduleData["port"].IsMap()
            || !moduleData["port"][portName.toStdString()]) {
            continue; /* Skip if port not found */
        }

        const YAML::Node &portData  = moduleData["port"][portName.toStdString()];
        int               portWidth = getPortWidth(portData);

        /* Set expected width if not set yet */
        if (expectedWidth == -1) {
            expectedWidth = portWidth;
        }
        /* Check if this port width matches the expected width */
        else if (portWidth != expectedWidth) {
            return false; /* Width mismatch found */
        }
    }

    return true;
}

QSoCGenerateManager::PortDirectionStatus QSoCGenerateManager::checkPortDirectionConsistency(
    const QList<QPair<QString, QString>> &connections)
{
    if (connections.isEmpty()) {
        return PortDirectionStatus::Underdrive; /* No connections means no drivers */
    }

    int outputCount = 0;
    int inputCount  = 0;
    int inoutCount  = 0;

    /* Check each connection's port direction */
    for (const auto &conn : connections) {
        const QString &instanceName = conn.first;
        const QString &portName     = conn.second;

        /* Get instance's module name */
        if (!netlistData["instance"][instanceName.toStdString()]
            || !netlistData["instance"][instanceName.toStdString()]["module"]
            || !netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
            continue; /* Skip invalid instance */
        }

        const QString moduleName = QString::fromStdString(
            netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

        /* Get module definition */
        if (!moduleManager || !moduleManager->isModuleExist(moduleName)) {
            continue; /* Skip if module not found */
        }

        YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);
        if (!moduleData["port"] || !moduleData["port"].IsMap()
            || !moduleData["port"][portName.toStdString()]) {
            continue; /* Skip if port not found */
        }

        const YAML::Node &portData = moduleData["port"][portName.toStdString()];

        /* Get port direction */
        QString direction = "input"; /* Default to input if not specified */
        if (portData["direction"] && portData["direction"].IsScalar()) {
            direction = QString::fromStdString(portData["direction"].as<std::string>()).toLower();
        }

        /* Count by direction */
        if (direction == "output" || direction == "out") {
            outputCount++;
        } else if (direction == "inout") {
            inoutCount++;
        } else if (direction == "input" || direction == "in") {
            inputCount++;
        } else {
            inputCount++; /* Treat unknown directions as input */
        }
    }

    /* Check for underdrive - all ports are inputs */
    if (outputCount == 0 && inoutCount == 0 && inputCount > 0) {
        return PortDirectionStatus::Underdrive;
    }

    /* Check for multidrive - multiple output or inout ports */
    if (outputCount + inoutCount > 1) {
        return PortDirectionStatus::Multidrive;
    }

    /* Otherwise, valid connection pattern */
    return PortDirectionStatus::Valid;
}

bool QSoCGenerateManager::generateVerilog(const QString &outputFileName)
{
    /* Check if netlistData is valid */
    if (!netlistData["instance"]) {
        qCritical() << "Error: Invalid netlist data, missing 'instance' section, make sure "
                       "loadNetlist() and processNetlist() have been called";
        return false;
    }

    if (!netlistData["instance"].IsMap() || netlistData["instance"].size() == 0) {
        qCritical() << "Error: Invalid netlist data, 'instance' section is empty or not a map";
        return false;
    }

    /* Check if net section exists and has valid format if present */
    if (netlistData["net"] && !netlistData["net"].IsMap()) {
        qCritical() << "Error: Invalid netlist data, 'net' section is not a map";
        return false;
    }

    /* Check if project manager is valid */
    if (!projectManager) {
        qCritical() << "Error: Project manager is null";
        return false;
    }

    if (!projectManager->isValidOutputPath(true)) {
        qCritical() << "Error: Invalid output path: " << projectManager->getOutputPath();
        return false;
    }

    /* Prepare output file path */
    const QString outputFilePath
        = QDir(projectManager->getOutputPath()).filePath(outputFileName + ".v");

    /* Open output file for writing */
    QFile outputFile(outputFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCritical() << "Error: Failed to open output file for writing:" << outputFilePath;
        return false;
    }

    QTextStream out(&outputFile);

    /* Generate file header */
    out << "/**\n";
    out << " * @file " << outputFileName << ".v\n";
    out << " * @brief RTL implementation of " << outputFileName << "\n";
    out << " *\n";
    out << " * @details This file contains RTL implementation based on the input netlist.\n"
        << " *          Auto-generated RTL Verilog file. Generated by "
        << QCoreApplication::applicationName() << " " << QCoreApplication::applicationVersion()
        << ".\n";
    out << " * NOTE: Auto-generated file, do not edit manually.\n";
    out << " */\n\n";

    /* Generate module declaration */
    out << "module " << outputFileName;

    /* Add module parameters if they exist */
    if (netlistData["parameter"] && netlistData["parameter"].IsMap()
        && netlistData["parameter"].size() > 0) {
        out << " #(\n";
        QStringList paramDeclarations;

        for (auto paramIter = netlistData["parameter"].begin();
             paramIter != netlistData["parameter"].end();
             ++paramIter) {
            if (!paramIter->first.IsScalar()) {
                qWarning() << "Warning: Invalid parameter name, skipping";
                continue;
            }

            const QString paramName = QString::fromStdString(paramIter->first.as<std::string>());

            if (!paramIter->second.IsMap()) {
                qWarning() << "Warning: Parameter" << paramName << "has invalid format, skipping";
                continue;
            }

            QString paramType  = ""; // Default to empty for Verilog 2001
            QString paramValue = "";

            if (paramIter->second["type"] && paramIter->second["type"].IsScalar()) {
                paramType = QString::fromStdString(paramIter->second["type"].as<std::string>());
                // Strip out 'logic' keyword for Verilog 2001 compatibility
                paramType = paramType.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");

                // Add a space if type isn't empty after processing
                if (!paramType.isEmpty() && !paramType.endsWith(" ")) {
                    paramType += " ";
                }
            }

            if (paramIter->second["value"] && paramIter->second["value"].IsScalar()) {
                paramValue = QString::fromStdString(paramIter->second["value"].as<std::string>());
            }

            paramDeclarations.append(
                QString("    parameter %1%2 = %3").arg(paramType).arg(paramName).arg(paramValue));
        }

        if (!paramDeclarations.isEmpty()) {
            out << paramDeclarations.join(",\n") << "\n";
        }
        out << ")";
    }

    /* Start port list */
    out << " (";

    /* Collect all ports for module interface */
    QStringList            ports;
    QMap<QString, QString> portToNetConnections; /* Map of port name to connected net name */

    /* Process port section if it exists */
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (auto portIter = netlistData["port"].begin(); portIter != netlistData["port"].end();
             ++portIter) {
            if (!portIter->first.IsScalar()) {
                qWarning() << "Warning: Invalid port name, skipping";
                continue;
            }

            const QString portName = QString::fromStdString(portIter->first.as<std::string>());

            if (!portIter->second.IsMap()) {
                qWarning() << "Warning: Port" << portName << "has invalid format, skipping";
                continue;
            }

            QString direction = "input";
            QString type      = ""; /* Empty type by default for Verilog 2001 */

            if (portIter->second["direction"] && portIter->second["direction"].IsScalar()) {
                QString dirStr = QString::fromStdString(
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

            if (portIter->second["type"] && portIter->second["type"].IsScalar()) {
                type = QString::fromStdString(portIter->second["type"].as<std::string>());
                /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                type = type.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");
            }

            /* Store the connection information if present */
            if (portIter->second["connect"] && portIter->second["connect"].IsScalar()) {
                QString connectedNet = QString::fromStdString(
                    portIter->second["connect"].as<std::string>());
                portToNetConnections[portName] = connectedNet;
            }

            /* Add port declaration */
            ports.append(QString("%1 %2").arg(direction).arg(
                type.isEmpty() ? portName : type + " " + portName));
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

    /* Add connections (wires) section comment */
    out << "    /* Wire declarations */\n";

    /* Generate wire declarations FIRST */
    if (netlistData["net"]) {
        if (!netlistData["net"].IsMap()) {
            qWarning() << "Warning: 'net' section is not a map, skipping wire declarations";
        } else if (netlistData["net"].size() == 0) {
            qWarning() << "Warning: 'net' section is empty, no wire declarations to generate";
        } else {
            for (auto netIter = netlistData["net"].begin(); netIter != netlistData["net"].end();
                 ++netIter) {
                if (!netIter->first.IsScalar()) {
                    qWarning() << "Warning: Invalid net name, skipping";
                    continue;
                }

                const QString netName = QString::fromStdString(netIter->first.as<std::string>());

                if (!netIter->second) {
                    qWarning() << "Warning: Net" << netName << "has null data, skipping";
                    continue;
                }

                /* Net connections should be a map of instance-port pairs */
                if (!netIter->second.IsMap()) {
                    qWarning() << "Warning: Net" << netName << "is not a map, skipping";
                    continue;
                }

                const YAML::Node &connections = netIter->second;

                if (connections.size() == 0) {
                    qWarning() << "Warning: Net" << netName << "has no connections, skipping";
                    continue;
                }

                /* Build a list of instance-port pairs for width check */
                QList<QPair<QString, QString>> portPairs;

                /* Build port pairs from netlistData */
                const YAML::Node &netNode = netlistData["net"][netName.toStdString()];
                if (netNode.IsMap()) {
                    for (const auto &instancePair : netNode) {
                        if (instancePair.first.IsScalar()) {
                            QString instanceName = QString::fromStdString(
                                instancePair.first.as<std::string>());

                            /* Verify this is a valid instance with a port */
                            if (instancePair.second.IsMap() && instancePair.second["port"]
                                && instancePair.second["port"].IsScalar()) {
                                QString portName = QString::fromStdString(
                                    instancePair.second["port"].as<std::string>());
                                portPairs.append(qMakePair(instanceName, portName));
                            }
                        }
                    }
                }

                /* Check port width consistency */
                if (!checkPortWidthConsistency(portPairs)) {
                    qWarning() << "Warning: Port width mismatch detected for net" << netName;
                    out << "    /* TODO: width mismatch on net " << netName
                        << ", please check connected ports */\n";
                }

                /* Check port direction consistency */
                PortDirectionStatus dirStatus = checkPortDirectionConsistency(portPairs);
                if (dirStatus == PortDirectionStatus::Underdrive) {
                    qWarning() << "Warning: Net" << netName
                               << "has only input ports, missing driver";
                    out << "    /* TODO: Net " << netName << " is undriven - missing source */\n";
                } else if (dirStatus == PortDirectionStatus::Multidrive) {
                    qWarning() << "Warning: Net" << netName << "has multiple output/inout ports";
                    out << "    /* TODO: Net " << netName
                        << " has multiple drivers - potential conflict */\n";
                }

                /* Always declare wire for all nets */
                out << "    wire " << netName << ";\n";

                /* Build port connection mapping for each instance */
                try {
                    /* Directly build connections from netlistData */
                    const YAML::Node &netNode = netlistData["net"][netName.toStdString()];
                    if (netNode.IsMap()) {
                        for (const auto &instancePair : netNode) {
                            if (instancePair.first.IsScalar()) {
                                QString instanceName = QString::fromStdString(
                                    instancePair.first.as<std::string>());

                                /* Verify this is a valid instance with a port */
                                if (instancePair.second.IsMap() && instancePair.second["port"]
                                    && instancePair.second["port"].IsScalar()) {
                                    QString portName = QString::fromStdString(
                                        instancePair.second["port"].as<std::string>());

                                    /* Add to the connection map */
                                    if (!instancePortConnections.contains(instanceName)) {
                                        instancePortConnections[instanceName]
                                            = QMap<QString, QString>();
                                    }
                                    instancePortConnections[instanceName][portName] = netName;
                                }
                            }
                        }
                    }
                } catch (const std::exception &e) {
                    qWarning() << "Exception building connection map:" << e.what();
                }
            }
            out << "\n";
        }
    } else {
        qWarning()
            << "Warning: No 'net' section in netlist, no wire declarations will be generated";
    }

    /* Add instances section comment */
    out << "    /* Module instantiations */\n";

    /* Generate instance declarations after wire declarations */
    for (auto instanceIter = netlistData["instance"].begin();
         instanceIter != netlistData["instance"].end();
         ++instanceIter) {
        /* Check if the instance name is a scalar */
        if (!instanceIter->first.IsScalar()) {
            qWarning() << "Warning: Invalid instance name, skipping";
            continue;
        }

        const QString instanceName = QString::fromStdString(instanceIter->first.as<std::string>());

        /* Check if the instance data is valid */
        if (!instanceIter->second || !instanceIter->second.IsMap()) {
            qWarning() << "Warning: Invalid instance data for" << instanceName
                       << "(not a map), skipping";
            continue;
        }

        const YAML::Node &instanceData = instanceIter->second;

        if (!instanceData["module"] || !instanceData["module"].IsScalar()) {
            qWarning() << "Warning: Invalid module name for instance" << instanceName;
            continue;
        }

        const QString moduleName = QString::fromStdString(instanceData["module"].as<std::string>());

        /* Generate instance declaration with parameters if any */
        out << "    " << moduleName << " ";

        /* Add parameters if they exist */
        if (instanceData["parameter"]) {
            if (!instanceData["parameter"].IsMap()) {
                qWarning() << "Warning: 'parameter' section for instance" << instanceName
                           << "is not a map, ignoring";
            } else if (instanceData["parameter"].size() == 0) {
                qWarning() << "Warning: 'parameter' section for instance" << instanceName
                           << "is empty, ignoring";
            } else {
                out << "#(\n";

                QStringList paramList;
                for (auto paramIter = instanceData["parameter"].begin();
                     paramIter != instanceData["parameter"].end();
                     ++paramIter) {
                    if (!paramIter->first.IsScalar()) {
                        qWarning() << "Warning: Invalid parameter name in instance" << instanceName;
                        continue;
                    }

                    if (!paramIter->second.IsScalar()) {
                        qWarning()
                            << "Warning: Parameter"
                            << QString::fromStdString(paramIter->first.as<std::string>())
                            << "in instance" << instanceName << "has a non-scalar value, skipping";
                        continue;
                    }

                    const QString paramName = QString::fromStdString(
                        paramIter->first.as<std::string>());
                    const QString paramValue = QString::fromStdString(
                        paramIter->second.as<std::string>());

                    paramList.append(QString("        .%1(%2)").arg(paramName).arg(paramValue));
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
                        qWarning() << "Warning: Invalid port name in module" << moduleName;
                        continue;
                    }

                    QString portName = QString::fromStdString(portIter->first.as<std::string>());

                    /* Check if this port has a connection */
                    if (portMap.contains(portName)) {
                        QString wireConnection = portMap[portName];
                        portConnections.append(
                            QString("        .%1(%2)").arg(portName).arg(wireConnection));
                    } else {
                        /* Port exists in module but has no connection */
                        QString direction = "signal";
                        if (portIter->second && portIter->second["direction"]
                            && portIter->second["direction"].IsScalar()) {
                            direction = QString::fromStdString(
                                portIter->second["direction"].as<std::string>());
                        }
                        portConnections.append(QString("        .%1(/* TODO: %2 %3 missing */)")
                                                   .arg(portName)
                                                   .arg(direction)
                                                   .arg(portName));
                    }
                }
            } else {
                qWarning() << "Warning: Module" << moduleName << "has no valid port section";
            }
        } else {
            qWarning() << "Warning: Failed to get module definition for" << moduleName;

            /* Fall back to existing connections if module definition not available */
            if (instancePortConnections.contains(instanceName)) {
                QMap<QString, QString>        &portMap = instancePortConnections[instanceName];
                QMapIterator<QString, QString> it(portMap);
                while (it.hasNext()) {
                    it.next();
                    portConnections.append(QString("        .%1(%2)").arg(it.key()).arg(it.value()));
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
    }

    /* Generate port connection assignments */
    if (!portToNetConnections.isEmpty()) {
        out << "\n    /* Port connection assignments */\n";
        out << "    /* Note: These assignments connect top-level ports to internal wires */\n";

        for (auto it = portToNetConnections.begin(); it != portToNetConnections.end(); ++it) {
            const QString &portName = it.key();
            const QString &netName  = it.value();

            /* Get port direction and width */
            QString portDirection = "input"; // Default
            QString portWidth     = "";

            if (netlistData["port"] && netlistData["port"][portName.toStdString()]
                && netlistData["port"][portName.toStdString()]["direction"]
                && netlistData["port"][portName.toStdString()]["direction"].IsScalar()) {
                QString dirStr
                    = QString::fromStdString(
                          netlistData["port"][portName.toStdString()]["direction"].as<std::string>())
                          .toLower();

                /* Handle both full and abbreviated forms */
                if (dirStr == "out" || dirStr == "output") {
                    portDirection = "output";
                } else if (dirStr == "in" || dirStr == "input") {
                    portDirection = "input";
                } else if (dirStr == "inout") {
                    portDirection = "inout";
                }
            }

            /* Get port type/width */
            if (netlistData["port"] && netlistData["port"][portName.toStdString()]
                && netlistData["port"][portName.toStdString()]["type"]
                && netlistData["port"][portName.toStdString()]["type"].IsScalar()) {
                portWidth = QString::fromStdString(
                    netlistData["port"][portName.toStdString()]["type"].as<std::string>());
            }

            /* Get net width */
            QString netWidth = "";
            if (netlistData["net"] && netlistData["net"][netName.toStdString()]
                && netlistData["net"][netName.toStdString()]["type"]
                && netlistData["net"][netName.toStdString()]["type"].IsScalar()) {
                netWidth = QString::fromStdString(
                    netlistData["net"][netName.toStdString()]["type"].as<std::string>());
            }

            /* Check width compatibility */
            bool widthMismatch = !portWidth.isEmpty() && !netWidth.isEmpty()
                                 && portWidth != netWidth;

            /* Generate the appropriate assign statement based on port direction */
            if (portDirection == "input") {
                out << "    assign " << netName << " = " << portName;
                if (widthMismatch) {
                    out << "; /* TODO: Width mismatch - port: " << portWidth
                        << ", net: " << netWidth << " */";
                }
                out << ";\n";
            } else if (portDirection == "output") {
                out << "    assign " << portName << " = " << netName;
                if (widthMismatch) {
                    out << "; /* TODO: Width mismatch - port: " << portWidth
                        << ", net: " << netWidth << " */";
                }
                out << ";\n";
            } else if (portDirection == "inout") {
                out << "    /* TODO: inout port " << portName << " connected to net " << netName
                    << " */\n";
            }
        }
        out << "\n";
    }

    /* Close module */
    out << "endmodule\n";

    outputFile.close();
    qInfo() << "Successfully generated Verilog file:" << outputFilePath;

    /* Format generated Verilog file if verible-verilog-format is available */
    formatVerilogFile(outputFilePath);

    return true;
}

bool QSoCGenerateManager::formatVerilogFile(const QString &filePath)
{
    /* Check if verible-verilog-format tool is available in the system */
    QProcess which;
    which.start("which", QStringList() << "verible-verilog-format");
    which.waitForFinished();

    if (which.exitCode() != 0) {
        /* Tool not found, silently return */
        qDebug() << "verible-verilog-format not found, skipping formatting";
        return false;
    }

    /* Tool found, proceed with formatting */
    qInfo() << "Formatting Verilog file using verible-verilog-format...";

    QProcess    formatter;
    QStringList args;
    args << "--inplace"
         << "--column_limit" << "119"
         << "--indentation_spaces" << "4"
         << "--line_break_penalty" << "4"
         << "--wrap_spaces" << "4"
         << "--port_declarations_alignment" << "align"
         << "--port_declarations_indentation" << "indent"
         << "--formal_parameters_alignment" << "align"
         << "--formal_parameters_indentation" << "indent"
         << "--assignment_statement_alignment" << "align"
         << "--enum_assignment_statement_alignment" << "align"
         << "--class_member_variable_alignment" << "align"
         << "--module_net_variable_alignment" << "align"
         << "--named_parameter_alignment" << "align"
         << "--named_parameter_indentation" << "indent"
         << "--named_port_alignment" << "align"
         << "--named_port_indentation" << "indent"
         << "--struct_union_members_alignment" << "align" << filePath;

    formatter.start("verible-verilog-format", args);
    formatter.waitForFinished();

    if (formatter.exitCode() == 0) {
        qInfo() << "Successfully formatted Verilog file";
        return true;
    } else {
        qWarning() << "Error formatting Verilog file:" << formatter.errorString();
        return false;
    }
}