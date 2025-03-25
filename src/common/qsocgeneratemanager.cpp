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

QSocGenerateManager::QSocGenerateManager(
    QObject            *parent,
    QSocProjectManager *projectManager,
    QSocModuleManager  *moduleManager,
    QSocBusManager     *busManager,
    QLLMService        *llmService)
    : QObject(parent)
    , projectManager(projectManager)
    , moduleManager(moduleManager)
    , busManager(busManager)
    , llmService(llmService)
{
    /* Empty constructor */
}

void QSocGenerateManager::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

void QSocGenerateManager::setModuleManager(QSocModuleManager *moduleManager)
{
    this->moduleManager = moduleManager;
}

void QSocGenerateManager::setBusManager(QSocBusManager *busManager)
{
    this->busManager = busManager;
}

void QSocGenerateManager::setLLMService(QLLMService *llmService)
{
    this->llmService = llmService;
}

QSocProjectManager *QSocGenerateManager::getProjectManager()
{
    return projectManager;
}

QSocModuleManager *QSocGenerateManager::getModuleManager()
{
    return moduleManager;
}

QSocBusManager *QSocGenerateManager::getBusManager()
{
    return busManager;
}

QLLMService *QSocGenerateManager::getLLMService()
{
    return llmService;
}

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

    /* Get port widths for all connections */
    QMap<QPair<QString, QString>, QString> portWidths;

    for (const auto &conn : connections) {
        QString instanceName = conn.instanceName;
        QString portName     = conn.portName;

        if (conn.type == PortType::TopLevel) {
            /* Handle top-level port */
            if (netlistData["port"] && netlistData["port"][portName.toStdString()]
                && netlistData["port"][portName.toStdString()]["type"]
                && netlistData["port"][portName.toStdString()]["type"].IsScalar()) {
                /* Get port width from netlist data */
                QString width = QString::fromStdString(
                    netlistData["port"][portName.toStdString()]["type"].as<std::string>());
                /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                width = width.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");
                portWidths[qMakePair(instanceName, portName)] = width;
            } else {
                /* Default width if not specified */
                portWidths[qMakePair(instanceName, portName)] = "";
            }
        } else {
            /* Handle module port */
            if (netlistData["instance"][instanceName.toStdString()]
                && netlistData["instance"][instanceName.toStdString()]["module"]
                && netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
                QString moduleName = QString::fromStdString(
                    netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

                /* Get port width from module definition */
                if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                    YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                    if (moduleData["port"] && moduleData["port"].IsMap()
                        && moduleData["port"][portName.toStdString()]["type"]
                        && moduleData["port"][portName.toStdString()]["type"].IsScalar()) {
                        QString width = QString::fromStdString(
                            moduleData["port"][portName.toStdString()]["type"].as<std::string>());
                        /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                        width = width.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");
                        portWidths[qMakePair(instanceName, portName)] = width;
                    } else {
                        /* Default width if not specified */
                        portWidths[qMakePair(instanceName, portName)] = "";
                    }
                }
            }
        }
    }

    /* Compare port widths for consistency */
    QString referenceWidth;
    bool    firstPort = true;

    for (auto it = portWidths.constBegin(); it != portWidths.constEnd(); ++it) {
        if (firstPort) {
            referenceWidth = it.value();
            firstPort      = false;
        } else if (!it.value().isEmpty() && !referenceWidth.isEmpty() && it.value() != referenceWidth) {
            /* Width mismatch detected */
            return false;
        }
    }

    /* All widths are consistent or some are unspecified */
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
                QString dirStr = QString::fromStdString(
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
                QString moduleName = QString::fromStdString(
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
    } else if (outputCount + inoutCount > 1) {
        /* Multiple output or inout ports - potential conflict */
        return PortDirectionStatus::Multidrive;
    } else {
        /* Normal case: one driver, multiple inputs */
        return PortDirectionStatus::Valid;
    }
}

