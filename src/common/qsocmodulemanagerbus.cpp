// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocmodulemanager.h"
#include "common/qstaticmarkdown.h"
#include "common/qstaticregex.h"
#include "common/qstaticstringweaver.h"

#include <QDebug>

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
            groupModule.append(QString::fromStdString(portNameStd));
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
            groupModule.append(QString::fromStdString(portNameStd));
        }
    }

    /* Extract bus signals from busYaml */
    QVector<QString> groupBus;
    if (busYaml["port"]) {
        for (YAML::const_iterator it = busYaml["port"].begin(); it != busYaml["port"].end(); ++it) {
            const std::string portNameStd = it->first.as<std::string>();
            groupBus.append(QString::fromStdString(portNameStd));
        }
    }

    /* Build module ports list */
    QString portsList;
    for (const QString &port : groupModule) {
        portsList += "- " + port + "\n";
    }

    /* Build bus signals list */
    QString signalsList;
    for (const QString &signal : groupBus) {
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
        1. Use "master" if the interface is a master interface
        2. Use "slave" if the interface is a slave interface

        For the "name" field:
        1. Use a short, concise name suitable for Verilog interface naming
        2. Follow Verilog naming conventions (alphanumeric with underscores)
        3. The name should reflect the function of the interface group
        4. Do not use generic names like "interface1" - use functional names

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
    explanation = QStaticMarkdown::formatJsonToMarkdownTable(response.content);

    return true;
}
