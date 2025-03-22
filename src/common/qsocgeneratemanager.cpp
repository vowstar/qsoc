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
        if (!netlistData["instance"] || !netlistData["instance"].IsMap()) {
            qCritical() << "Error: Invalid netlist format, missing or invalid 'instance' section";
            return false;
        }

        /* Validate net and bus sections */
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
    if (!netlistData["instance"] || !netlistData["instance"].IsMap()) {
        qCritical() << "Error: Invalid netlist data, call loadNetlist() first";
        return false;
    }

    /* Process each bus connection */
    if (netlistData["bus"] && netlistData["bus"].IsMap()) {
        /* Create a new net section if it doesn't exist */
        if (!netlistData["net"]) {
            netlistData["net"] = YAML::Node(YAML::NodeType::Map);
        }

        /* Iterate through each bus connection */
        for (auto busEntry = netlistData["bus"].begin(); busEntry != netlistData["bus"].end();
             ++busEntry) {
            const QString busConnectionName = QString::fromStdString(
                busEntry->first.as<std::string>());
            const YAML::Node &busConnections = busEntry->second;

            if (!busConnections.IsSequence()) {
                qWarning() << "Warning: Invalid bus connection format for" << busConnectionName;
                continue;
            }

            /* Process each bus port in the connection */
            for (size_t i = 0; i < busConnections.size(); ++i) {
                const YAML::Node &connection = busConnections[i];

                if (!connection.IsMap() || !connection["instance"] || !connection["port"]) {
                    qWarning() << "Warning: Invalid connection data in bus" << busConnectionName;
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

                /* Check if module exists in module library */
                const QString moduleName = QString::fromStdString(
                    netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

                if (!moduleManager->isModuleExist(moduleName)) {
                    qWarning() << "Warning: Module" << moduleName << "not found in module library";
                    continue;
                }

                /* Get module data */
                YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

                /* Check if bus exists in module */
                if (!moduleData["bus"] || !moduleData["bus"].IsMap()
                    || !moduleData["bus"][portName.toStdString()]) {
                    qWarning() << "Warning: Bus" << portName << "not found in module" << moduleName;
                    continue;
                }

                /* Get bus name from module's bus definition */
                const QString busType = QString::fromStdString(
                    moduleData["bus"][portName.toStdString()]["bus"].as<std::string>());

                /* Check if bus exists in bus library */
                if (!busManager->isBusExist(busType)) {
                    qWarning() << "Warning: Bus type" << busType << "not found in bus library";
                    continue;
                }

                /* Get bus data */
                YAML::Node busData = busManager->getBusYaml(busType);

                /* Get bus port mappings from module */
                const YAML::Node &busMapping = moduleData["bus"][portName.toStdString()]["mapping"];

                /* Expand bus signals and add them to the net section */
                for (auto portIter = busData["port"].begin(); portIter != busData["port"].end();
                     ++portIter) {
                    const QString signalName = QString::fromStdString(
                        portIter->first.as<std::string>());

                    /* Check if this signal is mapped in the module */
                    if (!busMapping[signalName.toStdString()]) {
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

    /* Print the processed netlist for debugging */
    qInfo() << "Netlist processed, all buses expanded into individual signals";
    std::cout << "Expanded Netlist:\n" << netlistData << std::endl;
    return true;
}

bool QSoCGenerateManager::generateVerilog(const QString &outputFileName)
{
    /* Check if netlistData is valid */
    if (!netlistData["instance"] || !netlistData["instance"].IsMap() || !netlistData["net"]
        || !netlistData["net"].IsMap()) {
        qCritical() << "Error: Invalid netlist data, make sure loadNetlist() and processNetlist() "
                       "have been called";
        return false;
    }

    /* Check if project manager is valid */
    if (!projectManager || !projectManager->isValidOutputPath(true)) {
        qCritical() << "Error: Invalid project manager or output path";
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
        const QString instanceName = QString::fromStdString(instanceIter->first.as<std::string>());
        const YAML::Node &instanceData = instanceIter->second;

        if (!instanceData.IsMap() || !instanceData["module"]) {
            qWarning() << "Warning: Invalid instance data for" << instanceName;
            continue;
        }

        const QString moduleName = QString::fromStdString(instanceData["module"].as<std::string>());

        /* Generate instance declaration with parameters if any */
        out << "    " << moduleName << " ";

        /* Add parameters if they exist */
        if (instanceData["parameter"] && instanceData["parameter"].IsMap()) {
            out << "#(\n";

            QStringList paramList;
            for (auto paramIter = instanceData["parameter"].begin();
                 paramIter != instanceData["parameter"].end();
                 ++paramIter) {
                const QString paramName = QString::fromStdString(paramIter->first.as<std::string>());
                const QString paramValue = QString::fromStdString(
                    paramIter->second.as<std::string>());

                paramList.append(QString("        .%1(%2)").arg(paramName, paramValue));
            }

            out << paramList.join(",\n") << "\n    ) ";
        }

        out << instanceName << " (\n";

        /* Placeholder for port connections - would need to be filled with actual port connections */
        /* This would be quite complex and would require modules to be loaded */
        out << "        // Port connections would go here\n";

        out << "    );\n\n";
    }

    /* Generate wire declarations */
    for (auto netIter = netlistData["net"].begin(); netIter != netlistData["net"].end(); ++netIter) {
        const QString     netName     = QString::fromStdString(netIter->first.as<std::string>());
        const YAML::Node &connections = netIter->second;

        if (!connections.IsSequence() || connections.size() == 0) {
            qWarning() << "Warning: Invalid net data for" << netName;
            continue;
        }

        /* Determine wire type based on the first connection */
        /* In a real implementation, this would need validation across all connections */
        const YAML::Node &firstConnection = connections[0];

        if (!firstConnection.IsMap() || !firstConnection["instance"] || !firstConnection["port"]) {
            qWarning() << "Warning: Invalid connection data in net" << netName;
            continue;
        }

        const QString instanceName = QString::fromStdString(
            firstConnection["instance"].as<std::string>());
        const QString portName = QString::fromStdString(firstConnection["port"].as<std::string>());

        /* Get module name for this instance */
        const QString moduleName = QString::fromStdString(
            netlistData["instance"][instanceName.toStdString()]["module"].as<std::string>());

        /* Get port type from module definition */
        if (!moduleManager->isModuleExist(moduleName)) {
            qWarning() << "Warning: Module" << moduleName << "not found in module library";
            continue;
        }

        YAML::Node moduleData = moduleManager->getModuleYaml(moduleName);

        if (!moduleData["port"] || !moduleData["port"].IsMap()
            || !moduleData["port"][portName.toStdString()]) {
            qWarning() << "Warning: Port" << portName << "not found in module" << moduleName;
            continue;
        }

        const YAML::Node &portData = moduleData["port"][portName.toStdString()];
        QString           portType = "wire"; // Default type

        if (portData["type"]) {
            portType = QString::fromStdString(portData["type"].as<std::string>());
        }

        /* Generate wire declaration */
        out << "    wire " << portType << " " << netName << ";\n";
    }

    /* Close module */
    out << "\nendmodule\n";

    outputFile.close();
    qInfo() << "Successfully generated Verilog file:" << outputFilePath;

    return true;
}