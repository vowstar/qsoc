#include "common/qsocmodulemanager.h"

#include "common/qslangdriver.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qstaticregex.h"
#include "common/qstaticstringweaver.h"

#include <QDebug>
#include <QDir>
#include <QFile>

#include <fstream>

QSocModuleManager::QSocModuleManager(
    QObject            *parent,
    QSocProjectManager *projectManager,
    QSocBusManager     *busManager,
    QLLMService        *llmService)
    : QObject(parent)
    , projectManager(projectManager)
    , busManager(busManager)
    , llmService(llmService)
{
    /* Set projectManager */
    setProjectManager(projectManager);
    /* Set busManager */
    setBusManager(busManager);
}

void QSocModuleManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

void QSocModuleManager::setBusManager(QSocBusManager *busManager)
{
    /* Set busManager */
    if (busManager) {
        this->busManager = busManager;
    }
}

QSocProjectManager *QSocModuleManager::getProjectManager()
{
    return projectManager;
}

bool QSocModuleManager::isModulePathValid()
{
    /* Validate projectManager */
    if (!projectManager) {
        qCritical() << "Error: projectManager is nullptr.";
        return false;
    }
    /* Validate module path. */
    if (!projectManager->isValidModulePath()) {
        qCritical() << "Error: Invalid module path:" << projectManager->getModulePath();
        return false;
    }
    return true;
}

bool QSocModuleManager::importFromFileList(
    const QString            &libraryName,
    const QRegularExpression &moduleNameRegex,
    const QString            &fileListPath,
    const QStringList        &filePathList)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << moduleNameRegex.pattern();
        return false;
    }

    QSlangDriver driver(this, projectManager);
    if (driver.parseFileList(fileListPath, filePathList)) {
        /* Parse success */
        QStringList moduleList = driver.getModuleList();
        if (moduleList.isEmpty()) {
            /* No module found */
            qCritical() << "Error: no module found.";
            return false;
        }
        YAML::Node libraryYaml;
        QString    effectiveName = libraryName;

        if (moduleNameRegex.pattern().isEmpty()) {
            /* Pick first module if pattern is empty */
            const QString &moduleName = moduleList.first();
            qDebug() << "Pick first module:" << moduleName;
            if (effectiveName.isEmpty()) {
                effectiveName = moduleName.toLower();
                qDebug() << "Pick library filename:" << effectiveName;
            }
            const json       &moduleAst  = driver.getModuleAst(moduleName);
            const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
            /* Add module to library yaml */
            libraryYaml[moduleName.toStdString()] = moduleYaml;
            saveLibraryYaml(effectiveName, libraryYaml);
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
                qDebug() << "Found module:" << moduleName;
                if (effectiveName.isEmpty()) {
                    /* Use first module name as library filename */
                    effectiveName = moduleName.toLower();
                    qDebug() << "Pick library filename:" << effectiveName;
                }
                const json       &moduleAst           = driver.getModuleAst(moduleName);
                const YAML::Node &moduleYaml          = getModuleYaml(moduleAst);
                libraryYaml[moduleName.toStdString()] = moduleYaml;
                hasMatch                              = true;
            }
        }
        if (hasMatch) {
            saveLibraryYaml(effectiveName, libraryYaml);
            return true;
        }
    }
    qCritical() << "Error: no module found.";
    return false;
}

YAML::Node QSocModuleManager::getModuleYaml(const json &moduleAst)
{
    YAML::Node moduleYaml;
    /* Check if the module AST contains the required fields */
    if (moduleAst.contains("kind") && moduleAst.contains("name") && moduleAst.contains("body")
        && moduleAst["kind"] == "Instance" && moduleAst["body"].contains("members")) {
        /* Extract the module's kind and name */
        const QString &kind = QString::fromStdString(moduleAst["kind"]);
        const QString &name = QString::fromStdString(moduleAst["name"]);
        const json    &body = moduleAst["body"];
        /* Set of valid member kinds */
        const QSet<QString> validKind = {"port", "parameter"};
        /* Iterate through each member in the AST */
        for (const json &member : moduleAst["body"]["members"]) {
            /* Check if the member contains the necessary fields */
            if (member.contains("kind") && member.contains("name") && member.contains("type")) {
                const QString     &memberKind    = QString::fromStdString(member["kind"]).toLower();
                const QString     &memberName    = QString::fromStdString(member["name"]);
                const QString     &memberType    = QString::fromStdString(member["type"]).toLower();
                const std::string &memberKindStd = memberKind.toStdString();
                const std::string &memberNameStd = memberName.toStdString();
                const std::string &memberTypeStd = memberType.toStdString();
                /* Check if memberKind is within the valid kinds */
                if (!validKind.contains(memberKind)) {
                    /* If memberKind is not in the set, skip to next iteration */
                    continue;
                }
                /* Add member information to the YAML node */
                moduleYaml[memberKindStd][memberNameStd]["type"] = memberTypeStd;
                /* Check for and add the direction of the member if present */
                if (member.contains("direction")) {
                    const QString &memberDirection
                        = QString::fromStdString(member["direction"]).toLower();
                    const std::string &memberDirectionStd = memberDirection.toStdString();

                    moduleYaml[memberKindStd][memberNameStd]["direction"] = memberDirectionStd;
                }
                /* Check for and add the value of the member if present */
                if (member.contains("value")) {
                    const QString     &memberValue    = QString::fromStdString(member["value"]);
                    const std::string &memberValueStd = memberValue.toStdString();

                    moduleYaml[memberKindStd][memberNameStd]["value"] = memberValueStd;
                }
            }
        }
    }
    return moduleYaml;
}

