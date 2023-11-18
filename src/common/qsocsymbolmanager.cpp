#include "common/qsocsymbolmanager.h"

#include "common/qslangdriver.h"

#include <QDebug>
#include <QDir>
#include <QFile>

#include <fstream>

QSocSymbolManager::QSocSymbolManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    /* Set projectManager */
    setProjectManager(projectManager);
}

void QSocSymbolManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

QSocProjectManager *QSocSymbolManager::getProjectManager()
{
    return projectManager;
}

bool QSocSymbolManager::isSymbolPathValid()
{
    /* Validate projectManager */
    if (!projectManager) {
        qCritical() << "Error: projectManager is nullptr.";
        return false;
    }
    /* Validate symbol path. */
    if (!projectManager->isValidSymbolPath()) {
        qCritical() << "Error: Invalid symbol path:" << projectManager->getSymbolPath();
        return false;
    }
    return true;
}

YAML::Node QSocSymbolManager::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
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

bool QSocSymbolManager::importFromFileList(
    const QString            &symbolBasename,
    const QRegularExpression &moduleNameRegex,
    const QString            &fileListPath,
    const QStringList        &filePathList)
{
    /* Validate projectManager and its path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
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
        YAML::Node symbolYaml;
        QString    localSymbolBasename = symbolBasename;

        if (moduleNameRegex.pattern().isEmpty()) {
            /* Pick first module if pattern is empty */
            const QString &moduleName = moduleList.first();
            qDebug() << "Pick first module:" << moduleName;
            if (localSymbolBasename.isEmpty()) {
                /* Use first module name as symbol filename */
                localSymbolBasename = moduleName.toLower();
                qDebug() << "Pick symbol filename:" << localSymbolBasename;
            }
            const json       &moduleAst  = driver.getModuleAst(moduleName);
            const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
            /* Add module to symbol yaml */
            symbolYaml[moduleName.toStdString()] = moduleYaml;
            saveSymbolYaml(localSymbolBasename, symbolYaml);
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            const QRegularExpressionMatch &match = moduleNameRegex.match(moduleName);
            if (match.hasMatch()) {
                qDebug() << "Found module:" << moduleName;
                if (localSymbolBasename.isEmpty()) {
                    /* Use first module name as symbol filename */
                    localSymbolBasename = moduleName.toLower();
                    qDebug() << "Pick symbol filename:" << localSymbolBasename;
                }
                const json       &moduleAst          = driver.getModuleAst(moduleName);
                const YAML::Node &moduleYaml         = getModuleYaml(moduleAst);
                symbolYaml[moduleName.toStdString()] = moduleYaml;
                hasMatch                             = true;
            }
        }
        if (hasMatch) {
            saveSymbolYaml(localSymbolBasename, symbolYaml);
            return true;
        }
    }
    qCritical() << "Error: no module found.";
    return false;
}

YAML::Node QSocSymbolManager::getModuleYaml(const json &moduleAst)
{
    YAML::Node moduleYaml;
    if (moduleAst.contains("kind") && moduleAst.contains("name") && moduleAst.contains("body")
        && moduleAst["kind"] == "Instance" && moduleAst["body"].contains("members")) {
        const QString &kind = QString::fromStdString(moduleAst["kind"]);
        const QString &name = QString::fromStdString(moduleAst["name"]);
        const json    &body = moduleAst["body"];
        for (const json &member : moduleAst["body"]["members"]) {
            if (member.contains("kind") && member.contains("name") && member.contains("type")) {
                const QString &memberKind = QString::fromStdString(member["kind"]);
                const QString &memberName = QString::fromStdString(member["name"]);
                const QString &memberType = QString::fromStdString(member["type"]);
                if (memberKind == "Port") {
                    moduleYaml["port"][memberName.toStdString()]["type"] = memberType.toStdString();
                } else if (memberKind == "Parameter") {
                    moduleYaml["parameter"][memberName.toStdString()]["type"]
                        = memberType.toStdString();
                    if (member.contains("value")) {
                        const QString &memberValue = QString::fromStdString(member["value"]);
                        moduleYaml["parameter"][memberName.toStdString()]["value"]
                            = memberValue.toStdString();
                    }
                }
            }
        }
    }
    return moduleYaml;
}

