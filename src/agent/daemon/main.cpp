// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file main.cpp
 * @brief qsoc-agentd: the agent daemon.
 * @details Hosts QSocAgentRuntime sessions behind a local socket. Any
 *          frontend (TUI, GUI, web) connects, opens a session and drives
 *          turns; all agent infrastructure lives here.
 */

#include "agent/daemon/qsocagentdaemon.h"
#include "agent/daemon/qsocdaemonresources.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocinterrupt.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsocprocessowner.h"
#include "common/qsocproxy.h"
#include "common/qsocwinconsole.h"

#include <csignal>
#include <iostream>
#include <limits>
#include <QCommandLineParser>
#include <QCoreApplication>
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
        QStringLiteral("QSoC agent daemon: agent infrastructure behind a local socket."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(
        {QStringLiteral("s"), QStringLiteral("socket")},
        QStringLiteral("Local endpoint (default: $XDG_RUNTIME_DIR/qsoc/agentd.sock)."),
        QStringLiteral("path"));
    QCommandLineOption foregroundOption(
        {QStringLiteral("f"), QStringLiteral("foreground")},
        QStringLiteral("Stay in the foreground (default; no daemonising is done)."));
    parser.addOption(socketOption);
    parser.addOption(foregroundOption);
    QCommandLineOption parentOption(
        QStringLiteral("parent-pid"),
        QStringLiteral("Exit when the owning parent process exits."),
        QStringLiteral("pid"));
    parser.addOption(parentOption);
    QCommandLineOption sessionOption(
        {QStringLiteral("session-worker"), QStringLiteral("session")},
        QStringLiteral("Serve one connection, then exit."));
    sessionOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(sessionOption);
    QCommandLineOption resourceOption(QStringLiteral("resource-probe"));
    resourceOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(resourceOption);
    QCommandLineOption memoryReserveOption(
        QStringLiteral("smt-memory-reserve-mib"),
        QStringLiteral("Extra memory headroom required before starting an SMT worker."),
        QStringLiteral("MiB"),
        QStringLiteral("512"));
    QCommandLineOption memoryStrictOption(
        QStringLiteral("smt-memory-strict"),
        QStringLiteral("Wait for a fresh effective-memory sample before starting SMT workers."));
    parser.addOption(memoryReserveOption);
    parser.addOption(memoryStrictOption);
    parser.process(app);
    bool              reserveOk   = false;
    const quint64     reserveMiB  = parser.value(memoryReserveOption).toULongLong(&reserveOk);
    constexpr quint64 bytesPerMiB = 1024 * 1024;
    if (!reserveOk
        || reserveMiB > static_cast<quint64>((std::numeric_limits<qint64>::max)()) / bytesPerMiB) {
        std::cerr << "qsoc-agentd: invalid SMT memory reserve" << std::endl;
        return 2;
    }
    const QSocMemoryBudget::Policy
                 memoryPolicy{parser.isSet(memoryStrictOption), reserveMiB * bytesPerMiB};
    bool         parentOk  = false;
    const qint64 parentPid = parser.value(parentOption).toLongLong(&parentOk);
    if (parser.isSet(parentOption) && (!parentOk || parentPid <= 1))
        return 2;
    QSocProcessOwner owner;
    if (parentOk && !owner.watch(parentPid)) {
        std::cerr << "qsoc-agentd: could not watch the owning process" << std::endl;
        return 1;
    }
    if (parser.isSet(resourceOption)) {
        const int code = parentOk ? QSocDaemonResources::runProbe(parentPid) : 2;
        QSocConsole::restore();
        QSocWinConsole::restore();
        return code;
    }
    if (!QSocInterrupt::installBridge())
        return 1;
    std::signal(SIGTERM, requestTermination);

    const QString requestedPath = parser.value(socketOption);
    const QString socketPath = requestedPath.isEmpty() ? QString()
                                                       : QSocLocalEndpoint::resolve(requestedPath);

    QSocAgentDaemon daemon(socketPath, nullptr, memoryPolicy);
    daemon.setSingleSession(parser.isSet(sessionOption));
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
        if (terminationRequested || QSocInterrupt::requested()) {
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
