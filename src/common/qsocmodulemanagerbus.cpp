// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocmodulemanager.h"
#include "common/qstaticmarkdown.h"
#include "common/qstaticregex.h"
#include "common/qstaticstringweaver.h"

#include <nlohmann/json.hpp>
#include <QDebug>

using json = nlohmann::json;

bool QSocModuleManager::addModuleBus(
    const QString &moduleName,
    const QString &busName,
    const QString &busMode,
    const QString &busInterface)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return false;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Validate busManager */
    if (!busManager) {
        qCritical() << "Error: busManager is null.";
        return false;
    }

    /* Get bus YAML */
    YAML::Node busYaml = busManager->getBusYaml(busName);
    if (!busManager->isBusExist(busName)) {
        qCritical() << "Error: Bus does not exist:" << busName;
        return false;
    }

    /* Extract module ports from moduleYaml */
    QVector<QString> groupModule;
    if (moduleYaml["port"]) {
        for (YAML::const_iterator it = moduleYaml["port"].begin(); it != moduleYaml["port"].end();
             ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            groupModule.append(QString::fromStdString(portNameStd));
        }
    }

    /* Extract bus signals from busYaml - with the new structure where signals are under "port" */
    QVector<QString> groupBus;
    if (busYaml["port"]) {
        /* Signals are under the "port" node */
        for (YAML::const_iterator it = busYaml["port"].begin(); it != busYaml["port"].end(); ++it) {
            const std::string busSignalStd = it->first.as<std::string>();
            groupBus.append(QString::fromStdString(busSignalStd));
        }
    } else {
        /* No port node found */
        qCritical() << "Error: Bus has invalid structure (missing 'port' node):" << busName;
        return false;
    }

    /* Print extracted lists for debugging */
    qDebug() << "Module ports:" << groupModule;
    qDebug() << "Bus signals:" << groupBus;

    /* Use QStaticStringWeaver to match bus signals to module ports */
    /* Step 1: Extract candidate substrings for clustering */
    int minSubstringLength = 3; /* Min length for common substrings */
    int freqThreshold      = 2; /* Must appear in at least 2 strings */

    QMap<QString, int> candidateSubstrings = QStaticStringWeaver::extractCandidateSubstrings(
        groupModule, minSubstringLength, freqThreshold);

    /* Step 2: Cluster module ports based on common substrings */
    QMap<QString, QVector<QString>> groups
        = QStaticStringWeaver::clusterStrings(groupModule, candidateSubstrings);

    /* Step 3: Find best matching group for the bus interface hint */
    QList<QString> candidateMarkers = candidateSubstrings.keys();
    std::sort(candidateMarkers.begin(), candidateMarkers.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });

    /* Find best matching group markers for the bus interface hint */
    QVector<QString> bestHintGroupMarkers;

    /* Try to find markers using the original bus interface without hardcoding prefixes */
    QString bestMarker
        = QStaticStringWeaver::findBestGroupMarkerForHint(busInterface, candidateMarkers);
    if (!bestMarker.isEmpty()) {
        bestHintGroupMarkers.append(bestMarker);
        qDebug() << "Best matching marker:" << bestMarker << "for hint:" << busInterface;
    } else {
        /* If no marker found, use empty string */
        bestHintGroupMarkers.append("");
        qDebug() << "No suitable group marker found, using empty string";
    }

    /* Collect all module ports from groups whose keys match any of the best hint group markers */
    QVector<QString> filteredModulePorts;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QString groupKey = it.key();
        bool    matches  = false;

        for (const QString &marker : bestHintGroupMarkers) {
            if (groupKey.contains(marker, Qt::CaseInsensitive)) {
                matches = true;
                break;
            }
        }

        if (matches) {
            qDebug() << "Including ports from group:" << groupKey;
            for (const QString &portStr : it.value()) {
                filteredModulePorts.append(portStr);
            }
        }
    }

    /* If no filtered ports found, fall back to all ports */
    if (filteredModulePorts.isEmpty()) {
        qDebug() << "No ports found in matching groups, using all ports";
        filteredModulePorts = groupModule;
    } else {
        qDebug() << "Using filtered ports for matching:" << filteredModulePorts;
    }

    /* Find optimal matching between bus signals and filtered module ports */
    QMap<QString, QString> matching
        = QStaticStringWeaver::findOptimalMatching(filteredModulePorts, groupBus, bestMarker);

    /* Debug output */
    for (auto it = matching.begin(); it != matching.end(); ++it) {
        qDebug() << "Bus signal:" << it.key() << "matched with module port:" << it.value();
    }

    /* Add bus interface to module YAML */
    moduleYaml["bus"][busInterface.toStdString()]["bus"]  = busName.toStdString();
    moduleYaml["bus"][busInterface.toStdString()]["mode"] = busMode.toStdString();

    /* Add signal mappings to the bus interface */
    for (auto it = matching.begin(); it != matching.end(); ++it) {
        moduleYaml["bus"][busInterface.toStdString()]["mapping"][it.key().toStdString()]
            = it.value().toStdString();
    }

    /* Update module YAML */
    return updateModuleYaml(moduleName, moduleYaml);
}

