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
     *          If libraryName is empty, the first matching verilog module
     *          name is automatically selected and converted into lowercase as
     *          the symbol library name.
     *          If moduleNameRegex is empty, the first matching verilog module
     *          is automatically selected for import.
     * @param libraryName The basename of the symbol library file without ext
     * @param moduleNameRegex Regular expression to match the module name
     * @param fileListPath The path of the verilog file list
     * @param filePathList The list of verilog files
     * @retval true Import successfully
     * @retval false Import failed
     */
    bool importFromFileList(
        const QString            &libraryName,
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
     * @param libraryName The basename of the symbol library file without ext
     * @param symbolYaml The symbol YAML object
     * @retval true Save successfully
     * @retval false Save failed
     */
    bool saveSymbolYaml(const QString &libraryName, const YAML::Node &symbolYaml);
    /**
     * @brief Get list of symbol basenames
     * @details Retrieves basenames of ".soc_sym" files in directory defined by
     *          `symbolPath`, excluding the ".soc_sym" extension. Scans the symbol
     *          directory and compiles a list of relevant basenames. Useful for
     *          processing or iterating over project symbol files. This function
     *          relies on projectManager to be valid.
     * @param libraryNameRegex Regular expression to match file basenames,
     *        default is ".*".
     * @return QStringList of basenames for all ".soc_sym" files in the symbol
     *         directory, excluding the ".soc_sym" extension
     */
    QStringList listLibrary(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Check if the symbol file exists in the filesystem
     * @details This function checks if a symbol file with the given basename
     *          exists in the symbol directory. It is used to verify the
     *          existence of symbol files before processing them.
     * @param libraryName The basename of the symbol file, without the
     *        ".soc_sym" extension.
     * @retval true Symbol file exists in the symbol directory
     * @retval false Symbol file does not exist in the symbol directory
     */
    bool isExist(const QString &libraryName);
    /**
     * @brief Load a specific symbol by basename
     * @details Loads symbol specified by `libraryName` from the symbol
     *          directory. Useful for retrieving individual symbol files.
     *          Relies on a valid projectManager and existence of the symbol
     *          file.
     * @param libraryName Basename of symbol file to load, without
     *        ".soc_sym" extension
     * @retval true Symbol is successfully loaded
     * @retval false Loading fails or file does not exist
     */
    bool load(const QString &libraryName);
    /**
     * @brief Load symbols matching a regex pattern
     * @details Loads symbol files matching `libraryNameRegex` in the symbol
     *          directory. Ideal for batch processing or retrieving symbols
     *          based on naming patterns. Requires projectManager to be valid.
     * @param libraryNameRegex Regex to match file basenames, defaults to
     *        ".*", matching all symbols
     * @retval true All matching symbols are loaded
     * @retval false Loading any matching symbols fails
     */
    bool load(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Load multiple symbols by a list of basenames
     * @details Loads multiple symbols specified in `libraryNameList` from the
     *          symbol directory. Useful for loading a specific set of symbol files.
     *          Requires a valid projectManager and checks the existence of each
     *          symbol file.
     * @param libraryNameList List of symbol file basenames to load, without
     *        ".soc_sym" extensions.
     * @retval true All specified symbols are successfully loaded
     * @retval false Loading fails for any of the specified symbols
     */
    bool load(const QStringList &libraryNameList);
    /**
     * @brief Remove a specific symbol by basename
     * @details Removes the symbol file identified by `libraryName` from
     *          the symbol directory. This method is useful for deleting
     *          individual symbol files. It requires a valid projectManager
     *          and checks if the symbol file exists.
     * @param libraryName Basename of the symbol file to remove, without
     *        the ".soc_sym" extension.
     * @retval true The symbol file is successfully removed
     * @retval false Removal fails or the file does not exist
     */
    bool remove(const QString &libraryName);
    /**
     * @brief Remove symbols matching a regex pattern
     * @details Removes all symbol files from the symbol directory that
     *          match `libraryNameRegex`. This method is ideal for batch
     *          removal of symbols based on naming patterns. It relies on
     *          a valid projectManager to execute.
     * @param libraryNameRegex Regex to match file basenames. Defaults
     *        to ".*", which matches all symbol files.
     * @retval true if all matching symbols are successfully removed
     * @retval false if removal of any matching symbols fails
     */
    bool remove(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Remove multiple symbols by a list of basenames
     * @details Removes multiple symbol files specified in `libraryNameList`
     *          from the symbol directory. Useful for deleting a specific set of
     *          symbol files. Requires a valid projectManager and checks each
     *          symbol file's existence.
     * @param libraryNameList List of symbol file basenames to remove, without
     *        ".soc_sym" extensions.
     * @retval true All specified symbols are successfully removed
     * @retval false Removal fails for any of the specified symbols
     */
    bool remove(const QStringList &libraryNameList);
    /**
     * @brief Save symbol data associated with a specific basename
     * @details Serializes and saves the symbol data related to the given
     *          `libraryName`. It locates the corresponding modules in
     *          `symbolLib` using `symbolMap`, then serializes them into YAML
     *          format. The result is saved to a file with the same basename,
     *          appending the ".soc_sym" extension. Existing files are
     *          overwritten. This function requires a valid projectManager.
     * @param libraryName The basename of the symbol, excluding extension
     * @retval true on successful serialization and saving
     * @retval false on failure to serialize or save
     */
    bool save(const QString &libraryName);
    /**
     * @brief Save multiple symbols matching a regex pattern
     * @details Iterates through `symbolMap` to find symbols matching the
     *          provided regex pattern. Each matching symbol is serialized and
     *          saved individually in YAML format. Files are named after the
     *          symbol basenames with the ".soc_sym" extension. Existing files
     *          are overwritten. This function requires a valid projectManager.
     * @param libraryNameRegex Regular expression to filter symbol basenames
     * @retval true if all matching symbols are successfully saved
     * @retval false if saving any matching symbol fails
     */
    bool save(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Save multiple symbols by a list of basenames
     * @details Serializes and saves symbol data related to each `libraryName`
     *          in `libraryNameList`. It locates corresponding modules in
     *          `symbolLib` using `symbolMap`, then serializes them into YAML
     *          format. Results are saved to files named after each basename,
     *          appending ".soc_sym". Existing files are overwritten. Requires
     *          a valid projectManager.
     * @param libraryNameList List of symbol basenames to save, excluding
     *        extensions.
     * @retval true All specified symbols are successfully saved
     * @retval false Saving fails for any of the specified symbols
     */
    bool save(const QStringList &libraryNameList);
    /**
     * @brief Get list of module names matching a regex pattern
     * @details Retrieves module names from the `symbolLib` YAML node that
     *          match the provided `moduleNameRegex`. This function scans
     *          the symbol library and compiles a list of module names. Useful
     *          for processing or iterating over project modules. It relies on
     *          the validity of the `symbolLib` YAML node. This function
     *          requires a valid projectManager.
     * @param moduleNameRegex Regular expression to match module names,
     *        default is ".*".
     * @return QStringList of module names matching the regex in the symbol
     *         library
     */
    QStringList listModule(const QRegularExpression &moduleNameRegex = QRegularExpression(".*"));

    /**
     * @brief Remove modules matching regex from symbol library
     * @details Removes modules that match `moduleNameRegex` from symbolLib,
     *          updating symbolMap accordingly. It saves symbols with remaining
     *          module associations and removes files with no associations.
     *          Requires a valid projectManager for execution.
     * @param moduleNameRegex Regex to filter module names for removal.
     * @retval true All matching modules are successfully processed
     * @retval false Errors occur during module removal or symbol saving
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
