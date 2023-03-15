#include "qsoccliworker.h"

#include <QTextStream>

#include <QtGlobal>
#include <QString>

#include <slang/driver/Driver.h>
#include <slang/util/Version.h>

using namespace slang;
using namespace slang::driver;

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{
    Driver driver;
    driver.addStandardArgs();

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
