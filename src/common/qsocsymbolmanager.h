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
    /**
     * @brief Check if the symbol file exists in the filesystem
     * @details This function checks if a symbol file with the given basename
     *          exists in the symbol directory. It is used to verify the
     *          existence of symbol files before processing them.
     * @param symbolBasename The basename of the symbol file, without the
     *        ".soc_sym" extension.
     * @retval true Symbol file exists in the symbol directory
     * @retval false Symbol file does not exist in the symbol directory
     */
    bool isExist(const QString &symbolBasename);
    /**
     * @brief Load a specific symbol by basename
     * @details Loads symbol specified by `symbolBasename` from the symbol
     *          directory. Useful for retrieving individual symbol files.
     *          Relies on a valid projectManager and existence of the symbol
     *          file.
     * @param symbolBasename Basename of symbol file to load, without
     *        ".soc_sym" extension
     * @retval true Symbol is successfully loaded
     * @retval false Loading fails or file does not exist
     */
    bool load(const QString &symbolBasename);
    /**
     * @brief Load symbols matching a regex pattern
     * @details Loads symbol files matching `symbolBasenameRegex` in the symbol
     *          directory. Ideal for batch processing or retrieving symbols
     *          based on naming patterns. Requires projectManager to be valid.
     * @param symbolBasenameRegex Regex to match file basenames, defaults to
     *        ".*", matching all symbols
     * @retval true All matching symbols are loaded
     * @retval false Loading any matching symbols fails
     */
    bool load(const QRegularExpression &symbolBasenameRegex = QRegularExpression(".*"));
    /**
     * @brief Remove a specific symbol by basename
     * @details Removes the symbol file identified by `symbolBasename` from
     *          the symbol directory. This method is useful for deleting
     *          individual symbol files. It requires a valid projectManager
     *          and checks if the symbol file exists.
     * @param symbolBasename Basename of the symbol file to remove, without
     *        the ".soc_sym" extension.
     * @retval true The symbol file is successfully removed
     * @retval false Removal fails or the file does not exist
     */
    bool remove(const QString &symbolBasename);
    /**
     * @brief Remove symbols matching a regex pattern
     * @details Removes all symbol files from the symbol directory that
     *          match `symbolBasenameRegex`. This method is ideal for batch
     *          removal of symbols based on naming patterns. It relies on
     *          a valid projectManager to execute.
     * @param symbolBasenameRegex Regex to match file basenames. Defaults
     *        to ".*", which matches all symbol files.
     * @retval true if all matching symbols are successfully removed
     * @retval false if removal of any matching symbols fails
     */
    bool remove(const QRegularExpression &symbolBasenameRegex = QRegularExpression(".*"));
    /**
     * @brief Get list of module names matching a regex pattern
     * @details Retrieves module names from the `symbolLib` YAML node that
     *          match the provided `moduleNameRegex`. This function scans
     *          the symbol library and compiles a list of module names. Useful
     *          for processing or iterating over project modules. It relies on
     *          the validity of the `symbolLib` YAML node.
     * @param moduleNameRegex Regular expression to match module names,
     *        default is ".*".
     * @return QStringList of module names matching the regex in the symbol
     *         library
     */
    QStringList listModule(const QRegularExpression &moduleNameRegex = QRegularExpression(".*"));

    /**
     * @brief Remove modules matching a regex pattern
     * @details Removes modules from the symbol library that match the
     *          provided `moduleNameRegex`. This method is suitable for batch
     *          removal of modules based on naming patterns. It updates the
     *          symbol library and symbol map, and removes symbol files if their
     *          last mapping in the symbol map is deleted. Requires a valid
     *          projectManager for execution.
     * @param moduleNameRegex Regex to match module names. Defaults to
     *        ".*", matching all modules.
     * @retval true All matching modules are successfully removed
     * @retval false Removal of any matching modules fails
     */
    bool removeModule(const QRegularExpression &moduleNameRegex = QRegularExpression(".*"));

private:
    /* Internal used project manager */
    QSocProjectManager *projectManager = nullptr;
    /* Symbol and module name map */
    QMap<QString, QString> symbolMap;
    /* Symbol library YAML node */
    YAML::Node symbolLib;
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