bool QSocSymbolManager::saveSymbolYaml(const QString &symbolBasename, const YAML::Node &symbolYaml)
{
    YAML::Node localSymbolYaml;
    /* Validate projectManager and its path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
        return false;
    }
    /* Check file path */
    const QString &symbolPath         = projectManager->getSymbolPath();
    const QString &moduleYamlFilePath = symbolPath + "/" + symbolBasename + ".soc_sym";
    if (QFile::exists(moduleYamlFilePath)) {
        /* Load symbol YAML file */
        std::ifstream inputFileStream(moduleYamlFilePath.toStdString());
        localSymbolYaml = mergeNodes(YAML::Load(inputFileStream), symbolYaml);
        qDebug() << "Load and merge";
    } else {
        localSymbolYaml = symbolYaml;
    }

    /* Save YAML file */
    std::ofstream outputFileStream(moduleYamlFilePath.toStdString());
    outputFileStream << localSymbolYaml;
    return true;
}

QStringList QSocSymbolManager::listSymbol(const QRegularExpression &symbolBasenameRegex)
{
    QStringList result;
    /* Validate projectManager and its path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
        return result;
    }
    /* QDir for '.soc_sym' files in symbol path, sorted by name. */
    const QDir symbolPathDir(
        projectManager->getSymbolPath(),
        "*.soc_sym",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    /* Add matching file basenames from projectDir to result list. */
    foreach (const QString &filename, symbolPathDir.entryList()) {
        if (symbolBasenameRegex.match(filename).hasMatch()) {
            result.append(filename.split('.').first());
        }
    }

    return result;
}

bool QSocSymbolManager::isExist(const QString &symbolBasename)
{
    /* Validate projectManager and its symbol path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
        return false;
    }

    /* Check symbol basename */
    if (symbolBasename.isEmpty()) {
        qCritical() << "Error: Symbol basename is empty.";
        return false;
    }

    /* Get the full file path by joining symbol path and basename with extension */
    const QString filePath
        = QDir(projectManager->getSymbolPath()).filePath(symbolBasename + ".soc_sym");

    /* Check if symbol file exists */
    return QFile::exists(filePath);
}

bool QSocSymbolManager::load(const QString &symbolBasename)
{
    /* Validate projectManager and its path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
        return false;
    }

    /* Check if symbol file exists */
    if (!isExist(symbolBasename)) {
        qCritical() << "Error: Symbol file does not exist for basename:" << symbolBasename;
        return false;
    }

    /* Get the full file path by joining symbol path and basename with extension */
    const QString filePath
        = QDir(projectManager->getSymbolPath()).filePath(symbolBasename + ".soc_sym");

    /* Open the YAML file */
    std::ifstream fileStream(filePath.toStdString());
    if (!fileStream.is_open()) {
        qCritical() << "Error: Unable to open file:" << filePath;
        return false;
    }

    try {
        /* Load YAML content into a temporary node */
        YAML::Node tempNode = YAML::Load(fileStream);

        /* Iterate through the temporary node and add to symbolLib */
        for (YAML::const_iterator it = tempNode.begin(); it != tempNode.end(); ++it) {
            const auto key = it->first.as<std::string>();

            /* Add to symbolLib */
            symbolLib[key] = it->second;

            /* Update symbolMap with symbolBasename to key mapping */
            symbolMap[symbolBasename] = QString::fromStdString(key);
        }
    } catch (const YAML::Exception &e) {
        qCritical() << "Error parsing YAML file:" << filePath << ":" << e.what();
        return false;
    }

    return true;
}

bool QSocSymbolManager::load(const QRegularExpression &symbolBasenameRegex)
{
    /* Validate projectManager */
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    /* Get the list of symbol basenames matching the regex */
    const QStringList matchingBasenames = listSymbol(symbolBasenameRegex);

    /* Iterate through the list and load each symbol */
    for (const QString &basename : matchingBasenames) {
        if (!load(basename)) {
            qCritical() << "Error: Failed to load symbol:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocSymbolManager::remove(const QString &symbolBasename)
{
    /* Validate projectManager and its symbol path */
    if (!isSymbolPathValid()) {
        qCritical() << "Error: projectManager is null or invalid symbol path.";
        return false;
    }

    /* Check symbol basename */
    if (symbolBasename.isEmpty()) {
        qCritical() << "Error: Symbol basename is empty.";
        return false;
    }

    /* Get the full file path */
    const QString filePath
        = QDir(projectManager->getSymbolPath()).filePath(symbolBasename + ".soc_sym");

    /* Check if symbol file exists */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: Symbol file does not exist for basename:" << symbolBasename;
        return false;
    }

    /* Remove the file */
    if (!QFile::remove(filePath)) {
        qCritical() << "Error: Failed to remove symbol file:" << filePath;
        return false;
    }

    /* Remove from symbolLib and symbolMap */
    symbolLib.remove(symbolBasename.toStdString());
    symbolMap.remove(symbolBasename);

    return true;
}

bool QSocSymbolManager::remove(const QRegularExpression &symbolBasenameRegex)
{
    /* Validate projectManager */
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    /* Get the list of symbol basenames matching the regex */
    const QStringList matchingBasenames = listSymbol(symbolBasenameRegex);

    /* Iterate through the list and remove each symbol */
    for (const QString &basename : matchingBasenames) {
        if (!remove(basename)) {
            qCritical() << "Error: Failed to remove symbol:" << basename;
            return false;
        }
    }

    return true;
}

QStringList QSocSymbolManager::listModule(const QRegularExpression &moduleNameRegex)
{
    QStringList result;

    /* Iterate through each node in symbolLib */
    for (YAML::const_iterator it = symbolLib.begin(); it != symbolLib.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (moduleNameRegex.match(moduleName).hasMatch()) {
            result.append(moduleName);
        }
    }

    return result;
}

bool QSocSymbolManager::removeModule(const QRegularExpression &moduleNameRegex)
{
    /* Validate projectManager */
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    /* List to keep track of symbol basenames and module names for removal */
    QSet<QString>    symbolsToRemove;
    QVector<QString> modulesToRemove;

    /* Iterate through symbolLib to find matching modules */
    for (YAML::const_iterator it = symbolLib.begin(); it != symbolLib.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if module name matches the regex */
        if (moduleNameRegex.match(moduleName).hasMatch()) {
            /* Mark module for removal */
            modulesToRemove.append(moduleName);

            /* Find and mark symbol basename for removal */
            for (auto symbolIt = symbolMap.begin(); symbolIt != symbolMap.end();) {
                if (symbolIt.value() == moduleName) {
                    symbolsToRemove.insert(symbolIt.key());
                    symbolIt = symbolMap.erase(symbolIt);
                } else {
                    ++symbolIt;
                }
            }
        }
    }

    /* Remove marked modules from symbolLib */
    for (const QString &moduleName : modulesToRemove) {
        symbolLib.remove(moduleName.toStdString());
    }

    /* Remove symbols if they have no remaining mappings in symbolMap */
    for (const QString &symbolBasename : symbolsToRemove) {
        if (!symbolMap.contains(symbolBasename)) {
            /* Remove symbol file */
            if (!remove(symbolBasename)) {
                qCritical() << "Error: Failed to remove symbol:" << symbolBasename;
                return false;
            }
        }
    }

    return true;
}
