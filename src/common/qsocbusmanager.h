#ifndef QSOCBUSMANAGER_H
#define QSOCBUSMANAGER_H

#include "common/qsocprojectmanager.h"

#include <QObject>
#include <QRegularExpression>

#include <yaml-cpp/yaml.h>

/**
 * @brief The QSocBusManager class.
 * @details This class is used to manage the bus library files.
 */
class QSocBusManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor.
     * @details This constructor will create an instance of this object.
     * @param[in] parent parent object.
     * @param[in] projectManager project manager.
     */
    explicit QSocBusManager(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

public slots:
    /**
     * @brief Set the project manager.
     * @details Assigns a new project manager to this object. The project
     *          manager is used for managing various project-related
     *          functionalities.
     * @param projectManager Pointer to the new project manager.
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Get the project manager.
     * @details Retrieves the currently assigned project manager. This manager
     *          is responsible for handling various aspects of the project.
     * @return QSocProjectManager * Pointer to the current project manager.
     */
    QSocProjectManager *getProjectManager();

    /**
     * @brief Checks the validity of the bus path.
     * @details Verifies if the bus path set in the project manager is valid.
     * @retval true Bus path is valid.
     * @retval false Bus path is invalid or projectManager is nullptr.
     */
    bool isBusPathValid();

    /**
     * @brief Save the library YAML object to library file.
     * @details This function will save the library YAML object to library file.
     * @param libraryName The basename of the library file without ext.
     * @param libraryYaml The library YAML object.
     * @retval true Save successfully.
     * @retval false Save failed.
     */
    bool saveLibraryYaml(const QString &libraryName, const YAML::Node &libraryYaml);

    /**
     * @brief Get list of library basenames.
     * @details Retrieves basenames of ".soc_bus" files in directory defined by
     *          `busPath`, excluding the ".soc_bus" extension. Scans the
     *          bus directory and compiles a list of relevant basenames.
     *          Useful for processing or iterating over library files. This
     *          function relies on projectManager to be valid.
     * @param libraryNameRegex Regular expression to match file basenames,
     *        default is ".*".
     * @return QStringList of basenames for all ".soc_bus" files in the bus
     *         directory, excluding the ".soc_bus" extension.
     */
    QStringList listLibrary(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));

    /**
     * @brief Check if the library file exists in the filesystem.
     * @details This function checks if a library file with the given basename
     *          exists in the bus directory. It is used to verify the
     *          existence of library files before processing them.
     * @param libraryName The basename of the library file, without the
     *        ".soc_bus" extension.
     * @retval true Library file exists in the bus directory.
     * @retval false Library file does not exist in the bus directory.
     */
    bool isExist(const QString &libraryName);

    /**
     * @brief Load a specific library by basename.
     * @details Loads library specified by `libraryName` from the bus directory.
     *          Useful for retrieving individual library files. Relies on a
     *          valid projectManager and existence of the library file.
     * @param libraryName Basename of bus file to load, without
     *        ".soc_bus" extension.
     * @retval true Library is successfully loaded.
     * @retval false Loading fails or file does not exist.
     */
    bool load(const QString &libraryName);

    /**
     * @brief Load libraries matching a regex pattern.
     * @details Loads library files matching `libraryNameRegex` in the bus
     *          directory. Ideal for batch processing or retrieving libraries
     *          based on naming patterns. Requires projectManager to be valid.
     * @param libraryNameRegex Regex to match file basenames, defaults to
     *        ".*", matching all libraries.
     * @retval true All matching libraries are loaded.
     * @retval false Loading any matching libraries fails.
     */
    bool load(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));

    /**
     * @brief Load multiple libraries by a list of basenames.
     * @details Loads multiple libraries specified in `libraryNameList` from the
     *          bus directory. Useful for loading a specific set of
     *          library files. Requires a valid projectManager and checks the
     *          existence of each library file.
     * @param libraryNameList List of library file basenames to load, without
     *        ".soc_bus" extensions.
     * @retval true All specified libraries are successfully loaded.
     * @retval false Loading fails for any of the specified libraries.
     */
    bool load(const QStringList &libraryNameList);

    /**
     * @brief Remove a specific library by basename.
     * @details Removes the library file identified by `libraryName` from
     *          the library directory. This method is useful for deleting
     *          individual library files. It requires a valid projectManager
     *          and checks if the library file exists.
     * @param libraryName Basename of the library file to remove, without
     *        the ".soc_bus" extension.
     * @retval true The library file is successfully removed.
     * @retval false Removal fails or the file does not exist.
     */
    bool remove(const QString &libraryName);

    /**
     * @brief Remove libraries matching a regex pattern.
     * @details Removes library bus files from the bus directory that
     *          match `libraryNameRegex`. This method is ideal for batch
     *          removal of libraries based on naming patterns. It relies on
     *          a valid projectManager to execute.
     * @param libraryNameRegex Regex to match file basenames. Defaults
     *        to ".*", which matches all bus files.
     * @retval true if all matching libraries are successfully removed.
     * @retval false if removal of any matching libraries fails.
     */
    bool remove(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));

    /**
     * @brief Remove multiple libraries by a list of basenames.
     * @details Removes multiple bus files specified in `libraryNameList`
     *          from the bus directory. Useful for deleting a specific set of
     *          bus files. Requires a valid projectManager and checks each
     *          bus file's existence.
     * @param libraryNameList List of bus file basenames to remove, without
     *        ".soc_bus" extensions.
     * @retval true All specified libraries are successfully removed.
     * @retval false Removal fails for any of the specified libraries.
     */
    bool remove(const QStringList &libraryNameList);

    /**
     * @brief Save library data associated with a specific basename.
     * @details Serializes and saves the bus data related to the given
     *          `libraryName`. It locates the corresponding buses in
     *          `busData` using `libraryMap`, then serializes them into YAML
     *          format. The result is saved to a file with the same basename,
     *          appending the ".soc_bus" extension. Existing files are
     *          overwritten. This function requires a valid projectManager.
     * @param libraryName The basename of the bus, excluding extension.
     * @retval true on successful serialization and saving.
     * @retval false on failure to serialize or save.
     */
    bool save(const QString &libraryName);

    /**
     * @brief Save multiple libraries matching a regex pattern.
     * @details Iterates through `libraryMap` to find libraries matching the
     *          provided regex pattern. Each matching library is serialized and
     *          saved individually in YAML format. Files are named after the
     *          library basenames with the ".soc_bus" extension. Existing files
     *          are overwritten. This function requires a valid projectManager.
     * @param libraryNameRegex Regular expression to filter library basenames.
     * @retval true if all matching libraries are successfully saved.
     * @retval false if saving any matching bus fails.
     */

    bool save(const QRegularExpression &libraryNameRegex = QRegularExpression(".*"));
    /**
     * @brief Save multiple libraries by a list of basenames.
     * @details Serializes and saves bus data related to each `libraryName`
     *          in `libraryNameList`. It locates corresponding buses in
     *          `busData` using `libraryMap`, then serializes them into YAML
     *          format. Results are saved to files named after each basename,
     *          appending ".soc_bus". Existing files are overwritten. Requires
     *          a valid projectManager.
     * @param libraryNameList List of library basenames to save, excluding
     *        extensions.
     * @retval true All specified libraries are successfully saved.
     * @retval false Saving fails for any of the specified libraries.
     */
    bool save(const QStringList &libraryNameList);

    /**
     * @brief Get list of bus names matching a regex pattern.
     * @details Retrieves bus names from the `busData` YAML node that
     *          match the provided `busNameRegex`. This function scans
     *          the bus library and compiles a list of bus names. Useful
     *          for processing or iterating over project libraries. It relies on
     *          the validity of the `busData` YAML node. This function
     *          requires a valid projectManager.
     * @param busNameRegex Regular expression to match bus names,
     *        default is ".*".
     * @return QStringList of bus names matching the regex in the bus
     *         library.
     */
    QStringList listBus(const QRegularExpression &busNameRegex = QRegularExpression(".*"));

    /**
     * @brief Remove buses matching regex from bus library.
     * @details Removes buses that match `busNameRegex` from busData,
     *          updating libraryMap accordingly. It saves libraries with
     *          remaining bus associations and removes files with no
     *          associations. Requires a valid projectManager for execution.
     * @param busNameRegex Regex to filter bus names for removal.
     * @retval true All matching buses are successfully processed.
     * @retval false Errors occur during bus removal or bus saving.
     */
    bool removeBus(const QRegularExpression &busNameRegex = QRegularExpression(".*"));

    /**
     * @brief Retrieve YAML node for buses matching the regex.
     * @details Fetches and returns the YAML node for buses whose names
     *          match the provided regular expression. This allows for
     *          specific querying and manipulation of bus data within
     *          the bus library. Defaults to fetching all bus nodes
     *          if no regex is specified.
     * @param busNameRegex Regex used to filter the bus names.
     *                        Default is ".*", which matches all buses.
     * @return YAML::Node The YAML node(s) corresponding to the matched
     *                    bus(es). Returns an empty node if no match is found.
     */
    YAML::Node getBusNode(const QRegularExpression &busNameRegex = QRegularExpression(".*"));

