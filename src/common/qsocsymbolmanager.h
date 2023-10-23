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
     * @brief   Import verilog files from file list
     * @param   symbolFilename the name of the symbol library file
     * @param   moduleNameRegex regular expression to match the module name
     * @param   fileListPath the path of the verilog file list
     * @param   filePathList the list of verilog files
     * @return  bool true if import successfully, otherwise false
     * @details This function will import verilog files from file list, and
     *          generate the symbol library file. 
     *          If symbolFilename is empty, the first matching verilog module
     *          name is automatically selected and converted into lowercase as
     *          the symbol library name.
     *          If moduleNameRegex is empty, the first matching verilog module
     *          is automatically selected for import.
     */
    bool importFromFileList(
        const QString            &symbolFilename,
        const QRegularExpression &moduleNameRegex,
        const QString            &fileListPath,
        const QStringList        &filePathList);
    /**
     * @brief Get the Module Yaml object
     *
     * @param moduleAst The module AST json object
     * @return YAML::Node The module YAML object
     * @details This function will convert the module AST json object to YAML 
     *          object.
     */
    YAML::Node getModuleYaml(const json &moduleAst);
    /**
     * @brief Save the symbol YAML object to symbol file
     *
     * @param symbolYaml The symbol YAML object
     * @param symbolFilename The symbolFilename
     * @return true if save successfully, otherwise false
     * @details This function will save the symbol YAML object to symbol library
     *          file.
     */
    bool saveSymbolYaml(const YAML::Node &symbolYaml, const QString &symbolFilename);

private:
    /* Internal used project manager */
    QSocProjectManager *projectManager = nullptr;
    /**
     * @brief Merge two YAML nodes
     *
     * @param toYaml The destination YAML node
     * @param fromYaml The source YAML node
     * @return YAML::Node The merged YAML node which is a merge of fromYaml into
     *                    toYaml.
     * @details This function will merge two YAML nodes. It returns a new map
     *          resultYaml which is a merge of fromYaml into toYaml.
     *          Values from fromYaml will replace identically keyed non-map
     *          values from toYaml in the resultYaml map.
     *          The values ​​in toYaml and fromYaml will not be modified.
     */
    YAML::Node mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml);

signals:
};

#endif // QSOCSYMBOLMANAGER_H
