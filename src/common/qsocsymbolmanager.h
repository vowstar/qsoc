#ifndef QSOCSYMBOLMANAGER_H
#define QSOCSYMBOLMANAGER_H

#include "common/qsocprojectmanager.h"

#include <QObject>
#include <QRegularExpression>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

/**
 * @brief   The QSocSymbolManager class
 * @details This class is used to manage the symbol files.
 */
class QSocSymbolManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief   Constructor
     * @param   parent parent object
     * @param   projectManager project manager
     * @details This constructor will create an instance of this object.
     */
    explicit QSocSymbolManager(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

public slots:
    /**
     * @brief   Import symbol files from file list
     * @param   symbolNameRegex regular expression to match the symbol name
     * @param   directoryPath directory path
     * @details This function will import symbol files from file list.
     * @return  bool true if import successfully, otherwise false
     */
    bool importFromFileList(
        const QRegularExpression &symbolNameRegex,
        const QString            &fileListPath,
        const QStringList        &filePathList);
    /**
     * @brief Get the Module Yaml object
     *
     * @param moduleAst The module AST json object
     * @return YAML::Node The module YAML object
     * @details This function will convert the module AST json object to YAML object
     */
    YAML::Node getModuleYaml(const json &moduleAst);
    /**
     * @brief Save the module YAML object to file
     *
     * @param moduleYaml The module YAML object
     * @param moduleName The module name
     * @return true if save successfully, otherwise false
     * @details This function will save the module YAML object to file
     */
    bool saveModuleYaml(const YAML::Node &moduleYaml, const QString &moduleName);

private:
    QSocProjectManager *projectManager = nullptr;

signals:
};

#endif // QSOCSYMBOLMANAGER_H
