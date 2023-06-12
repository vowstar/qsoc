#ifndef QSOCPROJECTMANAGER_H
#define QSOCPROJECTMANAGER_H

#include <QObject>

class QSocProjectManager : public QObject
{
    Q_OBJECT
public:
    explicit QSocProjectManager(QObject *parent = nullptr);

public slots:
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
    QString projectPath;
    QString busPath;
    QString symbolPath;
    QString schematicPath;
    QString outputPath;

signals:
};

#endif // QSOCPROJECTMANAGER_H
