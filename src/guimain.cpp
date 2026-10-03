// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"
#include "common/qstaticicontheme.h"
#include "common/qstatictranslator.h"
#include "gui/mainwindow/mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
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
