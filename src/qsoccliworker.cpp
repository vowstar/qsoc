#include "qsoccliworker.h"

#include <QString>
#include <QtGlobal>

#include "qslangdriver.h"

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{}

QSocCliWorker::~QSocCliWorker() {}

void QSocCliWorker::setup(QThread &thread)
{
    connect(&thread, SIGNAL(started()), this, SLOT(run()));
    connect(this, SIGNAL(finished()), &thread, SLOT(quit()));
}

void QSocCliWorker::setParser(QCommandLineParser *parser)
{
    this->parser = parser;
}

void QSocCliWorker::processFileList(const QString &fileListName)
{
    QSlangDriver driver(this);
    if (driver.parseFileList(fileListName)) {
        /* Parse success */
    } else {
        /* Parse fail */
    }
}

void QSocCliWorker::run()
{
    mutex.lock();

    if (parser) {
        QString selfName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

        if (parser->isSet("filelist")) {
            QString fileListName = parser->value("filelist");
            processFileList(fileListName);
        }
    }

    QCoreApplication::exit(0);
    QCoreApplication::processEvents();
    mutex.unlock();

    emit finished();
}
