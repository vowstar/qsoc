#ifndef QSOCMODULEMANAGER_H
#define QSOCMODULEMANAGER_H

#include "common/qsocprojectmanager.h"

#include <QObject>
#include <QRegularExpression>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

/**
 * @brief   The QSocModuleManager class
 * @details This class is used to manage the module library files.
 */
class QSocModuleManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor
     * @details This constructor will create an instance of this object.
     * @param[in] parent parent object
     * @param[in] projectManager project manager
     */
    explicit QSocModuleManager(
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
     * @brief Check if module path is valid
     * @details Verifies the validity of the module path set in the project
     *          manager. Checks if the path exists and is a directory.
     * @retval true Module path is valid
     * @retval false Module path is invalid, or projectManager is nullptr
     */
    bool isModulePathValid();
    /**
     * @brief Check if a regular expression is valid and non-empty
     * @details Validates the provided regular expression object. It checks
     *          whether the regex is a valid pattern and not an empty string.
     *          This function is useful for ensuring that regex patterns used
     *          for filtering or matching are correctly formatted and not blank.
     * @param regex The QRegularExpression object to validate
     * @retval true If the regex is valid and non-empty
     * @retval false If the regex is invalid or an empty string
     */
    bool isNameRegexValid(const QRegularExpression &regex);
    /**
     * @brief Determines if a string contains a regular expression
     * @details Checks if the provided string contains characters or patterns
     *          commonly used in regular expressions. This function is helpful
     *          for distinguishing between plain text and regex patterns,
     *          especially when both could be user inputs. It uses a heuristic
     *          approach to identify regex-specific characters or sequences.
     * @param str The string to be checked for regular expression content
     * @retval true If the string appears to contain a regular expression
     * @retval false If the string does not contain common regex characters or
     *         patterns
     */
    bool isNameRegularExpression(const QString &str);
    /**
     * @brief Checks if a string exactly matches a regular expression
     * @details This function determines if the given string strictly matches
     *          the provided regular expression. If the regex pattern is a plain
     *          string (not a typical regular expression), it converts it to a regex
     *          that matches the exact string. Useful for validating inputs against
     *          specific patterns or keywords.
     * @param str The string to compare against the regex
     * @param regex The QRegularExpression to match against the string
     * @retval true If the string exactly matches the regex
     * @retval false If the string does not match, or the pattern is empty
     */
    bool isNameExactMatch(const QString &str, const QRegularExpression &regex);
    /**
     * @brief Import verilog files from file list
     * @details This function will import verilog files from file list, and
     *          generate the module library file.
     *          If libraryName is empty, the first matching verilog module
     *          name is automatically selected and converted into lowercase as
     *          the module library name.
     *          If moduleNameRegex is empty, the first matching verilog module
     *          is automatically selected for import.
     * @param libraryName The basename of the module library file without ext
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
     * @brief Save the library YAML object to library file
     * @details This function will save the library YAML object to library file.
     * @param libraryName The basename of the library file without ext
     * @param libraryYaml The library YAML object
     * @retval true Save successfully
     * @retval false Save failed
     */
    bool saveLibraryYaml(const QString &libraryName, const YAML::Node &libraryYaml);
    /**
     * @brief Get list of library basenames
     * @details Retrieves basenames of ".soc_sym" files in directory defined by
     *          `modulePath`, excluding the ".soc_sym" extension. Scans the
     *          module directory and compiles a list of relevant basenames.
     *          Useful for processing or iterating over library files. This
     *          function relies on projectManager to be valid.
     * @param libraryNameRegex Regular expression to match file basenames,
     *        default is ".*".
     * @return QStringList of basenames for all ".soc_sym" files in the module
     *         directory, excluding the ".soc_sym" extension
     */
    QStringList listLibrary(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Check if the library file exists in the filesystem
     * @details This function checks if a library file with the given basename
     *          exists in the module directory. It is used to verify the
     *          existence of library files before processing them.
     * @param libraryName The basename of the library file, without the
     *        ".soc_sym" extension.
     * @retval true Library file exists in the module directory
     * @retval false Library file does not exist in the module directory
     */
    bool isExist(const QString &libraryName);
    /**
     * @brief Load a specific library by basename
     * @details Loads library specified by `libraryName` from the module
     *          directory. Useful for retrieving individual library files.
     *          Relies on a valid projectManager and existence of the library
     *          file.
     * @param libraryName Basename of module file to load, without
     *        ".soc_sym" extension
     * @retval true Library is successfully loaded
     * @retval false Loading fails or file does not exist
     */
    bool load(const QString &libraryName);
    /**
     * @brief Load libraries matching a regex pattern
     * @details Loads library files matching `libraryNameRegex` in the module
     *          directory. Ideal for batch processing or retrieving libraries
     *          based on naming patterns. Requires projectManager to be valid.
     * @param libraryNameRegex Regex to match file basenames, defaults to
     *        ".*", matching all libraries
     * @retval true All matching libraries are loaded
     * @retval false Loading any matching libraries fails
     */
    bool load(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Load multiple libraries by a list of basenames
     * @details Loads multiple libraries specified in `libraryNameList` from the
     *          module directory. Useful for loading a specific set of
     *          library files. Requires a valid projectManager and checks the
     *          existence of each library file.
     * @param libraryNameList List of library file basenames to load, without
     *        ".soc_sym" extensions.
     * @retval true All specified libraries are successfully loaded
     * @retval false Loading fails for any of the specified libraries
     */
    bool load(const QStringList &libraryNameList);
    /**
     * @brief Remove a specific library by basename
     * @details Removes the library file identified by `libraryName` from
     *          the library directory. This method is useful for deleting
     *          individual library files. It requires a valid projectManager
     *          and checks if the library file exists.
     * @param libraryName Basename of the library file to remove, without
     *        the ".soc_sym" extension.
     * @retval true The library file is successfully removed
     * @retval false Removal fails or the file does not exist
     */
    bool remove(const QString &libraryName);
    /**
     * @brief Remove libraries matching a regex pattern
     * @details Removes all module files from the module directory that
     *          match `libraryNameRegex`. This method is ideal for batch
     *          removal of libraries based on naming patterns. It relies on
     *          a valid projectManager to execute.
     * @param libraryNameRegex Regex to match file basenames. Defaults
     *        to ".*", which matches all module files.
     * @retval true if all matching libraries are successfully removed
     * @retval false if removal of any matching libraries fails
     */
    bool remove(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Remove multiple libraries by a list of basenames
     * @details Removes multiple module files specified in `libraryNameList`
     *          from the module directory. Useful for deleting a specific set of
     *          module files. Requires a valid projectManager and checks each
     *          module file's existence.
     * @param libraryNameList List of module file basenames to remove, without
     *        ".soc_sym" extensions.
     * @retval true All specified libraries are successfully removed
     * @retval false Removal fails for any of the specified libraries
     */
    bool remove(const QStringList &libraryNameList);
    /**
     * @brief Save library data associated with a specific basename
     * @details Serializes and saves the module data related to the given
     *          `libraryName`. It locates the corresponding modules in
     *          `moduleData` using `libraryMap`, then serializes them into YAML
     *          format. The result is saved to a file with the same basename,
     *          appending the ".soc_sym" extension. Existing files are
     *          overwritten. This function requires a valid projectManager.
     * @param libraryName The basename of the module, excluding extension
     * @retval true on successful serialization and saving
     * @retval false on failure to serialize or save
     */
    bool save(const QString &libraryName);
    /**
     * @brief Save multiple libraries matching a regex pattern
     * @details Iterates through `libraryMap` to find libraries matching the
     *          provided regex pattern. Each matching library is serialized and
     *          saved individually in YAML format. Files are named after the
     *          library basenames with the ".soc_sym" extension. Existing files
     *          are overwritten. This function requires a valid projectManager.
     * @param libraryNameRegex Regular expression to filter library basenames
     * @retval true if all matching libraries are successfully saved
     * @retval false if saving any matching module fails
     */
    bool save(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Save multiple libraries by a list of basenames
     * @details Serializes and saves module data related to each `libraryName`
     *          in `libraryNameList`. It locates corresponding modules in
     *          `moduleData` using `libraryMap`, then serializes them into YAML
     *          format. Results are saved to files named after each basename,
     *          appending ".soc_sym". Existing files are overwritten. Requires
     *          a valid projectManager.
     * @param libraryNameList List of library basenames to save, excluding
     *        extensions.
     * @retval true All specified libraries are successfully saved
     * @retval false Saving fails for any of the specified libraries
     */
    bool save(const QStringList &libraryNameList);
    /**
     * @brief Get list of module names matching a regex pattern
     * @details Retrieves module names from the `moduleData` YAML node that
     *          match the provided `moduleNameRegex`. This function scans
     *          the module library and compiles a list of module names. Useful
     *          for processing or iterating over project libraries. It relies on
     *          the validity of the `moduleData` YAML node. This function
     *          requires a valid projectManager.
     * @param moduleNameRegex Regular expression to match module names,
     *        default is ".*".
     * @return QStringList of module names matching the regex in the module
     *         library
     */
    QStringList listModule(const QRegularExpression &moduleNameRegex = QRegularExpression(".*"));

    /**
     * @brief Remove modules matching regex from module library
     * @details Removes modules that match `moduleNameRegex` from moduleData,
     *          updating libraryMap accordingly. It saves libraries with
     *          remaining module associations and removes files with no
     *          associations. Requires a valid projectManager for execution.
     * @param moduleNameRegex Regex to filter module names for removal.
     * @retval true All matching modules are successfully processed
     * @retval false Errors occur during module removal or module saving
     */
    bool removeModule(const QRegularExpression &moduleNameRegex = QRegularExpression(".*"));

private:
    /* Internal used project manager */
    QSocProjectManager *projectManager = nullptr;
    /* This QMap, libraryMap, maps library names to sets of module names.
       Each key in the map is a library name (QString).
       The corresponding value is a QSet<QString> containing the names
       of all modules that are part of that library.
       - key: QString libraryName
       - value: QSet<QString> moduleNameSet
       This structure ensures that each library name is associated with
       a unique set of module names, allowing efficient retrieval and
       management of modules within each library. */
    QMap<QString, QSet<QString>> libraryMap;
    /* Module library YAML node */
    YAML::Node moduleData;
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
    /**
     * @brief Adds a module name to the library map.
     * @details This function adds a given module name to the set of modules
     *          associated with a specified library in the libraryMap.
     *          If the library does not already exist in the map, it creates
     *          a new entry with an empty set of modules and then adds the
     *          module name. If the library exists, it simply adds the module
     *          name to the existing set.
     * @param libraryName The name of the library.
     * @param moduleName The name of the module to add to the library.
     */
    void libraryMapAdd(const QString &libraryName, const QString &moduleName);
    /**
     * @brief Removes a module from a library in the library map.
     * @details This function removes the specified module from the set
     *          associated with the given library name in the library map. If
     *          the module is the only one in the set, it also removes the
     *          entire library entry from the map. Does nothing if the library
     *          or module does not exist.
     * @param libraryName The name of the library to be modified.
     * @param moduleName The name of the module to be removed.
     */
    void libraryMapRemove(const QString &libraryName, const QString &moduleName);

signals:
};

#endif // QSOCMODULEMANAGER_H
