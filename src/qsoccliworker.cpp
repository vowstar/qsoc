#include "qsoccliworker.h"

#include <QTextStream>

#include <QtGlobal>
#include <QString>

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{

}

QSocCliWorker::~QSocCliWorker()
{

}

void QSocCliWorker::setup(QThread &thread)
{
    connect(&thread, SIGNAL(started()), this, SLOT(run()));
    connect(this, SIGNAL(finished()), &thread, SLOT(quit()));
}

void QSocCliWorker::setParser(QCommandLineParser *parser)
{
    this->parser = parser;
}

void QSocCliWorker::run()
{
    mutex.lock();

    if (parser)
    {
        QString selfName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    }
    QCoreApplication::exit(0);
    QCoreApplication::processEvents();
    mutex.unlock();

    emit finished();
}