bool QSocModuleManager::addModuleBusWithLLM(
    const QString &moduleName,
    const QString &busName,
    const QString &busMode,
    const QString &busInterface)
{
    /* Validate llmService */
    if (!llmService) {
        qCritical() << "Error: llmService is null.";
        return false;
    }

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return false;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Validate busManager */
    if (!busManager) {
        qCritical() << "Error: busManager is null.";
        return false;
    }

    /* Get bus YAML */
    YAML::Node busYaml = busManager->getBusYaml(busName);
    if (!busManager->isBusExist(busName)) {
        qCritical() << "Error: Bus does not exist:" << busName;
        return false;
    }

    /* Extract module ports from moduleYaml */
    QVector<QString> groupModule;
    if (moduleYaml["port"]) {
        for (YAML::const_iterator it = moduleYaml["port"].begin(); it != moduleYaml["port"].end();
             ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            const QString     portName    = QString::fromStdString(portNameStd);
            QString           typeInfo;
            QString           direction = "";

            if (it->second["type"]) {
                typeInfo = QString::fromStdString(it->second["type"].as<std::string>());
            }

            if (it->second["direction"]) {
                direction = QString::fromStdString(it->second["direction"].as<std::string>());
            }

            groupModule.append(direction + " " + typeInfo + " " + portName);
        }
    }

    /* Extract bus signals from busYaml */
    QVector<QString> groupBus;
    if (busYaml["port"]) {
        for (YAML::const_iterator it = busYaml["port"].begin(); it != busYaml["port"].end(); ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            groupBus.append(QString::fromStdString(portNameStd));
        }
    } else {
        /* No port node found */
        qCritical() << "Error: Bus has invalid structure (missing 'port' node):" << busName;
        return false;
    }

    qDebug() << "Module ports:" << groupModule;
    qDebug() << "Bus signals:" << groupBus;

    /* Build prompt */
    /* clang-format off */
    QString prompt = QStaticStringWeaver::stripCommonLeadingWhitespace(R"(
        I need to match bus signals to module ports based on naming conventions and semantics.

        Module name: %1
        Bus name: %2
        Module ports:
        %3

        Bus signals:
        %4

        Please provide the best mapping between bus signals and module ports.
        Consider matches related to: %5.
        For unmatched bus signals, use empty string.
        Return a JSON object where keys are bus signals and values are module ports.
    )")
    .arg(moduleName)
    .arg(busName)
    .arg(groupModule.join(", "))
    .arg(groupBus.join(", "))
    .arg(busInterface);
    /* clang-format on */

    /* Send request to LLM service */
    LLMResponse response = llmService->sendRequest(
        prompt,
        /* Default system prompt */
        "You are a helpful assistant that specializes in hardware "
        "design and bus interfaces.",
        0.2,
        true);

    /* Return error if request failed */
    if (!response.success) {
        qCritical() << "Error: LLM API request failed:" << response.errorMessage;
        return false;
    }

    /* Extract mappings from response */
    QMap<QString, QString> matching = QLLMService::extractMappingsFromResponse(response);

    if (matching.isEmpty()) {
        qCritical() << "Error: Failed to obtain mapping from LLM provider";
        return false;
    }

    /* Debug output */
    for (auto it = matching.begin(); it != matching.end(); ++it) {
        qDebug() << "Bus signal:" << it.key() << "matched with module port:" << it.value();
    }

    /* Add bus interface to module YAML */
    moduleYaml["bus"][busInterface.toStdString()]["bus"]  = busName.toStdString();
    moduleYaml["bus"][busInterface.toStdString()]["mode"] = busMode.toStdString();

    /* Add signal mappings to the bus interface */
    for (auto it = matching.begin(); it != matching.end(); ++it) {
        moduleYaml["bus"][busInterface.toStdString()]["mapping"][it.key().toStdString()]
            = it.value().toStdString();
    }

    /* Update module YAML */
    return updateModuleYaml(moduleName, moduleYaml);
}

