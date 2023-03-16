#ifndef QSOCCLIWORKER_H
#define QSOCCLIWORKER_H

#include <QObject>
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QMutex>
#include <QThread>

class QSocCliWorker : public QObject
{
    Q_OBJECT
public:
    explicit QSocCliWorker(QObject *parent = nullptr);
    ~QSocCliWorker();
    void setup(QThread &thread);
    void setParser(QCommandLineParser *parser);

public slots:
    void run();

private:
    QCommandLineParser *parser;
    QMutex mutex;

    void processFileList(const QString &fileListName);

signals:
    void finished();
};

#endif // QSOCCLIWORKER_H
