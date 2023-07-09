#ifndef QSOCPROJECTMANAGER_H
#define QSOCPROJECTMANAGER_H

#include <QMap>
#include <QObject>

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
     * @param parent parent object
     * @details This constructor will initialize project environment
     */
    explicit QSocProjectManager(QObject *parent = nullptr);

public slots:
    /**
     * @brief Set project environment variable
     * @param key environment variable key
     * @param value environment variable value
     * @details This function will set project environment variable
     */
    void setEnv(const QString &key, const QString &value);
    /**
     * @brief Set project environment variables
     * @param env environment variables
     * @details This function will set project environment variables
     */
    void setEnv(const QMap<QString, QString> &env);
    /**
     * @brief Get project environment variables
     * @return environment variable values map
     * @details This function will return project environment variables
     */
    const QMap<QString, QString> &getEnv();
    /**
     * @brief Is the project exist on filesystem
     * @param projectName 
     * @return true 
     * @return false 
     * @details This function will check if the project exist on filesystem
     */
    bool isExist(const QString &projectName);
    /**
     * @brief Save project environment variables and settings
     * @param projectName project name
     * @return true if save successfully, otherwise false
     * @details This function will save project environment variables
     *          and settings into project file
     */
    bool save(const QString &projectName);
    /**
     * @brief Load project environment variables and settings
     * @param projectName project name
     * @return true if load successfully, otherwise false
     * @details This function will load project environment variables
     *          and settings from project file
     */
    bool load(const QString &projectName);
    /**
     * @brief Auto load project environment variables and settings
     * @return true if load successfully, otherwise false
     * @details This function will load project environment variables
     *          and settings from project file automatically
     */
    bool autoLoad();
    /**
     * @brief Validate project environment variables and settings
     * @return true if valid, otherwise false
     * @details This function will check if project environment variables
     *          and settings are valid
     */
    bool isValid();
    /**
     * @brief Get project name
     * @return project name
     * @details This function will return project name
     */
    const QString &getProjectName();
    /**
     * @brief Get project path
     * @return project path
     * @details This function will return project path
     */
    const QString &getProjectPath();
    /**
     * @brief Get project bus path
     * @return project bus path
     * @details This function will return project bus path
     */
    const QString &getBusPath();
    /**
     * @brief Get project symbol path
     * @return project symbol path
     * @details This function will return project symbol path
     */
    const QString &getSymbolPath();
    /**
     * @brief Get project schematic path
     * @return project schematic path
     * @details This function will return project schematic path
     */
    const QString &getSchematicPath();
    /**
     * @brief Get project output path
     * @return project output path
     * @details This function will return project output path
     */
    const QString &getOutputPath();
    /**
     * @brief Set project name
     * @param projectName project name
     * @details This function will set project name
     */
    void setProjectName(const QString &projectName);
    /**
     * @brief Set project path
     * @param projectPath project path
     * @details This function will set project path
     */
    void setProjectPath(const QString &projectPath);
    /**
     * @brief Set project bus path
     * @param busPath bus path
     * @details This function will set project bus path
     */
    void setBusPath(const QString &busPath);
    /**
     * @brief Set project symbol path
     * @param symbolPath symbol path
     * @details This function will set project symbol path
     */
    void setSymbolPath(const QString &symbolPath);
    /**
     * @brief Set project schematic path
     * @param schematicPath schematic path
     * @details This function will set project schematic path
     */
    void setSchematicPath(const QString &schematicPath);
    /**
     * @brief Set project output path
     * @param outputPath output path
     * @details This function will set project output path
     */
    void setOutputPath(const QString &outputPath);

private:
    /* Project environment variables map */
    QMap<QString, QString> env;
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
     * @brief Get simplify path
     * @param path path
     * @return simplify path
     * @details This function will simplify path
     */
    QString getSimplifyPath(const QString &path);
    /**
     * @brief Get expand path
     * @param path path
     * @return expand path
     * @details This function will expand path
     */
    QString getExpandPath(const QString &path);

signals:
};

#endif // QSOCPROJECTMANAGER_H
