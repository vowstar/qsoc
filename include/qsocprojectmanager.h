#ifndef QSOCPROJECTMANAGER_H
#define QSOCPROJECTMANAGER_H

#include <QMap>
#include <QObject>

class QSocProjectManager : public QObject
{
    Q_OBJECT
public:
    explicit QSocProjectManager(QObject *parent = nullptr);

public slots:
    void           setEnv(const QString &key, const QString &value);
    void           setEnv(const QMap<QString, QString> &env);
    bool           create(const QString &projectName);
    const QString &getProjectPath();
    const QString &getBusPath();
    const QString &getSymbolPath();
    const QString &getSchematicPath();
    const QString &getOutputPath();
    void           setProjectPath(const QString &projectPath);
    void           setBusPath(const QString &busPath);
    void           setSymbolPath(const QString &symbolPath);
    void           setSchematicPath(const QString &schematicPath);
    void           setOutputPath(const QString &outputPath);

private:
    /* Project environment variables map */
    QMap<QString, QString> env;
    /* Project private variables */
    QString projectName;
    QString projectPath;
    QString busPath;
    QString symbolPath;
    QString schematicPath;
    QString outputPath;

    QString getSimplifyPath(const QString &path);
    QString getExpandPath(const QString &path);

signals:
};

#endif // QSOCPROJECTMANAGER_H
