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
    void parseSymbol();
    void processFileList(const QString &fileListName);

signals:
    void exit(int returnCode = 0);
    void quit();
};

#endif // QSOCCLIWORKER_H
