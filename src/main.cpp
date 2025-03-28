// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qstaticicontheme.h"
#include "common/qstaticlog.h"
#include "common/qstatictranslator.h"
#include "gui/mainwindow/mainwindow.h"

#include <QApplication>

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
    int result = 0;
    /* Install message handler to direct outputs to appropriate streams */
    QStaticLog::installMessageHandler();
    /* Check if GUI mode is requested */
    if (isGui(argc, argv)) {
        const QApplication app(argc, argv);
        QStaticTranslator::setup();
        QSocCliWorker socCliWorker;
        socCliWorker.setup(app.arguments(), true);
        QStaticIconTheme::setup();
        MainWindow mainWindow;
        mainWindow.show();
        result = app.exec();
    } else {
        const QCoreApplication app(argc, argv);
        QStaticTranslator::setup();
        QSocCliWorker socCliWorker;
        socCliWorker.setup(app.arguments(), false);
        result = app.exec();
    }

    /* Restore original message handler before exiting */
    QStaticLog::restoreMessageHandler();

    return result;
}
