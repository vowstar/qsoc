#include "common/qsocmodulemanager.h"

#include "common/qslangdriver.h"
#include "common/qstaticregex.h"

#include <QDebug>
#include <QDir>
#include <QFile>

#include <fstream>

QSocModuleManager::QSocModuleManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    /* Set projectManager */
    setProjectManager(projectManager);
}

void QSocModuleManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
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
        QString    locallibraryName = libraryName;

        if (moduleNameRegex.pattern().isEmpty()) {
            /* Pick first module if pattern is empty */
            const QString &moduleName = moduleList.first();
            qDebug() << "Pick first module:" << moduleName;
            if (locallibraryName.isEmpty()) {
                /* Use first module name as library filename */
                locallibraryName = moduleName.toLower();
                qDebug() << "Pick library filename:" << locallibraryName;
            }
            const json       &moduleAst  = driver.getModuleAst(moduleName);
            const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
            /* Add module to library yaml */
            libraryYaml[moduleName.toStdString()] = moduleYaml;
            saveLibraryYaml(locallibraryName, libraryYaml);
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
                qDebug() << "Found module:" << moduleName;
                if (locallibraryName.isEmpty()) {
                    /* Use first module name as library filename */
                    locallibraryName = moduleName.toLower();
                    qDebug() << "Pick library filename:" << locallibraryName;
                }
                const json       &moduleAst           = driver.getModuleAst(moduleName);
                const YAML::Node &moduleYaml          = getModuleYaml(moduleAst);
                libraryYaml[moduleName.toStdString()] = moduleYaml;
                hasMatch                              = true;
            }
        }
        if (hasMatch) {
            saveLibraryYaml(locallibraryName, libraryYaml);
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
                    /* If memberKind is not in the set, exit the loop. */
                    break;
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

bool QSocModuleManager::isExist(const QString &libraryName)
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

bool QSocModuleManager::load(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        qCritical() << "Error: projectManager is null or invalid module path.";
        return false;
    }

    /* Check if library file exists */
    if (!isExist(libraryName)) {
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
        // dataToSave[moduleNameStd].remove("library");
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

YAML::Node QSocModuleManager::getModuleNode(const QRegularExpression &moduleNameRegex)
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
