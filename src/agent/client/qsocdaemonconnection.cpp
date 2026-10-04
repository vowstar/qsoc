// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocdaemonconnection.h"
#include "common/qsocsibling.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

QSocDaemonConnection::QSocDaemonConnection(const QString &endpoint)
    : endpoint_(endpoint.isEmpty() ? directory_.filePath("agent.sock") : endpoint)
    , owned_(endpoint.isEmpty())
    , client_(std::make_unique<QSocAgentDaemonClient>(endpoint_))
{}

QSocDaemonConnection::~QSocDaemonConnection()
{
    client_.reset();
    if (owned_ && child_.state() != QProcess::NotRunning) {
        child_.terminate();
        if (!child_.waitForFinished(3000)) {
            child_.kill();
            child_.waitForFinished(1000);
        }
    }
}

bool QSocDaemonConnection::start()
{
    if (owned_) {
        const QString program = QSocSibling::path(QStringLiteral("qsoc-agentd"));
        if (program.isEmpty()) {
            error_ = QSocSibling::missingMessage(QStringLiteral("qsoc-agentd"));
            return false;
        }
        if (!directory_.isValid()) {
            error_ = QStringLiteral("Could not create daemon socket directory.");
            return false;
        }
        child_.setStandardInputFile(QProcess::nullDevice());
        child_.setStandardOutputFile(QProcess::nullDevice());
        child_.setStandardErrorFile(directory_.filePath("daemon.log"));
#ifdef Q_OS_UNIX
        child_.setChildProcessModifier([] {
            if (::setsid() < 0)
                ::_exit(127);
        });
#elif defined(Q_OS_WIN)
        child_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NEW_PROCESS_GROUP;
        });
#endif
        child_.start(
            program,
            {"--socket",
             endpoint_,
             "--parent-pid",
             QString::number(QCoreApplication::applicationPid())});
        if (!child_.waitForStarted(5000)) {
            error_ = child_.errorString();
            return false;
        }
    }
    QDeadlineTimer startup(5000);
    while (!client_->connectToDaemon(static_cast<int>(startup.remainingTime()))) {
        if (!owned_ || startup.hasExpired() || child_.state() == QProcess::NotRunning) {
            error_ = QStringLiteral("Could not connect to agent daemon: %1").arg(client_->error());
            return false;
        }
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    if (owned_ && client_->daemonProcessId() != child_.processId()) {
        error_ = QStringLiteral("Agent daemon socket is served by another process.");
        return false;
    }
    return true;
}
