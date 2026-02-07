// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPROJECTMANAGER_H
#define QSOCPROJECTMANAGER_H

#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>

#include <yaml-cpp/yaml.h>

/**
 * @brief The QSocProjectManager class.
 * @details This class is used to manage project environment variables.
 *          It can save and load project environment variables from
 *          project file.
 */
class QSocProjectManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSocProjectManager.
     * @details This constructor will initialize project environment.
     * @param[in] parent parent object.
     */
    explicit QSocProjectManager(QObject *parent = nullptr);

    /**
     * @brief Destructor for QSocProjectManager.
     * @details This destructor will free all the allocated resources.
     */
    ~QSocProjectManager() override;

public slots:
    /**
     * @brief Set project environment variable.
     * @details This function will set project environment variable.
     * @param key environment variable key.
     * @param value environment variable value.
     */
    void setEnv(const QString &key, const QString &value);

    /**
     * @brief Set project environment variables.
     * @details This function will set project environment variables.
     * @param env environment variables.
     */
    void setEnv(const QMap<QString, QString> &env);

    /**
     * @brief Get project environment variables.
     * @details This function will return project environment variables.
     * @return environment variable values map.
     */
    const QMap<QString, QString> &getEnv();

    /**
     * @brief Is the project exist on filesystem.
     * @details This function will check if the project exist on filesystem.
     * @param projectName The name of the project.
     * @retval true Project exist in the project directory.
     * @retval false Project does not exist in the project directory.
     */
    bool isExist(const QString &projectName);

    /**
     * @brief Create project directories.
     * @details Creates all necessary directories for the project, including
     *          bus, module, schematic, and output directories.
     * @retval true if all directories were successfully created.
     * @retval false if any directory creation failed.
     */
    bool mkpath();

    /**
     * @brief Save project environment variables and settings.
     * @details This function will save project environment variables
     *          and settings into project file.
     * @param projectName The name of the project.
     * @retval true Save successfully.
     * @retval false Save failed.
     */
    bool save(const QString &projectName);

    /**
     * @brief Load project environment variables and settings.
     * @details This function will load project environment variables
     *          and settings from project file.
     * @param projectName The name of the project.
     * @retval true Load successfully.
     * @retval false Load failed.
     */
    bool load(const QString &projectName);

    /**
     * @brief Auto load the first found project.
     * @details Automatically loads the first project file found in the specified
     *          directory. It initializes project environment variables and
     *          settings based on this file.
     * @retval true Load successfully.
     * @retval false Load failed.
     */
    bool loadFirst(bool silent = false);

    /**
     * @brief Remove project file.
     * @details This function will remove project file which in the project
     *          directory.
     * @param projectName The name of the project.
     * @retval true Remove successfully.
     * @retval false Remove failed.
     */
    bool remove(const QString &projectName);

    /**
     * @brief List matched projects.
     * @details This function will list matched projects which in the project
     *          directory.
     * @param projectNameRegex regular expression to match the project name,
     *        default is ".*".
     * @return QStringList The list of project in the project directory.
     */
    QStringList list(const QRegularExpression &projectNameRegex = QRegularExpression(".*"));

    /**
     * @brief Validate project environment settings.
     * @details Checks the validity of project environment settings.
     * @param writable Controls if all path writability should be checked.
     * @retval true Project environment settings are valid.
     * @retval false Project environment settings are invalid.
     */
    bool isValid(bool writable = false);

    /**
     * @brief Validate the project node.
     * @details Checks if the provided project YAML node is valid.
     * @retval true The project node is valid.
     * @retval false The project node is invalid.
     */
    bool isValidProjectNode();

    /**
     * @brief Validate the project name.
     * @details Checks if the provided project name is valid.
     * @retval true The project name is valid.
     * @retval false The project name is invalid.
     */
    bool isValidProjectName();

    /**
     * @brief Validate the path.
     * @details Checks if the provided path is valid.
     * @param path The path to validate.
     * @param writable Controls if path writability should be checked.
     * @retval true The path is valid.
     * @retval false The path is invalid.
     */
    bool isValidPath(const QString &path, bool writable = false);

    /**
     * @brief Validate the project path.
     * @details Checks if the provided project path is valid.
     * @param writable Controls if path writability should be checked.
     * @retval true The project path is valid.
     * @retval false The project path is invalid.
     */
    bool isValidProjectPath(bool writable = false);

    /**
     * @brief Validate the bus path.
     * @details Checks if the provided bus path is valid.
     * @param writable Controls if path writability should be checked.
     * @retval true The bus path is valid.
     * @retval false The bus path is invalid.
     */
    bool isValidBusPath(bool writable = false);

    /**
     * @brief Validate the module path.
     * @details Checks if the provided module path is valid.
     * @param writable Controls if path writability should be checked.
     * @retval true The module path is valid.
     * @retval false The module path is invalid.
     */
    bool isValidModulePath(bool writable = false);

    /**
     * @brief Validate the schematic path.
     * @details Checks if the provided schematic path is valid.
     * @param writable Controls if path writability should be checked.
     * @retval true The schematic path is valid.
     * @retval false The schematic path is invalid.
     */
    bool isValidSchematicPath(bool writable = false);

    /**
     * @brief Validate the output path.
     * @details Checks if the provided output path is valid.
     * @param writable Controls if path writability should be checked.
     * @retval true The output path is valid.
     * @retval false The output path is invalid.
     */
    bool isValidOutputPath(bool writable = false);

    /**
     * @brief Get project YAML configuration.
     * @details This function will return the project configuration in YAML.
     * @return YAML::Node & The project YAML configuration.
     */
    const YAML::Node &getProjectYaml();

    /**
     * @brief Get project name.
     * @details This function will return project name.
     * @return QString & The project name.
     */
    const QString &getProjectName();

    /**
     * @brief Get project path.
     * @details This function will return project path.
     * @return QString & The project path.
     */
    const QString &getProjectPath();

    /**
     * @brief Get project bus path.
     * @details This function will return project bus path.
     * @return QString & The project bus path.
     */
    const QString &getBusPath();

    /**
     * @brief Get project module path.
     * @details This function will return project module path.
     * @return QString & The project module path.
     */
    const QString &getModulePath();

    /**
     * @brief Get project schematic path.
     * @details This function will return project schematic path.
     * @return QString & The project schematic path.
     */
    const QString &getSchematicPath();

    /**
     * @brief Get project output path.
     * @details This function will return project output path.
     * @return QString & The project output path.
     */
    const QString &getOutputPath();

    /**
     * @brief Get the current path
     * @details This function will return the current working path.
     * @return QString & The current path.
     */
    const QString &getCurrentPath();

    /**
     * @brief Set project node.
     * @details This function will set project YAML node.
     * @param projectNode The project node.
     */
    void setProjectNode(const YAML::Node &projectNode);

    /**
     * @brief Set project name.
     * @details This function will set project name.
     * @param projectName The project name.
     */
    void setProjectName(const QString &projectName);

    /**
     * @brief Set project path.
     * @details This function will set project path.
     * @param projectPath The project path.
     */
    void setProjectPath(const QString &projectPath);

    /**
     * @brief Set project bus path.
     * @details This function will set project bus path.
     * @param busPath The bus path.
     */
    void setBusPath(const QString &busPath);

    /**
     * @brief Set project module path.
     * @details This function will set project module path.
     * @param modulePath The module path.
     */
    void setModulePath(const QString &modulePath);

    /**
     * @brief Set project schematic path.
     * @details This function will set project schematic path.
     * @param schematicPath The schematic path.
     */
    void setSchematicPath(const QString &schematicPath);

    /**
     * @brief Set project output path.
     * @details This function will set project output path.
     * @param outputPath The output path.
     */
    void setOutputPath(const QString &outputPath);

    /**
     * @brief Set the current path and update all project-related paths
     * @details This function sets the current path and updates all other paths based on it:
     *          - Project path is set to the current path
     *          - Bus path is set to current path + "/bus"
     *          - Module path is set to current path + "/module"
     *          - Schematic path is set to current path + "/schematic"
     *          - Output path is set to current path + "/output"
     * @param currentPath The new current path to set
     */
    void setCurrentPath(const QString &currentPath);

private:
    /* Project environment variables map. */
    QMap<QString, QString> env;

    /* Project YAML node. */
    YAML::Node projectNode;

    /* Project name. */
    QString projectName;

    /* Project path. */
    QString projectPath;

    /* Project bus path. */
    QString busPath;

    /* Project module path. */
    QString modulePath;

    /* Project schematic path. */
    QString schematicPath;

    /* Project output path. */
    QString outputPath;

    /* Current path. */
    QString currentPath;

    /**
     * @brief Get simplified path.
     * @details This function will get simplified path.
     * @param path The path.
     * @return QString The simplified path.
     */
    QString getSimplifyPath(const QString &path);

    /**
     * @brief Get expanded path.
     * @details This function will get expanded path.
     * @param path The path.
     * @return QString The expanded path.
     */
    QString getExpandPath(const QString &path);

signals:
};

#endif // QSOCPROJECTMANAGER_H