YAML::Node QSocModuleManager::getModuleYaml(const QString &moduleName)
{
    YAML::Node result;

    /* Check if module exists in moduleData */
    if (!isModuleExist(moduleName)) {
        qWarning() << "Module does not exist:" << moduleName;
        return result;
    }

    /* Get module YAML node from moduleData */
    result = moduleData[moduleName.toStdString()];

    return result;
}

bool QSocModuleManager::saveLibraryYaml(const QString &libraryName, const YAML::Node &libraryYaml)
{
    YAML::Node localLibraryYaml;
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Check file path */
    const QString &modulePath = projectManager->getModulePath();
    const QString &filePath   = modulePath + "/" + libraryName + ".soc_mod";
    if (QFile::exists(filePath)) {
        /* Load library YAML file */
        std::ifstream inputFileStream(filePath.toStdString());
        localLibraryYaml = mergeNodes(YAML::Load(inputFileStream), libraryYaml);
        qDebug() << "Load and merge";
    } else {
        localLibraryYaml = libraryYaml;
    }

    /* Save YAML file */
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        qCritical() << "Error: Unable to open file for writing:" << filePath;
        return false;
    }
    outputFileStream << localLibraryYaml;
    return true;
}

bool QSocModuleManager::isLibraryFileExist(const QString &libraryName)
{
    /* Validate projectManager and its module path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        qCritical() << "Error: library basename is empty.";
        return false;
    }

    /* Get the full file path by joining module path and basename with extension */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Check if library file exists */
    return QFile::exists(filePath);
}

bool QSocModuleManager::isLibraryExist(const QString &libraryName)
{
    return libraryMap.contains(libraryName);
}

QStringList QSocModuleManager::listLibrary(const QRegularExpression &libraryNameRegex)
{
    QStringList result;
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return result;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return result;
    }
    /* QDir for '.soc_mod' files in module path, sorted by name. */
    const QDir modulePathDir(
        projectManager->getModulePath(),
        "*.soc_mod",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    /* Add matching file basenames from projectDir to result list. */
    foreach (const QString &filename, modulePathDir.entryList()) {
        if (QStaticRegex::isNameExactMatch(filename, libraryNameRegex)) {
            result.append(filename.split('.').first());
        }
    }

    return result;
}