bool QSocModuleManager::removeModuleBus(
    const QString &moduleName, const QRegularExpression &busInterfaceRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return false;
    }

    /* Validate busInterfaceRegex */
    if (!QStaticRegex::isNameRegexValid(busInterfaceRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << busInterfaceRegex.pattern();
        return false;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Check if the module has any bus interfaces defined */
    if (!moduleYaml["bus"]) {
        qDebug() << "Module doesn't have any bus interfaces:" << moduleName;
        return true; /* Return true as there's nothing to remove */
    }

    /* Keep track of whether we've removed anything */
    bool removedAny = false;

    /* Create a list of interfaces to remove (to avoid modifying during iteration) */
    std::vector<std::string> interfacesToRemove;

    /* Iterate through bus interfaces and collect ones matching the regex */
    for (YAML::const_iterator it = moduleYaml["bus"].begin(); it != moduleYaml["bus"].end(); ++it) {
        const std::string busInterfaceNameStd = it->first.as<std::string>();
        const QString     busInterfaceName    = QString::fromStdString(busInterfaceNameStd);

        if (QStaticRegex::isNameExactMatch(busInterfaceName, busInterfaceRegex)) {
            qDebug() << "Found matching bus interface to remove:" << busInterfaceName;
            interfacesToRemove.push_back(busInterfaceNameStd);
            removedAny = true;
        }
    }

    /* Remove each identified interface */
    for (const std::string &interfaceName : interfacesToRemove) {
        moduleYaml["bus"].remove(interfaceName);
    }

    /* If the bus node is now empty, remove it */
    if (moduleYaml["bus"] && moduleYaml["bus"].size() == 0) {
        moduleYaml.remove("bus");
    }

    /* Update module YAML if we made changes */
    if (removedAny) {
        return updateModuleYaml(moduleName, moduleYaml);
    }

    return true;
}

QStringList QSocModuleManager::listModuleBus(
    const QString &moduleName, const QRegularExpression &busInterfaceRegex)
{
    QStringList result;

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return result;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return result;
    }

    /* Validate busInterfaceRegex */
    if (!QStaticRegex::isNameRegexValid(busInterfaceRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << busInterfaceRegex.pattern();
        return result;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Check if the module has any bus interfaces defined */
    if (!moduleYaml["bus"]) {
        qDebug() << "Module doesn't have any bus interfaces:" << moduleName;
        return result;
    }

    /* Iterate through bus interfaces and collect interface names that match the regex */
    for (YAML::const_iterator it = moduleYaml["bus"].begin(); it != moduleYaml["bus"].end(); ++it) {
        const std::string busInterfaceNameStd = it->first.as<std::string>();
        const QString     busInterfaceName    = QString::fromStdString(busInterfaceNameStd);

        /* Check if the interface name matches the regex */
        if (QStaticRegex::isNameExactMatch(busInterfaceName, busInterfaceRegex)) {
            /* Get the bus name associated with this interface */
            if (it->second["bus"]) {
                const std::string busNameStd = it->second["bus"].as<std::string>();
                const QString     busName    = QString::fromStdString(busNameStd);

                /* Get the mode if available */
                QString mode = "unknown";
                if (it->second["mode"]) {
                    mode = QString::fromStdString(it->second["mode"].as<std::string>());
                }

                /* Format the result string: "interface_name [bus_name, mode]" */
                result.append(QString("%1 [%2, %3]").arg(busInterfaceName).arg(busName).arg(mode));
            } else {
                /* In case bus name is missing, just add the interface name */
                result.append(busInterfaceName);
            }
        }
    }

    return result;
}

YAML::Node QSocModuleManager::showModuleBus(
    const QString &moduleName, const QRegularExpression &busInterfaceRegex)
{
    YAML::Node result;

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return result;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return result;
    }

    /* Validate busInterfaceRegex */
    if (!QStaticRegex::isNameRegexValid(busInterfaceRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << busInterfaceRegex.pattern();
        return result;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Check if the module has any bus interfaces defined */
    if (!moduleYaml["bus"]) {
        qDebug() << "Module doesn't have any bus interfaces:" << moduleName;
        return result;
    }

    /* Create a "bus" node in the result */
    result["bus"] = YAML::Node(YAML::NodeType::Map);

    /* Iterate through bus interfaces and collect ones matching the regex */
    for (YAML::const_iterator it = moduleYaml["bus"].begin(); it != moduleYaml["bus"].end(); ++it) {
        const std::string busInterfaceNameStd = it->first.as<std::string>();
        const QString     busInterfaceName    = QString::fromStdString(busInterfaceNameStd);

        if (QStaticRegex::isNameExactMatch(busInterfaceName, busInterfaceRegex)) {
            qDebug() << "Found matching bus interface:" << busInterfaceName;
            result["bus"][busInterfaceNameStd] = it->second;
        }
    }

    return result;
}

QString QSocModuleManager::formatModuleBusJsonToMarkdownTable(const QString &jsonResponse)
{
    /* Try to parse the JSON response */
    try {
        json doc = json::parse(jsonResponse.toStdString());

        /* Check if the JSON has the expected structure */
        if (!doc.contains("groups") || !doc["groups"].is_array()) {
            qWarning() << "Invalid JSON structure: missing or invalid 'groups' array";
            return jsonResponse;
        }

        auto groups = doc["groups"];
        if (groups.empty()) {
            return "No potential bus interface groups found.";
        }

        /* Define table headers */
        QStringList headers
            = {"Group Name",
               "Type",
               "Data Width",
               "Address Width",
               "ID Width",
               "Burst Length",
               "Write",
               "Read"};

        /* Build table rows from JSON data */
        QVector<QStringList> rows;
        for (const auto &group : groups) {
            /* Extract values with fallbacks, handling both string and number types */
            QString name, type, wData, wAddr, wID, wLen;
            bool    enWrite = false, enRead = false;

            /* Handle name (should be string) */
            if (group.contains("name")) {
                if (group["name"].is_string()) {
                    name = QString::fromStdString(group["name"].get<std::string>());
                } else {
                    name = QString::fromStdString(group["name"].dump());
                }
            }

            /* Handle type (should be string) */
            if (group.contains("type")) {
                if (group["type"].is_string()) {
                    type = QString::fromStdString(group["type"].get<std::string>());
                } else {
                    type = QString::fromStdString(group["type"].dump());
                }
            }

            /* Handle wData (could be number or string) */
            if (group.contains("wData")) {
                if (group["wData"].is_string()) {
                    wData = QString::fromStdString(group["wData"].get<std::string>());
                } else if (group["wData"].is_number()) {
                    wData = QString::number(group["wData"].get<int>());
                } else {
                    wData = QString::fromStdString(group["wData"].dump());
                }
            }

            /* Handle wAddr (could be number or string) */
            if (group.contains("wAddr")) {
                if (group["wAddr"].is_string()) {
                    wAddr = QString::fromStdString(group["wAddr"].get<std::string>());
                } else if (group["wAddr"].is_number()) {
                    wAddr = QString::number(group["wAddr"].get<int>());
                } else {
                    wAddr = QString::fromStdString(group["wAddr"].dump());
                }
            }

            /* Handle wID (could be number or string) */
            if (group.contains("wID")) {
                if (group["wID"].is_string()) {
                    wID = QString::fromStdString(group["wID"].get<std::string>());
                } else if (group["wID"].is_number()) {
                    wID = QString::number(group["wID"].get<int>());
                } else {
                    wID = QString::fromStdString(group["wID"].dump());
                }
            }

            /* Handle wLen (could be number or string) */
            if (group.contains("wLen")) {
                if (group["wLen"].is_string()) {
                    wLen = QString::fromStdString(group["wLen"].get<std::string>());
                } else if (group["wLen"].is_number()) {
                    wLen = QString::number(group["wLen"].get<int>());
                } else {
                    wLen = QString::fromStdString(group["wLen"].dump());
                }
            }

            /* Handle boolean values */
            if (group.contains("enWrite")) {
                enWrite = group["enWrite"].get<bool>();
            }

            if (group.contains("enRead")) {
                enRead = group["enRead"].get<bool>();
            }

            /* Add row to table data */
            rows.append(
                {name, type, wData, wAddr, wID, wLen, enWrite ? "✓" : "✗", enRead ? "✓" : "✗"});
        }

        /* Use the QStaticMarkdown class to render the markdown table */
        return QStaticMarkdown::renderTable(headers, rows, QStaticMarkdown::Alignment::Left);
    } catch (const json::parse_error &e) {
        qWarning() << "Failed to parse JSON response:" << e.what();
        return jsonResponse; /* Return original response if parsing fails */
    } catch (const json::exception &e) {
        qWarning() << "JSON exception occurred:" << e.what();
        return "Error processing JSON response: " + QString(e.what());
    } catch (const std::exception &e) {
        qWarning() << "General exception occurred:" << e.what();
        return "Error: " + QString(e.what());
    }
}

bool QSocModuleManager::explainModuleBusWithLLM(
    const QString &moduleName, const QString &busName, QString &explanation)
{
    /* Validate llmService */
    if (!llmService) {
        qCritical() << "Error: llmService is null.";
        return false;
    }

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if module exists */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return false;
    }

    /* Get module YAML */
    YAML::Node moduleYaml = getModuleYaml(moduleName);

    /* Validate busManager */
    if (!busManager) {
        qCritical() << "Error: busManager is null.";
        return false;
    }

    /* Get bus YAML */
    YAML::Node busYaml = busManager->getBusYaml(busName);
    if (!busManager->isBusExist(busName)) {
        qCritical() << "Error: Bus does not exist:" << busName;
        return false;
    }

    /* Extract module ports from moduleYaml */
    QVector<QString> groupModule;
    if (moduleYaml["port"]) {
        for (YAML::const_iterator it = moduleYaml["port"].begin(); it != moduleYaml["port"].end();
             ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            const QString     portName    = QString::fromStdString(portNameStd);
            QString           typeInfo;
            QString           direction = "";

            if (it->second["type"]) {
                typeInfo = QString::fromStdString(it->second["type"].as<std::string>());
            }

            if (it->second["direction"]) {
                direction = QString::fromStdString(it->second["direction"].as<std::string>());
            }

            groupModule.append(direction + " " + typeInfo + " " + portName);
        }
    }

    /* Extract bus signals from busYaml - separate master and slave */
    QVector<QString> groupBusMaster;
    QVector<QString> groupBusSlave;

    if (busYaml["port"]) {
        for (YAML::const_iterator it = busYaml["port"].begin(); it != busYaml["port"].end(); ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            QString           portName    = QString::fromStdString(portNameStd);

            /* Check for master interface */
            if (it->second["master"] && it->second["master"].IsMap()) {
                QString portDirection = "";
                if (it->second["master"]["direction"]
                    && it->second["master"]["direction"].IsScalar()) {
                    portDirection = QString::fromStdString(
                        it->second["master"]["direction"].as<std::string>());
                }
                groupBusMaster.append(portDirection + " " + portName);
            }

            /* Check for slave interface */
            if (it->second["slave"] && it->second["slave"].IsMap()) {
                QString portDirection = "";
                if (it->second["slave"]["direction"]
                    && it->second["slave"]["direction"].IsScalar()) {
                    portDirection = QString::fromStdString(
                        it->second["slave"]["direction"].as<std::string>());
                }
                groupBusSlave.append(portDirection + " " + portName);
            }
        }
    } else {
        /* No port node found */
        qCritical() << "Error: Bus has invalid structure (missing 'port' node):" << busName;
        return false;
    }

    /* Build module ports list */
    QString portsList;
    for (const QString &port : groupModule) {
        portsList += "- " + port + "\n";
    }

    /* Build bus signals list with separate master/slave sections */
    QString signalsList = "Master Bus signals:\n";
    for (const QString &signal : groupBusMaster) {
        signalsList += "- " + signal + "\n";
    }

    signalsList += "\nSlave Bus signals:\n";
    for (const QString &signal : groupBusSlave) {
        signalsList += "- " + signal + "\n";
    }

    /* Prepare prompt for LLM */
    /* clang-format off */
    QString prompt = QStaticStringWeaver::stripCommonLeadingWhitespace(R"(
        Analyze the following module ports and bus signals to identify potential bus interface matches.

        Bus type: %1

        Module ports:
        %2

        Bus signals:
        %3

        Please analyze the signals and provide the following information ONLY for %1 bus type.
        If you don't find any matches for this specific bus type, return an empty groups array.

        Return the information in JSON format:
        {
        "groups": [
            {
            "type": "master/slave",
            "name": "short_verilog_interface_name",
            "wData": "data width",
            "wAddr": "address width",
            "wID": "ID width",
            "wLen": "burst length width",
            "enWrite": true/false,
            "enRead": true/false
            }
        ]
        }

        For the "type" field:
        1. Use "master" if the interface match the master bus signals
        2. Use "slave" if the interface match the slave bus signals

        For the "name" field:
        1. Use a short, concise name suitable for Verilog interface naming
        2. Follow Verilog naming conventions (alphanumeric with underscores)
        3. The name should reflect the function of the interface group
        4. Do not use generic names like "interface1" - use functional names
        5. "foo_bar" and "bar_foo" should be grouped together
        6. "foo_bar" and "foo_bar_baz" should be grouped together

        Please provide your analysis in the exact JSON format shown above.
    )")
    .arg(busName)
    .arg(portsList)
    .arg(signalsList);
    /* clang-format on */

    /* Send request to LLM service */
    LLMResponse response = llmService->sendRequest(
        prompt,
        /* Default system prompt */
        "You are a helpful assistant that specializes in hardware "
        "design and bus interfaces. You always respond in JSON format when requested.",
        0.2,
        true);

    /* Return error if request failed */
    if (!response.success) {
        qCritical() << "Error: LLM API request failed:" << response.errorMessage;
        return false;
    }

    /* Format the response into a markdown table */
    explanation = formatModuleBusJsonToMarkdownTable(response.content);

    return true;
}
