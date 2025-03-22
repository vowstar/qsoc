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
    /* Check if netlistData is valid */
    if (!netlistData["instance"]) {
        qCritical()
            << "Error: Invalid netlist data, missing 'instance' section, call loadNetlist() first";
        return false;
    }

    if (!netlistData["instance"].IsMap() || netlistData["instance"].size() == 0) {
        qCritical() << "Error: Invalid netlist data, 'instance' section is empty or not a map";
        return false;
    }

    /* Process each bus connection if bus section exists */
    if (netlistData["bus"]) {
        if (!netlistData["bus"].IsMap()) {
            qWarning() << "Warning: 'bus' section is not a map, skipping bus processing";
        } else if (netlistData["bus"].size() == 0) {
            qWarning() << "Warning: 'bus' section is empty, skipping bus processing";
        } else {
            /* Create a new net section if it doesn't exist */
            if (!netlistData["net"]) {
                netlistData["net"] = YAML::Node(YAML::NodeType::Map);
            } else if (!netlistData["net"].IsMap()) {
                netlistData["net"] = YAML::Node(YAML::NodeType::Map);
                qWarning() << "Warning: 'net' section was not a map, resetting it";
            }

            /* Iterate through each bus connection */
            for (auto busEntry = netlistData["bus"].begin(); busEntry != netlistData["bus"].end();
                 ++busEntry) {
                if (!busEntry->first.IsScalar() || !busEntry->second) {
                    qWarning() << "Warning: Invalid bus entry, skipping";
                    continue;
                }

                const QString busConnectionName = QString::fromStdString(
                    busEntry->first.as<std::string>());
                const YAML::Node &busConnections = busEntry->second;

                if (!busConnections.IsSequence() || busConnections.size() == 0) {
                    qWarning() << "Warning: Invalid or empty bus connection format for"
                               << busConnectionName;
                    continue;
                }

                /* Process each bus port in the connection */
                for (size_t i = 0; i < busConnections.size(); ++i) {
                    const YAML::Node &connection = busConnections[i];

                    if (!connection || !connection.IsMap()) {
                        qWarning() << "Warning: Invalid connection data (not a map) in bus"
                                   << busConnectionName;
                        continue;
                    }

                    if (!connection["instance"] || !connection["instance"].IsScalar()
                        || !connection["port"] || !connection["port"].IsScalar()) {
                        qWarning()
                            << "Warning: Invalid connection data (missing instance or port) in bus"
                            << busConnectionName;
                        continue;
                    }

                    const QString instanceName = QString::fromStdString(
                        connection["instance"].as<std::string>());
                    const QString portName = QString::fromStdString(
                        connection["port"].as<std::string>());

                    /* Check if instance exists in netlist */
                    if (!netlistData["instance"][instanceName.toStdString()]) {
                        qWarning() << "Warning: Instance" << instanceName << "not found in netlist";
                        continue;
                    }

                    if (!netlistData["instance"][instanceName.toStdString()].IsMap()
                        || !netlistData["instance"][instanceName.toStdString()]["module"]
                        || !netlistData["instance"][instanceName.toStdString()]["module"].IsScalar()) {
                        qWarning() << "Warning: Invalid instance data for" << instanceName;
                        continue;
                    }

                    /* Check if module exists in module library */
                    const QString moduleName = QString::fromStdString(
                        netlistData["instance"][instanceName.toStdString()]["module"]
                            .as<std::string>());

                    if (!moduleManager || !moduleManager->isModuleExist(moduleName)) {
                        qWarning()
                            << "Warning: Module" << moduleName << "not found in module library";
                        continue;
                    }

                    /* Get module data */
                    YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);
                    if (!moduleData) {
                        qWarning() << "Warning: Failed to get module data for" << moduleName;
                        continue;
                    }

                    /* Check if bus exists in module */
                    if (!moduleData["bus"] || !moduleData["bus"].IsMap()
                        || !moduleData["bus"][portName.toStdString()]
                        || !moduleData["bus"][portName.toStdString()].IsMap()) {
                        qWarning()
                            << "Warning: Bus" << portName << "not found in module" << moduleName;
                        continue;
                    }

                    /* Get bus name from module's bus definition */
                    if (!moduleData["bus"][portName.toStdString()]["bus"]
                        || !moduleData["bus"][portName.toStdString()]["bus"].IsScalar()) {
                        qWarning() << "Warning: Invalid bus definition for" << portName
                                   << "in module" << moduleName;
                        continue;
                    }

                    const QString busType = QString::fromStdString(
                        moduleData["bus"][portName.toStdString()]["bus"].as<std::string>());

                    /* Check if bus exists in bus library */
                    if (!busManager || !busManager->isBusExist(busType)) {
                        qWarning() << "Warning: Bus type" << busType << "not found in bus library";
                        continue;
                    }

                    /* Get bus data */
                    YAML::Node busData = busManager->getBusYaml(busType);
                    if (!busData || !busData["port"] || !busData["port"].IsMap()
                        || busData["port"].size() == 0) {
                        qWarning() << "Warning: Invalid bus data for" << busType;
                        continue;
                    }

                    /* Get bus port mappings from module */
                    if (!moduleData["bus"][portName.toStdString()]["mapping"]
                        || !moduleData["bus"][portName.toStdString()]["mapping"].IsMap()) {
                        qWarning() << "Warning: Missing or invalid mapping for bus" << portName
                                   << "in module" << moduleName;
                        continue;
                    }

                    const YAML::Node &busMapping
                        = moduleData["bus"][portName.toStdString()]["mapping"];

                    /* Expand bus signals and add them to the net section */
                    for (auto portIter = busData["port"].begin(); portIter != busData["port"].end();
                         ++portIter) {
                        if (!portIter->first.IsScalar()) {
                            qWarning() << "Warning: Invalid port name in bus" << busType;
                            continue;
                        }

                        const QString signalName = QString::fromStdString(
                            portIter->first.as<std::string>());

                        /* Check if this signal is mapped in the module */
                        if (!busMapping[signalName.toStdString()]
                            || !busMapping[signalName.toStdString()].IsScalar()) {
                            continue; // Skip unmapped signals
                        }

                        /* Get the module port name mapped to this bus signal */
                        const QString modulePortName = QString::fromStdString(
                            busMapping[signalName.toStdString()].as<std::string>());

                        /* Skip empty mappings */
                        if (modulePortName.isEmpty()) {
                            continue;
                        }

                        /* Create a unique net name for this bus signal */
                        const QString netName = busConnectionName + "_" + signalName;

                        /* Create a new net entry if it doesn't exist */
                        if (!netlistData["net"][netName.toStdString()]) {
                            netlistData["net"][netName.toStdString()] = YAML::Node(
                                YAML::NodeType::Sequence);
                        } else if (!netlistData["net"][netName.toStdString()].IsSequence()) {
                            qWarning() << "Warning: Net" << netName
                                       << "exists but is not a sequence, overwriting";
                            netlistData["net"][netName.toStdString()] = YAML::Node(
                                YAML::NodeType::Sequence);
                        }

                        /* Add the instance.port to the net */
                        YAML::Node connectionNode;
                        connectionNode["instance"] = instanceName.toStdString();
                        connectionNode["port"]     = modulePortName.toStdString();
                        netlistData["net"][netName.toStdString()].push_back(connectionNode);
                    }
                }
            }

            /* Remove the processed bus section (as it has been expanded to nets) */
            netlistData.remove("bus");
        }
    }

    /* Print the processed netlist for debugging */
    qInfo() << "Netlist processed, all buses expanded into individual signals";
    std::cout << "Expanded Netlist:\n" << netlistData << std::endl;
    return true;
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