bool QSocModuleManager::load(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if library file exists */
    if (!isLibraryFileExist(libraryName)) {
        qCritical() << "Error: Library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Get the full file path by joining module path and basename with extension */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Open the YAML file */
    std::ifstream fileStream(filePath.toStdString());
    if (!fileStream.is_open()) {
        qCritical() << "Error: Unable to open file:" << filePath;
        return false;
    }

    try {
        /* Load YAML content into a temporary node */
        YAML::Node tempNode = YAML::Load(fileStream);

        /* Iterate through the temporary node and add to moduleData */
        for (YAML::const_iterator it = tempNode.begin(); it != tempNode.end(); ++it) {
            const auto key = it->first.as<std::string>();

            /* Add to moduleData */
            moduleData[key]            = it->second;
            moduleData[key]["library"] = libraryName.toStdString();

            /* Update libraryMap with libraryName to key mapping */
            libraryMapAdd(libraryName, QString::fromStdString(key));
        }
    } catch (const YAML::Exception &e) {
        qCritical() << "Error parsing YAML file:" << filePath << ":" << e.what();
        return false;
    }

    return true;
}

bool QSocModuleManager::load(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and load each library */
    for (const QString &basename : matchingBasenames) {
        if (!load(basename)) {
            qCritical() << "Error: Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::load(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!load(basename)) {
            qCritical() << "Error: Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::save(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid library path.";
        return false;
    }

    /* Check if the libraryName exists in libraryMap */
    if (!libraryMap.contains(libraryName)) {
        qCritical() << "Error: Library basename not found in libraryMap.";
        return false;
    }

    /* Extract modules from moduleData */
    YAML::Node dataToSave;
    /* Iterate through each module in libraryMap */
    for (const auto &moduleItem : libraryMap[libraryName]) {
        const std::string moduleNameStd = moduleItem.toStdString();
        if (!moduleData[moduleNameStd]) {
            qCritical() << "Error: Module data is not exist: " << moduleNameStd;
            return false;
        }
        dataToSave[moduleNameStd] = moduleData[moduleNameStd];
        if (dataToSave[moduleNameStd]["library"]) {
            dataToSave[moduleNameStd].remove("library");
        }
    }

    /* Serialize and save to file */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        qCritical() << "Error: Unable to open file for writing:" << filePath;
        return false;
    }
    outputFileStream << dataToSave;
    return true;
}

bool QSocModuleManager::save(const QRegularExpression &libraryNameRegex)
{
    bool allSaved = true;

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Iterate over libraryMap and save matching libraries */
    for (const QString &libraryName : libraryMap.keys()) {
        if (QStaticRegex::isNameExactMatch(libraryName, libraryNameRegex)) {
            if (!save(libraryName)) {
                qCritical() << "Error: Failed to save library:" << libraryName;
                allSaved = false;
            }
        }
    }

    return allSaved;
}

bool QSocModuleManager::save(const QStringList &libraryNameList)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!save(basename)) {
            qCritical() << "Error: Failed to save library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::remove(const QString &libraryName)
{
    /* Validate projectManager and its module path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        qCritical() << "Error: library basename is empty.";
        return false;
    }

    /* Get the full file path */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Check if library file exists */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Remove the file */
    if (!QFile::remove(filePath)) {
        qCritical() << "Error: Failed to remove module file:" << filePath;
        return false;
    }

    /* Remove from moduleData and libraryMap */
    moduleData.remove(libraryName.toStdString());
    libraryMap.remove(libraryName);

    return true;
}

bool QSocModuleManager::remove(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and remove each library */
    for (const QString &basename : matchingBasenames) {
        if (!remove(basename)) {
            qCritical() << "Error: Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::remove(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!remove(basename)) {
            qCritical() << "Error: Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::isModuleExist(const QString &moduleName)
{
    return moduleData[moduleName.toStdString()].IsDefined();
}

QString QSocModuleManager::getModuleLibrary(const QString &moduleName)
{
    if (!isModuleExist(moduleName)) {
        return QString();
    }
    return QString::fromStdString(moduleData[moduleName.toStdString()]["library"].as<std::string>());
}

QStringList QSocModuleManager::listModule(const QRegularExpression &moduleNameRegex)
{
    QStringList result;
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << moduleNameRegex.pattern();
        return result;
    }

    /* Iterate through each node in moduleData */
    for (YAML::const_iterator it = moduleData.begin(); it != moduleData.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            result.append(moduleName);
        }
    }

    return result;
}

YAML::Node QSocModuleManager::getModuleYamls(const QRegularExpression &moduleNameRegex)
{
    YAML::Node result;

    /* Check if the regex is valid, if not, return an empty node */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        qWarning() << "Invalid regular expression provided.";
        return result;
    }

    /* Iterate over the moduleData to find matches */
    for (YAML::const_iterator it = moduleData.begin(); it != moduleData.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            /* Add the module node to the result */
            result[moduleName.toStdString()] = it->second;
        }
    }

    return result;
}

bool QSocModuleManager::updateModuleYaml(const QString &moduleName, const YAML::Node &moduleYaml)
{
    /* Check if module exists in a library */
    if (!isModuleExist(moduleName)) {
        qCritical() << "Error: Module does not exist:" << moduleName;
        return false;
    }

    /* Get the library name for this module */
    const QString libraryName = getModuleLibrary(moduleName);
    if (libraryName.isEmpty()) {
        qCritical() << "Error: Could not find library for module:" << moduleName;
        return false;
    }

    /* Update module data */
    moduleData[moduleName.toStdString()]            = moduleYaml;
    moduleData[moduleName.toStdString()]["library"] = libraryName.toStdString();

    /* Save the updated library */
    return save(libraryName);
}

