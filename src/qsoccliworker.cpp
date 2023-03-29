#include "qsoccliworker.h"

#include <QString>
#include <QTimer>
#include <QtGlobal>

#include "qslangdriver.h"
#include "qstaticlog.h"

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{}

QSocCliWorker::~QSocCliWorker() {}

void QSocCliWorker::setup(const QCoreApplication &application)
{
    /* Cause application to exit when finished */
    QObject::connect(this, SIGNAL(finished()), &application, SLOT(quit()));
    /* This will run the task from the application event loop */
    QTimer::singleShot(0, this, SLOT(run()));
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
    }
}

void QSocCliWorker::run()
{
    if (parser) {
        const QString selfName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        QStaticLog::logD(Q_FUNC_INFO, "Start " + selfName + " in CLI mode.");
        if (parser->isSet("filelist")) {
            const QString fileListName = parser->value("filelist");
            processFileList(fileListName);
        }
    }

    QCoreApplication::exit(0);
    QCoreApplication::processEvents();

    emit finished();
}