private:
    /* Internal used project manager. */
    QSocProjectManager *projectManager = nullptr;

    /* This QMap, libraryMap, maps library names to sets of bus names.
       Each key in the map is a library name (QString).
       The corresponding value is a QSet<QString> containing the names
       of all buses that are part of that library.
       - key: QString libraryName
       - value: QSet<QString> busNameSet
       This structure ensures that each library name is associated with
       a unique set of bus names, allowing efficient retrieval and
       management of buses within each library. */
    QMap<QString, QSet<QString>> libraryMap;

    /* Bus library YAML node */
    YAML::Node busData;

    /**
     * @brief Merge two YAML nodes.
     * @details This function will merge two YAML nodes. It returns a new map
     *          resultYaml which is a merge of fromYaml into toYaml.
     *          Values from fromYaml will replace identically keyed non-map
     *          values from toYaml in the resultYaml map.
     *          The values ​​in toYaml and fromYaml will not be modified.
     * @param toYaml The destination YAML node.
     * @param fromYaml The source YAML node.
     * @return YAML::Node The merged YAML node which is a merge of fromYaml into
     *                    toYaml.
     */
    YAML::Node mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml);

    /**
     * @brief Adds a bus name to the library map.
     * @details This function adds a given bus name to the set of buses
     *          associated with a specified library in the libraryMap.
     *          If the library does not already exist in the map, it creates
     *          a new entry with an empty set of buses and then adds the
     *          bus name. If the library exists, it simply adds the bus
     *          name to the existing set.
     * @param libraryName The name of the library.
     * @param busName The name of the bus to add to the library.
     */
    void libraryMapAdd(const QString &libraryName, const QString &busName);

    /**
     * @brief Removes a bus from a library in the library map.
     * @details This function removes the specified bus from the set
     *          associated with the given library name in the library map. If
     *          the bus is the only one in the set, it also removes the
     *          entire library entry from the map. Does nothing if the library
     *          or bus does not exist.
     * @param libraryName The name of the library to be modified.
     * @param busName The name of the bus to be removed.
     */
    void libraryMapRemove(const QString &libraryName, const QString &busName);

signals:
};

#endif // QSOCBUSMANAGER_H