bool QSocModuleManager::removeModule(const QRegularExpression &moduleNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << moduleNameRegex.pattern();
        return false;
    }

    QSet<QString> libraryToSave;
    QSet<QString> libraryToRemove;
    QSet<QString> moduleToRemove;

    for (auto moduleDataIter = moduleData.begin(); moduleDataIter != moduleData.end();
         ++moduleDataIter) {
        const QString moduleName = QString::fromStdString(moduleDataIter->first.as<std::string>());
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            moduleToRemove.insert(moduleName);
            const QString libraryName = QString::fromStdString(
                moduleDataIter->second["library"].as<std::string>());
            libraryToSave.insert(libraryName);
        }
    }

    /* Remove modules from moduleData */
    for (const QString &moduleName : moduleToRemove) {
        const QString libraryName = QString::fromStdString(
            moduleData[moduleName.toStdString()]["library"].as<std::string>());
        libraryMapRemove(libraryName, moduleName);
        if (!libraryMap.contains(libraryName)) {
            libraryToRemove.insert(libraryName);
        }
        moduleData.remove(moduleName.toStdString());
    }

    /* Ensure libraryToSave does not include modules marked for removal */
    for (const QString &libraryName : libraryToRemove) {
        libraryToSave.remove(libraryName);
    }

    const QStringList libraryToSaveList = QList<QString>(libraryToSave.begin(), libraryToSave.end());
    const QStringList libraryToRemoveList
        = QList<QString>(libraryToRemove.begin(), libraryToRemove.end());

    /* Save libraries that still have associations in libraryMap */
    if (!save(libraryToSaveList)) {
        qCritical() << "Error: Failed to save libraries.";
        return false;
    }

    /* Remove modules with no remaining associations in libraryMap */
    if (!remove(libraryToRemoveList)) {
        qCritical() << "Error: Failed to remove modules.";
        return false;
    }

    return true;
}

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
    if (!busYaml) {
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
    if (!busYaml) {
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

    qDebug() << "Module ports:" << groupModule;
    qDebug() << "Bus signals:" << groupBus;

    /* Build prompt */
    QString prompt
        = QString(
              "I need to match bus signals to module ports based on naming conventions and "
              "semantics.\n\n"
              "Module name: %1\n"
              "Bus name: %2\n"
              "Module ports:\n%4\n\n"
              "Bus signals:\n%5\n\n"
              "Please provide the best mapping between bus signals and module ports. "
              "Consider matches related to: %3.\n"
              "For unmatched bus signals, use empty string."
              "Return a JSON object where keys are bus signals and values are module ports. ")
              .arg(moduleName)
              .arg(busName)
              .arg(busInterface)
              .arg(groupModule.join(", "))
              .arg(groupBus.join(", "));

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

YAML::Node QSocModuleManager::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
{
    if (!fromYaml.IsMap()) {
        /* If fromYaml is not a map, merge result is fromYaml, unless fromYaml is null */
        return fromYaml.IsNull() ? toYaml : fromYaml;
    }
    if (!toYaml.IsMap()) {
        /* If toYaml is not a map, merge result is fromYaml */
        return fromYaml;
    }
    if (!fromYaml.size()) {
        /* If toYaml is a map, and fromYaml is an empty map, return toYaml */
        return toYaml;
    }
    /* Create a new map 'resultYaml' with the same mappings as toYaml, merged with fromYaml */
    YAML::Node resultYaml = YAML::Node(YAML::NodeType::Map);
    for (auto iter : toYaml) {
        if (iter.first.IsScalar()) {
            const std::string &key      = iter.first.Scalar();
            auto               tempYaml = YAML::Node(fromYaml[key]);
            if (tempYaml) {
                resultYaml[iter.first] = mergeNodes(iter.second, tempYaml);
                continue;
            }
        }
        resultYaml[iter.first] = iter.second;
    }
    /* Add the mappings from 'fromYaml' not already in 'resultYaml' */
    for (auto iter : fromYaml) {
        if (!iter.first.IsScalar() || !resultYaml[iter.first.Scalar()]) {
            resultYaml[iter.first] = iter.second;
        }
    }
    return resultYaml;
}

void QSocModuleManager::libraryMapAdd(const QString &libraryName, const QString &moduleName)
{
    /* Check if the library exists in the map */
    if (!libraryMap.contains(libraryName)) {
        /* If the key doesn't exist, create a new QSet and insert the moduleName */
        QSet<QString> moduleSet;
        moduleSet.insert(moduleName);
        libraryMap.insert(libraryName, moduleSet);
    } else {
        /* If the key exists, just add the moduleName to the existing QSet */
        libraryMap[libraryName].insert(moduleName);
    }
}

void QSocModuleManager::libraryMapRemove(const QString &libraryName, const QString &moduleName)
{
    /* Check if the library exists in the map */
    if (libraryMap.contains(libraryName)) {
        QSet<QString> &modules = libraryMap[libraryName];

        /* Remove the module if it exists in the set */
        modules.remove(moduleName);

        /* If the set becomes empty after removal, delete the library entry */
        if (modules.isEmpty()) {
            libraryMap.remove(libraryName);
        }
    }
}

void QSocModuleManager::setLLMService(QLLMService *llmService)
{
    this->llmService = llmService;
}

QLLMService *QSocModuleManager::getLLMService()
{
    return llmService;
}

QSocBusManager *QSocModuleManager::getBusManager()
{
    return busManager;
}
