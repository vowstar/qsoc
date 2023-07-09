#include "qsocsymbolmanager.h"

#include "qslangdriver.h"

#include <QDebug>
#include <QFile>

#include <fstream>

QSocSymbolManager::QSocSymbolManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    /* Set project manager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

bool QSocSymbolManager::importFromFileList(
    const QRegularExpression &symbolNameRegex,
    const QString            &fileListPath,
    const QStringList        &filePathList)
{
    if (!projectManager) {
        qCritical() << "Error: project manager is null.";
        return false;
    }
    Q_UNUSED(symbolNameRegex)

    QSlangDriver driver(this, projectManager);
    if (driver.parseFileList(fileListPath, filePathList)) {
        /* Parse success */
        QStringList moduleList = driver.getModuleList();
        if (moduleList.isEmpty()) {
            qCritical() << "Error: no module found.";
            return false;
        }
        /* Pick first module if pattern is empty */
        if (symbolNameRegex.pattern().isEmpty()) {
            const QString &moduleName = moduleList.first();
            qDebug() << "Pick first module:" << moduleName;
            // qDebug() << driver.getModuleAst(moduleName).dump(4).c_str();
            const json       &moduleAst  = driver.getModuleAst(moduleName);
            const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
            saveModuleYaml(moduleYaml, moduleName);
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            const QRegularExpressionMatch &match = symbolNameRegex.match(moduleName);
            if (match.hasMatch()) {
                qDebug() << "Found module:" << moduleName;
                // qDebug() << driver.getModuleAst(moduleName).dump(4).c_str();
                const json       &moduleAst  = driver.getModuleAst(moduleName);
                const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
                saveModuleYaml(moduleYaml, moduleName);
                hasMatch = true;
            }
        }
        if (hasMatch) {
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

bool QSocSymbolManager::saveModuleYaml(const YAML::Node &moduleYaml, const QString &moduleName)
{
    /* Check project manager */
    if (!projectManager) {
        qCritical() << "Error: project manager is null.";
        return false;
    }
    /* Check project path */
    if (!projectManager->isValid()) {
        qCritical() << "Error: project manager is invalid.";
        return false;
    }
    /* Check file path */
    const QString &symbolPath         = projectManager->getSymbolPath();
    const QString &moduleYamlFilePath = symbolPath + "/" + moduleName + ".soc_sym";
    /* Save YAML file */
    std::ofstream outputFileStream(moduleYamlFilePath.toStdString());
    outputFileStream << moduleYaml;
    return true;
}
