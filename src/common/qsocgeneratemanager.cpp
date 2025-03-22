#include "common/qsocgeneratemanager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
                std::string busType; // Will be determined from the first valid connection

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

                    /* Create a net for this signal as a sequence */
                    netlistData["net"][netName] = YAML::Node(YAML::NodeType::Sequence);

                    /* Add each connection to this net */
                    for (const Connection &conn : validConnections) {
                        try {
                            /* Get module data again */
                            YAML::Node moduleData = moduleManager->getModuleYaml(
                                QString::fromStdString(conn.moduleName));

                            /* Get port mapping */
                            std::string mappedPortName;
                            bool        mappingFound = false;

                            /* Try with the exact port name */
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
                            /* Try with the stripped port name (without pad_ prefix) */
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
                                continue; // Skip this signal for this connection
                            }

                            /* Create a connection entry in the sequence format */
                            YAML::Node connectionNode;
                            connectionNode["instance"] = conn.instanceName;
                            connectionNode["port"]     = mappedPortName;
                            netlistData["net"][netName].push_back(connectionNode);

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
    out << "// Generated by QSoC - Generated RTL Verilog file\n";
    out << "// Auto-generated file, do not edit manually\n\n";

    /* Generate module declaration */
    out << "module " << outputFileName << " (\n";

    /* Collect all ports for module interface */
    QStringList ports;

    /* For simplicity, all nets connected to the world outside will be considered ports */
    /* In a real implementation, this would need more sophisticated logic */

    /* Close module declaration */
    if (!ports.isEmpty()) {
        out << "    " << ports.join(",\n    ") << "\n";
    }
    out << ");\n\n";

    /* Generate instance declarations */
    for (auto instanceIter = netlistData["instance"].begin();
         instanceIter != netlistData["instance"].end();
         ++instanceIter) {
        if (!instanceIter->first.IsScalar()) {
            qWarning() << "Warning: Invalid instance name, skipping";
            continue;
        }

        const QString instanceName = QString::fromStdString(instanceIter->first.as<std::string>());

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

        /* Placeholder for port connections - would need to be filled with actual port connections */
        /* This would be quite complex and would require modules to be loaded */
        out << "        // Port connections would go here\n";

        out << "    );\n\n";
    }

    /* Generate wire declarations */
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

                /* Net connections should be a sequence of instance-port pairs */
                if (!netIter->second.IsSequence()) {
                    qWarning() << "Warning: Net" << netName << "is not a sequence, skipping";
                    continue;
                }

                const YAML::Node &connections = netIter->second;

                if (connections.size() == 0) {
                    qWarning() << "Warning: Net" << netName << "has no connections, skipping";
                    continue;
                }

                /* Determine wire type based on the first connection */
                /* In a real implementation, this would need validation across all connections */
                const YAML::Node &firstConnection = connections[0];

                if (!firstConnection.IsMap()) {
                    qWarning() << "Warning: First connection of net" << netName
                               << "is not a map, skipping";
                    continue;
                }

                if (!firstConnection["instance"] || !firstConnection["instance"].IsScalar()
                    || !firstConnection["port"] || !firstConnection["port"].IsScalar()) {
                    qWarning() << "Warning: Invalid connection data in net" << netName
                               << ", skipping";
                    continue;
                }

                const QString instanceName = QString::fromStdString(
                    firstConnection["instance"].as<std::string>());
                const QString portName = QString::fromStdString(
                    firstConnection["port"].as<std::string>());

                /* Check if instance exists */
                if (!netlistData["instance"]
                    || !netlistData["instance"][instanceName.toStdString()]) {
                    qWarning() << "Warning: Instance" << instanceName << "referenced in net"
                               << netName << "not found in netlist, skipping";
                    continue;
                }

                /* Get module name for this instance */
                if (!netlistData["instance"][instanceName.toStdString()]["module"]
                    || !netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
                    qWarning() << "Warning: Invalid module name for instance" << instanceName
                               << ", skipping";
                    continue;
                }

                const QString moduleName = QString::fromStdString(
                    netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

                /* Get port type from module definition */
                if (!moduleManager) {
                    qWarning() << "Warning: Module manager is null, skipping wire declaration for"
                               << netName;
                    continue;
                }

                if (!moduleManager->isModuleExist(moduleName)) {
                    qWarning() << "Warning: Module" << moduleName
                               << "not found in module library, skipping";
                    continue;
                }

                YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);
                if (!moduleData) {
                    qWarning() << "Warning: Failed to get module data for" << moduleName
                               << ", skipping";
                    continue;
                }

                if (!moduleData["port"]) {
                    qWarning() << "Warning: Module" << moduleName
                               << "has no port section, skipping";
                    continue;
                }

                if (!moduleData["port"].IsMap()) {
                    qWarning() << "Warning: Port section of module" << moduleName
                               << "is not a map, skipping";
                    continue;
                }

                if (!moduleData["port"][portName.toStdString()]) {
                    qWarning() << "Warning: Port" << portName << "not found in module" << moduleName
                               << ", skipping";
                    continue;
                }

                const YAML::Node &portData = moduleData["port"][portName.toStdString()];
                QString           portType = "wire"; // Default type

                if (portData["type"] && portData["type"].IsScalar()) {
                    portType = QString::fromStdString(portData["type"].as<std::string>());
                }

                /* Generate wire declaration */
                out << "    wire " << portType << " " << netName << ";\n";
            }
        }
    } else {
        qWarning()
            << "Warning: No 'net' section in netlist, no wire declarations will be generated";
    }

    /* Close module */
    out << "\nendmodule\n";

    outputFile.close();
    qInfo() << "Successfully generated Verilog file:" << outputFilePath;

    return true;
}