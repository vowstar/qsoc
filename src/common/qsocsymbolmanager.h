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
     * @brief Constructor
     * @details This constructor will create an instance of this object.
     * @param[in] parent parent object
     * @param[in] projectManager project manager
     */
    explicit QSocSymbolManager(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

public slots:
    /**
     * @brief Set the project manager
     * @details Assigns a new project manager to this object. The project manager
     *          is used for managing various project-related functionalities.
     * @param projectManager Pointer to the new project manager
     */
    void setProjectManager(QSocProjectManager *projectManager);
    /**
     * @brief Get the project manager
     * @details Retrieves the currently assigned project manager. This manager
     *          is responsible for handling various aspects of the project.
     * @return QSocProjectManager * Pointer to the current project manager
     */
    QSocProjectManager *getProjectManager();
    /**
     * @brief Check if symbol path is valid
     * @details Verifies the validity of the symbol path set in the project
     *          manager. Checks if the path exists and is a directory.
     * @retval true Symbol path is valid
     * @retval false Symbol path is invalid, or projectManager is nullptr
     */
    bool isSymbolPathValid();
    /**
     * @brief Import verilog files from file list
     * @details This function will import verilog files from file list, and
     *          generate the symbol library file.
     *          If symbolBasename is empty, the first matching verilog module
     *          name is automatically selected and converted into lowercase as
     *          the symbol library name.
     *          If moduleNameRegex is empty, the first matching verilog module
     *          is automatically selected for import.
     * @param symbolBasename The basename of the symbol library file without ext
     * @param moduleNameRegex Regular expression to match the module name
     * @param fileListPath The path of the verilog file list
     * @param filePathList The list of verilog files
     * @retval true Import successfully
     * @retval false Import failed
     */
    bool importFromFileList(
        const QString            &symbolBasename,
        const QRegularExpression &moduleNameRegex,
        const QString            &fileListPath,
        const QStringList        &filePathList);
    /**
     * @brief Get the Module Yaml object
     * @details This function will convert the module AST json object to YAML
     *          object. This function relies on projectManager to be valid.
     * @param moduleAst The module AST json object
     * @return YAML::Node The module YAML object
     */
    YAML::Node getModuleYaml(const json &moduleAst);
    /**
     * @brief Save the symbol YAML object to symbol file
     * @details This function will save the symbol YAML object to symbol library
     *          file.
     * @param symbolBasename The basename of the symbol library file without ext
     * @param symbolYaml The symbol YAML object
     * @retval true Save successfully
     * @retval false Save failed
     */
    bool saveSymbolYaml(const QString &symbolBasename, const YAML::Node &symbolYaml);
    /**
     * @brief Get list of symbol basenames
     * @details Retrieves basenames of ".soc_sym" files in directory defined by
     *          `symbolPath`, excluding the ".soc_sym" extension. Scans the symbol
     *          directory and compiles a list of relevant basenames. Useful for
     *          processing or iterating over project symbol files. This function
     *          relies on projectManager to be valid.
     * @param symbolBasenameRegex Regular expression to match file basenames,
     *        default is ".*".
     * @return QStringList of basenames for all ".soc_sym" files in the symbol
     *         directory, excluding the ".soc_sym" extension
     */
    QStringList listSymbol(const QRegularExpression &symbolBasenameRegex = QRegularExpression(".*"));

private:
    /* Internal used project manager */
    QSocProjectManager *projectManager = nullptr;
    /**
     * @brief Merge two YAML nodes
     * @details This function will merge two YAML nodes. It returns a new map
     *          resultYaml which is a merge of fromYaml into toYaml.
     *          Values from fromYaml will replace identically keyed non-map
     *          values from toYaml in the resultYaml map.
     *          The values ​​in toYaml and fromYaml will not be modified.
     * @param toYaml The destination YAML node
     * @param fromYaml The source YAML node
     * @return YAML::Node The merged YAML node which is a merge of fromYaml into
     *                    toYaml.
     */
    YAML::Node mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml);

signals:
};

#endif // QSOCSYMBOLMANAGER_H
