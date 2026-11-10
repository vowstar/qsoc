// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"
#include "common/qstatictranslator.h"
#include "gui/mainwindow/mainwindow.h"
#include "gui/qstaticicontheme.h"
#include "gui/report/qsocprimitivereportrenderer.h"

#include <cstring>
#include <QApplication>
#include <QDebug>
#include <QIcon>

int main(int argc, char *argv[])
{
    if (argc == 4 && std::strcmp(argv[1], "--render-primitive-report") == 0) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        /* The generated runtime uses valid JavaScript var hoisting. */
        qputenv("QT_LOGGING_RULES", qgetenv("QT_LOGGING_RULES") + "\nqt.qml.usedbeforedeclared=false");
        const QGuiApplication app(argc, argv);
        const QStringList     arguments = app.arguments();
        if (arguments.size() != 4) {
            qCritical() << "Expected a report request and output base";
            return 1;
        }
        QString error;
        if (renderPrimitiveReport(arguments[2], arguments[3], &error))
            return 0;
        qCritical().noquote() << error;
        return 1;
    }
    QSocConsole::install();
    QSocProxy::ensureSystemBootstrap();

    int result = 0;
    {
        const QApplication app(argc, argv);
        /* The same name as the CLI keeps settings and data paths shared. */
        QCoreApplication::setApplicationName(QStringLiteral("QSoC"));
        QCoreApplication::setApplicationVersion(QStringLiteral(QSOC_VERSION));
        QGuiApplication::setDesktopFileName(QStringLiteral("qsoc"));
        QApplication::setWindowIcon(QIcon(QStringLiteral(":/qsoc.svg")));
        QStaticTranslator::setup();
        QStaticIconTheme::setup();
        MainWindow mainWindow;
        mainWindow.show();
        result = app.exec();
    }

    QSocConsole::restore();
    return result;
}
