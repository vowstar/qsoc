// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"
#include "common/qsocsibling.h"
#include "common/qsocwinconsole.h"
#include "common/qstatictranslator.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#ifndef Q_OS_WIN
#include <cerrno>
#include <unistd.h>
#endif

namespace {

int guiArgument(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (argument == "--")
            return i + 1 < argc && qstrcmp(argv[i + 1], "gui") == 0 ? i + 1 : 0;
        if (argument == "--verbose" || argument == "--color") {
            ++i;
            continue;
        }
        if (argument.startsWith("--verbose=") || argument.startsWith("--color="))
            continue;
        return argument == "gui" ? i : 0;
    }
    return 0;
}

/* Replace this process with qsoc-gui, or start it and return on Windows. */
int launchGui(int argc, char *argv[], int guiIndex)
{
    const QString program = QSocSibling::path(QStringLiteral("qsoc-gui"));
    if (program.isEmpty()) {
        std::cerr << "qsoc: "
                  << QSocSibling::missingMessage(QStringLiteral("qsoc-gui")).toStdString()
                  << std::endl;
        return 127;
    }
#ifdef Q_OS_WIN
    QStringList arguments = QCoreApplication::arguments().mid(1);
    arguments.removeAt(guiIndex - 1);
    return QProcess::startDetached(program, arguments) ? 0 : 1;
#else
    const QByteArray    path = QFile::encodeName(program);
    std::vector<char *> args{const_cast<char *>(path.constData())};
    for (int i = 1; i < argc; ++i) {
        if (i != guiIndex)
            args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    ::execv(path.constData(), args.data());
    std::cerr << "qsoc: cannot start " << path.constData() << ": " << std::strerror(errno)
              << std::endl;
    return 127;
#endif
}

} /* namespace */

int main(int argc, char *argv[])
{
    /* Force UTF-8 + VT on Windows console before any output happens. No-op
       on POSIX. Must run before QSocConsole::install() so the first byte
       written lands in a console already configured for UTF-8 + ANSI. */
    QSocWinConsole::bootstrap();
    /* Install message handler to direct outputs to appropriate streams */
    QSocConsole::install();
    /* Bootstrap QNetworkProxyFactory once so QNetworkProxy::DefaultProxy
     * resolves to the system / environment proxy across every later
     * QNetworkAccessManager (LLM, MCP, web tools). Per-target overrides
     * still win via QSocProxy::resolve(). */
    QSocProxy::ensureSystemBootstrap();

    int result = 0;
    {
        const QCoreApplication app(argc, argv);
        if (const int guiIndex = guiArgument(argc, argv)) {
            result = launchGui(argc, argv, guiIndex);
        } else {
            QStaticTranslator::setup();
            QSocCliWorker socCliWorker;
            socCliWorker.setup(app.arguments(), false);
            result = app.exec();
        }
    }

    /* Restore original message handler before exiting */
    QSocConsole::restore();
    /* Restore the original console code page / mode (Windows only). */
    QSocWinConsole::restore();

    return result;
}
