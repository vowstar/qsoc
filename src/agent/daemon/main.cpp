// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file main.cpp
 * @brief qsoc-agentd: the agent daemon.
 * @details Hosts QSocAgentRuntime sessions behind a unix socket. Any
 *          frontend (TUI, GUI, web) connects, opens a session and drives
 *          turns; all agent infrastructure lives here.
 */

#include "agent/daemon/qsocagentdaemon.h"
#include "agent/remote/qsocinterrupt.h"
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"
#include "common/qsocwinconsole.h"

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <csignal>
#include <iostream>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QTimer>

namespace {

volatile std::sig_atomic_t terminationRequested = 0;
void                       requestTermination(int)
{
    terminationRequested = 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QSocWinConsole::bootstrap();
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QSoC"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QSOC_VERSION));
    QSocConsole::install();
    QSocProxy::ensureSystemBootstrap();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("QSoC agent daemon: agent infrastructure behind a unix socket."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(
        {QStringLiteral("s"), QStringLiteral("socket")},
        QStringLiteral("Unix socket path (default: $XDG_RUNTIME_DIR/qsoc/agentd.sock)."),
        QStringLiteral("path"));
    QCommandLineOption foregroundOption(
        {QStringLiteral("f"), QStringLiteral("foreground")},
        QStringLiteral("Stay in the foreground (default; no daemonising is done)."));
    parser.addOption(socketOption);
    parser.addOption(foregroundOption);
    QCommandLineOption parentOption(
        QStringLiteral("parent-pid"),
        QStringLiteral("Exit when the owning TUI process exits."),
        QStringLiteral("pid"));
    parser.addOption(parentOption);
    parser.process(app);
    bool         parentOk  = false;
    const qint64 parentPid = parser.value(parentOption).toLongLong(&parentOk);
    if (parser.isSet(parentOption) && (!parentOk || parentPid <= 1))
        return 2;
#ifdef Q_OS_LINUX
    // The timer handles orderly shutdown; the kernel also covers owner death
    // while a tool is blocking the daemon's event loop.
    if (parentOk && (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parentPid))
        return 1;
#endif
#ifdef Q_OS_WIN
    /* Holding a handle keeps the owner's pid from being reused under us. */
    HANDLE owner = parentOk ? ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid))
                            : nullptr;
    if (parentOk && owner == nullptr)
        return 1;
    const auto closeOwner = qScopeGuard([owner] {
        if (owner != nullptr)
            ::CloseHandle(owner);
    });
#endif
    if (!QSocInterrupt::installBridge())
        return 1;
    std::signal(SIGTERM, requestTermination);

    const QString socketPath = parser.value(socketOption);

    QSocAgentDaemon daemon(socketPath);
    QObject::connect(&daemon, &QSocAgentDaemon::connectionCountChanged, [](int count) {
        QSocConsole::debug() << "agentd: connections:" << count;
    });
    if (!daemon.start()) {
        std::cerr << "qsoc-agentd: " << daemon.error().toStdString() << std::endl;
        QSocConsole::restore();
        QSocWinConsole::restore();
        return 1;
    }
    const std::string effectivePath = socketPath.isEmpty()
                                          ? QSocAgentDaemon::defaultSocketPath().toStdString()
                                          : socketPath.toStdString();
    std::cout << "qsoc-agentd " << QSOC_VERSION << " listening on " << effectivePath << std::endl;

    QTimer lifecycle;
    QObject::connect(&lifecycle, &QTimer::timeout, &app, [&] {
        bool ownerGone = false;
#ifdef Q_OS_UNIX
        ownerGone = parentOk && static_cast<qint64>(::getppid()) != parentPid;
#elif defined(Q_OS_WIN)
        ownerGone = owner != nullptr && ::WaitForSingleObject(owner, 0) == WAIT_OBJECT_0;
#endif
        if (terminationRequested || ownerGone || QSocInterrupt::requested()) {
            daemon.shutdown();
            app.quit();
        }
    });
    lifecycle.start(50);

    const int code = app.exec();

    daemon.shutdown();
    QSocConsole::restore();
    QSocWinConsole::restore();
    return code;
}