bool QSocGenerateManager::generateVerilog(const QString &outputFileName)
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

            /* Default to empty for Verilog 2001 */
            QString paramType  = "";
            QString paramValue = "";

            if (paramIter->second["type"] && paramIter->second["type"].IsScalar()) {
                paramType = QString::fromStdString(paramIter->second["type"].as<std::string>());
                /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                paramType = paramType.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");

                /* Add a space if type isn't empty after processing */
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
            QString connectedPortName;
            bool    connectedToTopPort = false;

            for (auto it = portToNetConnections.begin(); it != portToNetConnections.end(); ++it) {
                if (it.value() == netName) {
                    connectedToTopPort = true;
                    connectedPortName  = it.key();
                    break;
                }
            }

            try {
                /* Build connections using port names where appropriate */
                const YAML::Node &netNode = netIter->second;
                if (netNode.IsMap()) {
                    for (const auto &instancePair : netNode) {
                        if (instancePair.first.IsScalar()) {
                            const QString instanceName = QString::fromStdString(
                                instancePair.first.as<std::string>());
                            if (!instancePortConnections.contains(instanceName)) {
                                instancePortConnections[instanceName] = QMap<QString, QString>();
                            }

                            if (instancePair.second.IsMap()
                                && instancePair.second["port"].IsScalar()) {
                                const QString portName = QString::fromStdString(
                                    instancePair.second["port"].as<std::string>());

                                /* If connected to top-level port, use the port name instead of net name */
                                if (connectedToTopPort) {
                                    instancePortConnections[instanceName][portName]
                                        = connectedPortName;
                                } else {
                                    instancePortConnections[instanceName][portName] = netName;
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception &e) {
                qWarning() << "Error processing net:" << e.what();
            }
        }
    }

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
                QList<PortConnection> portConnections;
                /* Collect detailed port information for each connection */
                QList<PortDetailInfo> portDetails;

                /* Check if this net is connected to a top-level port */
                bool    connectedToTopPort = false;
                QString connectedPortName;
                QString topLevelPortDirection = "unknown";

                /* Check if this net is connected to a top-level port */
                for (auto it = portToNetConnections.begin(); it != portToNetConnections.end();
                     ++it) {
                    if (it.value() == netName) {
                        connectedToTopPort = true;
                        connectedPortName  = it.key();

                        /* Get the port direction */
                        if (netlistData["port"]
                            && netlistData["port"][connectedPortName.toStdString()]
                            && netlistData["port"][connectedPortName.toStdString()]["direction"]
                            && netlistData["port"][connectedPortName.toStdString()]["direction"]
                                   .IsScalar()) {
                            QString dirStr = QString::fromStdString(
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
                            QString reversedDirection;
                            if (topLevelPortDirection == "output") {
                                reversedDirection
                                    = "input"; /* Top-level output is an input for internal nets */
                            } else if (topLevelPortDirection == "input") {
                                reversedDirection
                                    = "output"; /* Top-level input is an output for internal nets */
                            } else if (topLevelPortDirection == "inout") {
                                reversedDirection = "inout"; /* Bidirectional remains bidirectional */
                            }

                            /* Add top-level port to connection list */
                            portConnections.append(
                                PortConnection::createTopLevelPort(connectedPortName));

                            /* Get port width */
                            QString portWidth = "";
                            if (netlistData["port"][connectedPortName.toStdString()]["type"]
                                && netlistData["port"][connectedPortName.toStdString()]["type"]
                                       .IsScalar()) {
                                portWidth = QString::fromStdString(
                                    netlistData["port"][connectedPortName.toStdString()]["type"]
                                        .as<std::string>());
                                /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                                portWidth
                                    = portWidth.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");
                            }

                            /* Add to detailed port information with reversed direction */
                            portDetails.append(
                                PortDetailInfo::createTopLevelPort(
                                    connectedPortName, portWidth, reversedDirection));
                        }
                        break;
                    }
                }

                /* Build port connections from netlistData */
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

                                /* Create a module port connection */
                                portConnections.append(
                                    PortConnection::createModulePort(instanceName, portName));

                                /* Get additional details for this port */
                                QString portWidth     = "";
                                QString portDirection = "unknown";

                                /* Get instance's module */
                                if (netlistData["instance"][instanceName.toStdString()]
                                    && netlistData["instance"][instanceName.toStdString()]["module"]
                                    && netlistData["instance"][instanceName.toStdString()]["module"]
                                           .IsScalar()) {
                                    QString moduleName = QString::fromStdString(
                                        netlistData["instance"][instanceName.toStdString()]
                                                   ["module"]
                                                       .as<std::string>());

                                    /* Get module definition */
                                    if (moduleManager && moduleManager->isModuleExist(moduleName)) {
                                        YAML::Node moduleData = moduleManager->getModuleYaml(
                                            moduleName);

                                        if (moduleData["port"] && moduleData["port"].IsMap()
                                            && moduleData["port"][portName.toStdString()]) {
                                            /* Get port width */
                                            if (moduleData["port"][portName.toStdString()]["type"]
                                                && moduleData["port"][portName.toStdString()]
                                                             ["type"]
                                                                 .IsScalar()) {
                                                portWidth = QString::fromStdString(
                                                    moduleData["port"][portName.toStdString()]
                                                              ["type"]
                                                                  .as<std::string>());
                                                /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                                                portWidth = portWidth.replace(
                                                    QRegularExpression("\\blogic(\\s+|\\b)"), "");
                                            }

                                            /* Get port direction */
                                            if (moduleData["port"][portName.toStdString()]
                                                          ["direction"]
                                                && moduleData["port"][portName.toStdString()]
                                                             ["direction"]
                                                                 .IsScalar()) {
                                                portDirection = QString::fromStdString(
                                                    moduleData["port"][portName.toStdString()]
                                                              ["direction"]
                                                                  .as<std::string>());
                                                /* Handle both full and abbreviated forms */
                                                if (portDirection == "out"
                                                    || portDirection == "output") {
                                                    portDirection = "output";
                                                } else if (
                                                    portDirection == "in"
                                                    || portDirection == "input") {
                                                    portDirection = "input";
                                                } else if (portDirection == "inout") {
                                                    portDirection = "inout";
                                                }
                                            }
                                        }
                                    }
                                }

                                /* Add to detailed port information */
                                portDetails.append(
                                    PortDetailInfo::createModulePort(
                                        instanceName, portName, portWidth, portDirection));
                            }
                        }
                    }
                }

                /* Check port width consistency */
                if (!checkPortWidthConsistency(portConnections)) {
                    qWarning() << "Warning: Port width mismatch detected for net" << netName;

                    /* FIXME: Port width mismatch detected for net */
                    /* Create header for FIXME comment */
                    if (connectedToTopPort) {
                        out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                            << ") width mismatch - please check connected ports:\n";
                    } else {
                        out << "    /* FIXME: Net " << netName
                            << " width mismatch - please check connected ports:\n";
                    }

                    /* Add detailed information for each connected port */
                    for (const auto &detail : portDetails) {
                        if (detail.type == PortType::TopLevel) {
                            /* For top-level ports, we need to display the original direction, not the reversed one */
                            QString displayDirection = detail.direction;
                            /* input -> output, output -> input, inout -> inout */
                            if (displayDirection == "input") {
                                displayDirection = "output";
                            } else if (displayDirection == "output") {
                                displayDirection = "input";
                            }

                            out << "     *   Top-Level Port: " << detail.portName
                                << ", Direction: " << displayDirection << ", Width: "
                                << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                        } else {
                            /* Regular instance port */
                            if (netlistData["instance"][detail.instanceName.toStdString()]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                                  .IsScalar()) {
                                out << "     *   Module: "
                                    << netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .as<std::string>()
                                                      .c_str()
                                    << ", Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            } else {
                                /* Handle case where instance data might be invalid */
                                out << "     *   Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            }
                        }
                    }

                    out << "     */\n";
                }

                /* Check port direction consistency */
                PortDirectionStatus dirStatus = checkPortDirectionConsistency(portConnections);

                if (dirStatus == PortDirectionStatus::Undriven) {
                    qWarning() << "Warning: Net" << netName
                               << "has only input ports, missing driver";
                    if (connectedToTopPort) {
                        out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                            << ") is undriven - missing source:\n";
                    } else {
                        out << "    /* FIXME: Net " << netName
                            << " is undriven - missing source:\n";
                    }

                    /* Add detailed information for each connected port */
                    for (const auto &detail : portDetails) {
                        if (detail.type == PortType::TopLevel) {
                            /* For top-level ports, we need to display the original direction, not the reversed one */
                            QString displayDirection = detail.direction;
                            if (displayDirection == "input") {
                                displayDirection = "output";
                            } else if (displayDirection == "output") {
                                displayDirection = "input";
                            }
                            /* inout remains inout */

                            out << "     *   Top-Level Port: " << detail.portName
                                << ", Direction: " << displayDirection << ", Width: "
                                << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                        } else {
                            /* Regular instance port */
                            if (netlistData["instance"][detail.instanceName.toStdString()]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                                  .IsScalar()) {
                                out << "     *   Module: "
                                    << netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .as<std::string>()
                                                      .c_str()
                                    << ", Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            } else {
                                /* Handle case where instance data might be invalid */
                                out << "     *   Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            }
                        }
                    }

                    out << "     */\n";

                } else if (dirStatus == PortDirectionStatus::Multidrive) {
                    qWarning() << "Warning: Net" << netName << "has multiple output/inout ports";
                    if (connectedToTopPort) {
                        out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                            << ") has multiple drivers - potential conflict:\n";
                    } else {
                        out << "    /* FIXME: Net " << netName
                            << " has multiple drivers - potential conflict:\n";
                    }

                    /* Add detailed information for each connected port */
                    for (const auto &detail : portDetails) {
                        if (detail.type == PortType::TopLevel) {
                            /* For top-level ports, we need to display the original direction, not the reversed one */
                            QString displayDirection = detail.direction;
                            if (displayDirection == "input") {
                                displayDirection = "output";
                            } else if (displayDirection == "output") {
                                displayDirection = "input";
                            }
                            /* inout remains inout */

                            out << "     *   Top-Level Port: " << detail.portName
                                << ", Direction: " << displayDirection << ", Width: "
                                << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                        } else {
                            /* Regular instance port */
                            if (netlistData["instance"][detail.instanceName.toStdString()]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                && netlistData["instance"][detail.instanceName.toStdString()]
                                              ["module"]
                                                  .IsScalar()) {
                                out << "     *   Module: "
                                    << netlistData["instance"][detail.instanceName.toStdString()]
                                                  ["module"]
                                                      .as<std::string>()
                                                      .c_str()
                                    << ", Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            } else {
                                /* Handle case where instance data might be invalid */
                                out << "     *   Instance: " << detail.instanceName
                                    << ", Port: " << detail.portName
                                    << ", Direction: " << detail.direction << ", Width: "
                                    << (detail.width.isEmpty() ? "default" : detail.width) << "\n";
                            }
                        }
                    }

                    out << "     */\n";
                }

                /* Only declare wire if not connected to top-level port to avoid redundancy */
                if (!connectedToTopPort) {
                    /* Add wire declaration for this net */
                    out << "    wire " << netName << ";\n";
                } else {
                    /* Check for width mismatches between port and net */
                    QString portWidth     = "";
                    QString netWidth      = "";
                    QString portDirection = "input";

                    /* Get port information */
                    if (netlistData["port"]
                        && netlistData["port"][connectedPortName.toStdString()]) {
                        auto portNode = netlistData["port"][connectedPortName.toStdString()];

                        /* Get port direction */
                        if (portNode["direction"] && portNode["direction"].IsScalar()) {
                            QString dirStr = QString::fromStdString(
                                                 portNode["direction"].as<std::string>())
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

                        /* Get port width/type */
                        if (portNode["type"] && portNode["type"].IsScalar()) {
                            portWidth = QString::fromStdString(portNode["type"].as<std::string>());
                        }
                    }

                    /* Get net width */
                    if (netlistData["net"] && netlistData["net"][netName.toStdString()]
                        && netlistData["net"][netName.toStdString()]["type"]
                        && netlistData["net"][netName.toStdString()]["type"].IsScalar()) {
                        netWidth = QString::fromStdString(
                            netlistData["net"][netName.toStdString()]["type"].as<std::string>());
                    }

                    /* Check width compatibility */
                    bool widthMismatch = !portWidth.isEmpty() && !netWidth.isEmpty()
                                         && portWidth != netWidth;

                    /* Add width mismatch FIXME comment if needed */
                    if (widthMismatch) {
                        out << "    /* FIXME: Port " << connectedPortName << " (net " << netName
                            << ") width mismatch - port width: " << portWidth
                            << ", net width: " << netWidth << " */\n";
                    }

                    /* Add direction warning if needed */
                    if (portDirection == "inout") {
                        out << "    /* FIXME: Port " << connectedPortName
                            << " is inout - verify bidirectional behavior */\n";
                    }
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
                        QString width     = "";

                        if (portIter->second && portIter->second["direction"]
                            && portIter->second["direction"].IsScalar()) {
                            direction = QString::fromStdString(
                                portIter->second["direction"].as<std::string>());
                        }

                        /* Get port width/type */
                        if (portIter->second && portIter->second["type"]
                            && portIter->second["type"].IsScalar()) {
                            QString type = QString::fromStdString(
                                portIter->second["type"].as<std::string>());

                            /* Strip out 'logic' keyword for Verilog 2001 compatibility */
                            type = type.replace(QRegularExpression("\\blogic(\\s+|\\b)"), "");

                            /* Extract width information if it exists in format [x:y] or [x] */
                            QRegularExpression widthRegex(
                                "\\[\\s*(\\d+)\\s*(?::\\s*(\\d+))?\\s*\\]");
                            auto match = widthRegex.match(type);
                            if (match.hasMatch()) {
                                if (match.captured(2).isEmpty()) {
                                    /* Format [x] */
                                    width = match.captured(0);
                                } else {
                                    /* Format [x:y] */
                                    width = match.captured(0);
                                }
                            }
                        }

                        /* Format FIXME message with width if available */
                        if (width.isEmpty()) {
                            portConnections.append(QString("        .%1(/* FIXME: %2 %3 missing */)")
                                                       .arg(portName)
                                                       .arg(direction)
                                                       .arg(portName));
                        } else {
                            portConnections.append(
                                QString("        .%1(/* FIXME: %2 %3 %4 missing */)")
                                    .arg(portName)
                                    .arg(direction)
                                    .arg(width)
                                    .arg(portName));
                        }
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

    /* Close module */
    out << "\nendmodule\n";

    outputFile.close();
    qInfo() << "Successfully generated Verilog file:" << outputFilePath;

    /* Format generated Verilog file if verible-verilog-format is available */
    formatVerilogFile(outputFilePath);

    return true;
}

bool QSocGenerateManager::formatVerilogFile(const QString &filePath)
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

    QProcess formatter;
    /* clang-format off */
    const QString argsStr = QStringLiteral(R"(
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
    args << filePath;

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