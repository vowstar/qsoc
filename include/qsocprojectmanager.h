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
     * @brief QSocProjectManager
     * @param parent
     * @details This constructor will initialize project environment
     */
    explicit QSocProjectManager(QObject *parent = nullptr);

public slots:
    /**
     * @brief setEnv
     * @param key
     * @param value
     * @details This function will set project environment variable
     */
    void setEnv(const QString &key, const QString &value);
    /**
     * @brief setEnv
     * @param env
     * @details This function will set project environment variables
     */
    void setEnv(const QMap<QString, QString> &env);
    /**
     * @brief save
     * @param projectName
     * @return
     * @details This function will save project environment variables
     *          and settings into project file
     */
    bool save(const QString &projectName);
    /**
     * @brief load
     * @param projectName
     * @return
     * @details This function will load project environment variables
     *          and settings from project file
     */
    bool load(const QString &projectName);
    /**
     * @brief autoLoad
     * @return
     * @details This function will load project environment variables
     *         and settings from project file automatically
     */
    bool autoLoad();
    /**
     * @brief isValid
     * @return
     * @details This function will check if project environment variables
     *          and settings are valid
     */
    bool isValid();
    /**
     * @brief getProjectName
     * @return
     * @details This function will return project name
     */
    const QString &getProjectName();
    /**
     * @brief getProjectPath
     * @return
     * @details This function will return project path
     */
    const QString &getProjectPath();
    /**
     * @brief getBusPath
     * @return
     * @details This function will return bus path
     */
    const QString &getBusPath();
    /**
     * @brief getSymbolPath
     * @return
     * @details This function will return symbol path
     */
    const QString &getSymbolPath();
    /**
     * @brief getSchematicPath
     * @return
     * @details This function will return schematic path
     */
    const QString &getSchematicPath();
    /**
     * @brief getOutputPath
     * @return
     * @details This function will return output path
     */
    const QString &getOutputPath();
    /**
     * @brief setProjectName
     * @param projectName
     * @details This function will set project name
     */
    void setProjectName(const QString &projectName);
    /**
     * @brief setProjectPath
     * @param projectPath
     * @details This function will set project path
     */
    void setProjectPath(const QString &projectPath);
    /**
     * @brief setBusPath
     * @param busPath
     * @details This function will set bus path
     */
    void setBusPath(const QString &busPath);
    /**
     * @brief setSymbolPath
     * @param symbolPath
     * @details This function will set symbol path
     */
    void setSymbolPath(const QString &symbolPath);
    /**
     * @brief setSchematicPath
     * @param schematicPath
     * @details This function will set schematic path
     */
    void setSchematicPath(const QString &schematicPath);
    /**
     * @brief setOutputPath
     * @param outputPath
     * @details This function will set output path
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
     * @brief getSimplifyPath
     * @param path
     * @return
     * @details This function will simplify path
     */
    QString getSimplifyPath(const QString &path);
    /**
     * @brief getExpandPath
     * @param path
     * @return
     * @details This function will expand path
     */
    QString getExpandPath(const QString &path);

signals:
};

#endif // QSOCPROJECTMANAGER_H
