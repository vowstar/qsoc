#ifndef QSOCCONFIG_H
#define QSOCCONFIG_H

#include "common/qsocprojectmanager.h"

#include <QMap>
#include <QObject>
#include <QString>

/**
 * @brief The QSocConfig class.
 * @details This class is used to manage configuration settings from YAML files 
 *          and environment variables. It supports hierarchical configuration loading
 *          from multiple sources with different priorities.
 */
class QSocConfig : public QObject
{
    Q_OBJECT
public:
    /* System-level configuration file path (Linux only) */
    static const QString CONFIG_FILE_SYSTEM;

    /* User-level configuration file path */
    static const QString CONFIG_FILE_USER;

    /* Project-level configuration file name */
    static const QString CONFIG_FILE_PROJECT;

    /**
     * @brief Constructor for QSocConfig.
     * @details Initializes the configuration manager with optional project manager.
     * @param[in] parent Parent object.
     * @param[in] projectManager Pointer to project manager, can be nullptr.
     */
    explicit QSocConfig(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    /**
     * @brief Set the project manager.
     * @details Sets the project manager and reloads configuration if a valid 
     *          project manager is provided.
     * @param[in] projectManager Pointer to project manager, can be nullptr.
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Get the project manager.
     * @details Retrieves the currently assigned project manager.
     * @return QSocProjectManager* Pointer to the current project manager.
     */
    QSocProjectManager *getProjectManager();

    /**
     * @brief Load configuration from all sources.
     * @details This function loads configuration from environment variables and
     *          YAML files according to priority order:
     *          1. Environment variables (highest priority)
     *          2. Project-level config: getProjectPath()/.qsoc.yml
     *          3. User-level config: ~/.config/qsoc/qsoc.yml
     *          4. System-level config: /etc/qsoc/qsoc.yml (Linux only)
     */
    void loadConfig();

    /**
     * @brief Get configuration value.
     * @details Returns the value for the specified configuration key.
     * @param key Configuration key.
     * @param defaultValue Default value if key is not found.
     * @return Value of the configuration key or defaultValue if not found.
     */
    QString getValue(const QString &key, const QString &defaultValue = QString()) const;

    /**
     * @brief Set configuration value.
     * @details Sets the value for the specified configuration key.
     * @param key Configuration key.
     * @param value Configuration value.
     */
    void setValue(const QString &key, const QString &value);

    /**
     * @brief Check if configuration key exists.
     * @details Checks if the specified configuration key exists.
     * @param key Configuration key to check.
     * @return True if key exists, false otherwise.
     */
    bool hasKey(const QString &key) const;

    /**
     * @brief Get all configuration values.
     * @details Returns all configuration values as a map.
     * @return Map of all configuration values.
     */
    QMap<QString, QString> getAllValues() const;

private:
    /* Project manager pointer, can be nullptr */
    QSocProjectManager *projectManager;

    /* Configuration values map */
    QMap<QString, QString> configValues;

    /**
     * @brief Load configuration from environment variables.
     * @details Loads configuration from environment variables with highest priority.
     */
    void loadFromEnvironment();

    /**
     * @brief Load configuration from YAML file.
     * @details Loads configuration from specified YAML file.
     * @param filePath Path to YAML file.
     * @param override Whether to override existing values.
     */
    void loadFromYamlFile(const QString &filePath, bool override = false);

    /**
     * @brief Load configuration from project YAML file.
     * @details Loads configuration from project-level YAML file if project 
     *          manager is available.
     * @param override Whether to override existing values.
     */
    void loadFromProjectYaml(bool override = false);

    /**
     * @brief Create template configuration file with comments.
     * @details Creates a template configuration file with commented example settings
     *          if the file doesn't exist.
     * @param filePath Path to the configuration file.
     * @return True if successful, false otherwise.
     */
    bool createTemplateConfig(const QString &filePath);
};

#endif // QSOCCONFIG_H