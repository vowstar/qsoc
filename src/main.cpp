#include "mainwindow.h"

#include <QApplication>

#include "qsoccliworker.h"
#include "qstaticlog.h"
#include "qstatictranslator.h"

bool isGui(int &argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (0 == qstrcmp(argv[i], "gui"))
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    QStaticLog::setLevel(QStaticLog::levelS);

    int result = 0;

    if (isGui(argc, argv)) {
        const QApplication app(argc, argv);
        QStaticTranslator::setup();
        QSocCliWorker socCliWorker;
        socCliWorker.setup(true);
        MainWindow mainWindow;
        mainWindow.show();
        result = app.exec();
    } else {
        const QCoreApplication app(argc, argv);
        QStaticTranslator::setup();
        QSocCliWorker socCliWorker;
        socCliWorker.setup(false);
        result = app.exec();
    }
    return result;
}
