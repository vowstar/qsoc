#ifndef QSOCPROJECTMANAGER_H
#define QSOCPROJECTMANAGER_H

#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>

#include <yaml-cpp/yaml.h>

/**
 * @brief   The QSocProjectManager class
 * @details This class is used to manage project environment variables.
 *          It can save and load project environment variables from
 *          project file.
 */
class QSocProjectManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSocProjectManager
     * @details This constructor will initialize project environment.
     * @param[in] parent parent object
     */
    explicit QSocProjectManager(QObject *parent = nullptr);

public slots:
    /**
     * @brief Set project environment variable
     * @details This function will set project environment variable.
     * @param key environment variable key
     * @param value environment variable value
     */
    void setEnv(const QString &key, const QString &value);
    /**
     * @brief Set project environment variables
     * @details This function will set project environment variables.
     * @param env environment variables
     */
    void setEnv(const QMap<QString, QString> &env);
    /**
     * @brief Get project environment variables
     * @details This function will return project environment variables.
     * @return environment variable values map
     */
    const QMap<QString, QString> &getEnv();
    /**
     * @brief Is the project exist on filesystem
     * @details This function will check if the project exist on filesystem.
     * @param projectName The name of the project
     * @retval true Project exist in the project directory
     * @retval false Project does not exist in the project directory
     */
    bool isExist(const QString &projectName);
    /**
     * @brief Save project environment variables and settings
     * @details This function will save project environment variables
     *          and settings into project file.
     * @param projectName The name of the project
     * @retval true Save successfully
     * @retval false Save failed
     */
    bool save(const QString &projectName);
    /**
     * @brief Load project environment variables and settings
     * @details This function will load project environment variables
     *          and settings from project file.
     * @param projectName The name of the project
     * @retval true Load successfully
     * @retval false Load failed
     */
    bool load(const QString &projectName);
    /**
     * @brief Auto load the first found project
     * @details Automatically loads the first project file found in the specified
     *          directory. It initializes project environment variables and
     *          settings based on this file.
     * @retval true Load successfully
     * @retval false Load failed
     */
    bool loadFirst();
    /**
     * @brief Remove project file
     * @details This function will remove project file which in the project
     *          directory.
     * @param projectName The name of the project
     * @retval true Remove successfully
     * @retval false Remove failed
     */
    bool remove(const QString &projectName);
    /**
     * @brief List matched projects
     * @details This function will list matched projects which in the project
     *          directory.
     * @param projectNameRegex regular expression to match the project name,
     *        default is ".*"
     * @return QStringList The list of project in the project directory
     */
    QStringList list(const QRegularExpression &projectNameRegex = QRegularExpression(".*"));
    /**
     * @brief Validate project environment variables and settings
     * @details This function will check if project environment variables
     *          and settings are valid.
     * @retval true Project environment variables and settings are valid
     * @retval false Project environment variables and settings are invalid
     */
    bool isValid();
    /**
     * @brief Get project node
     * @details This function will return project YAML node.
     * @return YAML::Node & The project node
     */
    const YAML::Node &getProjectNode();
    /**
     * @brief Get project name
     * @details This function will return project name.
     * @return QString & The project name
     */
    const QString &getProjectName();
    /**
     * @brief Get project path
     * @details This function will return project path.
     * @return QString & The project path
     */
    const QString &getProjectPath();
    /**
     * @brief Get project bus path
     * @details This function will return project bus path.
     * @return QString & The project bus path
     */
    const QString &getBusPath();
    /**
     * @brief Get project symbol path
     * @details This function will return project symbol path.
     * @return QString & The project symbol path
     */
    const QString &getSymbolPath();
    /**
     * @brief Get project schematic path
     * @details This function will return project schematic path.
     * @return QString & The project schematic path
     */
    const QString &getSchematicPath();
    /**
     * @brief Get project output path
     * @details This function will return project output path.
     * @return QString & The project output path
     */
    const QString &getOutputPath();
    /**
     * @brief Set project node
     * @details This function will set project YAML node.
     * @param projectNode The project node
     */
    void setProjectNode(const YAML::Node &projectNode);
    /**
     * @brief Set project name
     * @details This function will set project name.
     * @param projectName The project name
     */
    void setProjectName(const QString &projectName);
    /**
     * @brief Set project path
     * @details This function will set project path.
     * @param projectPath The project path
     */
    void setProjectPath(const QString &projectPath);
    /**
     * @brief Set project bus path
     * @details This function will set project bus path.
     * @param busPath The bus path
     */
    void setBusPath(const QString &busPath);
    /**
     * @brief Set project symbol path
     * @details This function will set project symbol path.
     * @param symbolPath The symbol path
     */
    void setSymbolPath(const QString &symbolPath);
    /**
     * @brief Set project schematic path
     * @details This function will set project schematic path.
     * @param schematicPath The schematic path
     */
    void setSchematicPath(const QString &schematicPath);
    /**
     * @brief Set project output path
     * @details This function will set project output path.
     * @param outputPath The output path
     */
    void setOutputPath(const QString &outputPath);

private:
    /* Project environment variables map */
    QMap<QString, QString> env;
    /* Project YAML node */
    YAML::Node projectNode;
    /* Project name */
    QString projectName;
    /* Project path */
    QString projectPath;
    /* Project bus path */
    QString busPath;
    /* Project symbol path */
    QString symbolPath;
    /* Project schematic path */
    QString schematicPath;
    /* Project output path */
    QString outputPath;

    /**
     * @brief Get simplified path
     * @details This function will get simplified path.
     * @param path The path
     * @return QString The simplified path

     */
    QString getSimplifyPath(const QString &path);
    /**
     * @brief Get expanded path
     * @details This function will get expanded path.
     * @param path The path
     * @return QString The expanded path
     */
    QString getExpandPath(const QString &path);

signals:
};

#endif // QSOCPROJECTMANAGER_H
