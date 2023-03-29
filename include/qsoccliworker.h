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
    void setup(const QCoreApplication &application);
    void setParser(QCommandLineParser *parser);

public slots:
    void run();

private:
    QCommandLineParser *parser;

    void processFileList(const QString &fileListName);

signals:
    void finished();
};

#endif // QSOCCLIWORKER_H
