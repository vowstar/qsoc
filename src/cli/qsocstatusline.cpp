// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocstatusline.h"

#include "common/qsocshellpath.h"

#include <QStringList>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

QSocStatusLine::QSocStatusLine(QObject *parent)
    : QObject(parent)
{
    debounce_.setSingleShot(true);
    debounce_.setInterval(300);
    connect(&debounce_, &QTimer::timeout, this, &QSocStatusLine::fire);

    killTimer_.setSingleShot(true);
    connect(&killTimer_, &QTimer::timeout, this, [this]() {
        if (process_ != nullptr) {
            dropProcess();
            emit textReady(QString());
        }
    });
}

QSocStatusLine::~QSocStatusLine()
{
    dropProcess();
}

void QSocStatusLine::setCommand(const QString &command)
{
    command_ = command.trimmed();
    if (command_.isEmpty()) {
        debounce_.stop();
        dropProcess();
        emit textReady(QString());
    }
}

void QSocStatusLine::setWorkingDirectory(const QString &dir)
{
    workingDir_ = dir;
}

void QSocStatusLine::setTimeoutMs(int timeoutMs)
{
    timeoutMs_ = timeoutMs > 0 ? timeoutMs : 5000;
}

void QSocStatusLine::setDebounceMs(int debounceMs)
{
    debounce_.setInterval(debounceMs >= 0 ? debounceMs : 0);
}

void QSocStatusLine::requestRefresh(const nlohmann::json &payload)
{
    if (command_.isEmpty()) {
        return;
    }
    pending_ = payload;
    debounce_.start();
}

void QSocStatusLine::dropProcess()
{
    killTimer_.stop();
    if (process_ == nullptr) {
        return;
    }
    QProcess *old = process_.data();
    process_.clear();
    old->disconnect(this);
    if (old->state() != QProcess::NotRunning) {
        /* The command runs as `sh -c "..."` and may have forked (sleep,
         * curl): kill the whole process group, not just the leader. */
#ifdef Q_OS_UNIX
        const qint64 pid = old->processId();
        if (pid > 0) {
            ::kill(-static_cast<pid_t>(pid), SIGKILL);
        }
#endif
        old->kill();
    }
    old->deleteLater();
}

void QSocStatusLine::fire()
{
    dropProcess();

    const QString shellExe = QSocShellPath::bashPath();
    if (shellExe.isEmpty()) {
        emit textReady(QString());
        return;
    }

    auto *process = new QProcess(this);
    process_      = process;
#if defined(Q_OS_UNIX) && QT_VERSION >= 0x060000
    /* Own process group so a timeout kill reaps forked children too. */
    process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    });
#endif
    process->setProcessChannelMode(QProcess::SeparateChannels);
    if (!workingDir_.isEmpty()) {
        process->setWorkingDirectory(workingDir_);
    }

    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
            if (process_ != process) {
                return;
            }
            killTimer_.stop();
            process_.clear();
            process->deleteLater();
            if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                emit textReady(QString());
                return;
            }
            /* One row on screen: publish the first non-empty line. */
            const QString out = QString::fromUtf8(process->readAllStandardOutput());
            QString       line;
            for (const QString &candidate : out.split(QLatin1Char('\n'))) {
                if (!candidate.trimmed().isEmpty()) {
                    line = candidate.trimmed();
                    break;
                }
            }
            emit textReady(line);
        });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        if (process_ != process || process->error() != QProcess::FailedToStart) {
            return;
        }
        killTimer_.stop();
        process_.clear();
        process->deleteLater();
        emit textReady(QString());
    });

    process->start(shellExe, QStringList() << QStringLiteral("-c") << command_);

    /* Hand the payload to the child and close stdin so naive readers
     * (cat / jq) terminate cleanly. */
    const std::string serialized = pending_.dump();
    process->write(serialized.data(), static_cast<qint64>(serialized.size()));
    process->write("\n", 1);
    process->closeWriteChannel();

    killTimer_.start(timeoutMs_);
}
