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
    if (projectManager) {
        this->projectManager = projectManager;
    }
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
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: projectManager is null or invalid.";
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
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: projectManager is null or invalid.";
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
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: projectManager is null or invalid.";
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
