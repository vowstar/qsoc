#ifndef QSOCCLIWORKER_H
#define QSOCCLIWORKER_H

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QObject>

class QSocCliWorker : public QObject
{
    Q_OBJECT
public:
    explicit QSocCliWorker(QObject *parent = nullptr);
    ~QSocCliWorker();
    void setup(bool isGui = false);
    void process();

public slots:
    void run();

private:
    QCommandLineParser parser;

    void parseRoot(const QStringList &appArguments);
    void parseProject(const QStringList &appArguments);
    void parseProjectCreate(const QStringList &appArguments);
    void parseProjectUpdate(const QStringList &appArguments);
    void parseProjectRemove(const QStringList &appArguments);
    void parseSymbol(const QStringList &appArguments);
    void parseSymbolImport(const QStringList &appArguments);
    void parseSymbolUpdate(const QStringList &appArguments);
    void parseSymbolRemove(const QStringList &appArguments);
    void processFileList(const QString &fileListPath, const QStringList &filePathList);

signals:
    void exit(int returnCode = 0);
    void quit();
};

#endif // QSOCCLIWORKER_H
