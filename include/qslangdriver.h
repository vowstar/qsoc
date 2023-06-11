#ifndef QSLANGDRIVER_H
#define QSLANGDRIVER_H

#include <QMap>
#include <QObject>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class QSlangDriver : public QObject
{
    Q_OBJECT
public:
    explicit QSlangDriver(QObject *parent = nullptr);
    ~QSlangDriver();

public slots:
    void               setEnv(const QString &key, const QString &value);
    void               setEnv(const QMap<QString, QString> &env);
    bool               parseArgs(const QString &args);
    bool               parseFileList(const QString &fileListPath, const QStringList &filePathList);
    const json        &getAst();
    const json        &getModuleAst(const QString &moduleName);
    const QStringList &getModuleList();

private:
    QMap<QString, QString> env;
    json                   ast;
    QStringList            moduleList;
};

#endif // QSLANGDRIVER_H
