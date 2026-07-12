// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpstdio.h"

#include "agent/mcp/qsocmcpstdioparser_p.h"

#include "common/qsocconsole.h"

#include <QPointer>
#include <QProcessEnvironment>
#include <QTimer>

#include <utility>

namespace {

constexpr qsizetype kMaximumMessageBytes = 64 * 1024 * 1024;
constexpr int       kGracefulStopMs      = 200;
constexpr int       kForceStopMs         = 600;
constexpr auto      kFrameDelimiter      = "\r\n\r\n";
constexpr qsizetype kFrameDelimiterSize  = 4;
constexpr auto      kContentLengthKey    = "Content-Length:";

} // namespace

QSocMcpStdioTransport::QSocMcpStdioTransport(McpServerConfig config, QObject *parent)
    : QSocMcpTransport(parent)
    , config_(std::move(config))
{}

QSocMcpStdioTransport::~QSocMcpStdioTransport()
{
    lifecycleGeneration_++;
    terminal_       = true;
    acceptMessages_ = false;
    clearInput();

    QProcess *process = process_.data();
    process_          = nullptr;
    if (process == nullptr) {
        return;
    }
    disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(kForceStopMs);
    }
}

void QSocMcpStdioTransport::start()
{
    if (state() != State::Idle && state() != State::Stopped) {
        return;
    }
    lifecycleGeneration_++;
    terminal_       = false;
    acceptMessages_ = true;
    stopRequested_  = false;
    errorReported_  = false;
    clearInput();
    setState(State::Starting);
    createProcess();

    QProcess                             *process    = process_.data();
    const quint64                         generation = lifecycleGeneration_;
    const QPointer<QSocMcpStdioTransport> guard(this);
    process->start(config_.command, config_.args);
    if (guard.isNull() || !isCurrent(process, generation)) {
        return;
    }

    if (config_.connectTimeoutMs > 0) {
        const QPointer<QProcess> processGuard(process);
        QTimer::singleShot(config_.connectTimeoutMs, this, [this, processGuard, generation]() {
            if (processGuard.isNull() || !isCurrent(processGuard.data(), generation)
                || state() != State::Starting) {
                return;
            }
            failCurrent(
                processGuard.data(),
                generation,
                QStringLiteral("MCP server '%1' failed to start in time").arg(config_.name));
        });
    }
}

void QSocMcpStdioTransport::stop()
{
    if (terminal_) {
        setState(State::Stopped);
        return;
    }
    if (stopRequested_ || errorReported_) {
        return;
    }

    QProcess     *process    = process_.data();
    const quint64 generation = lifecycleGeneration_;
    stopRequested_           = true;
    acceptMessages_          = false;
    clearInput();
    setState(State::Stopping);
    if (process == nullptr) {
        terminal_ = true;
        setState(State::Stopped);
        emit closed();
        return;
    }
    requestShutdown(process, generation, true);
}

void QSocMcpStdioTransport::sendMessage(const nlohmann::json &message)
{
    if (process_ == nullptr || state() != State::Running) {
        emit errorOccurred(
            QStringLiteral("MCP transport for server '%1' is not running").arg(config_.name));
        return;
    }

    QByteArray frame;
    try {
        frame = QByteArray::fromStdString(message.dump());
    } catch (const std::exception &) {
        failCurrent(
            process_.data(),
            lifecycleGeneration_,
            QStringLiteral("Could not encode a JSON message for MCP server '%1'").arg(config_.name));
        return;
    }
    if (config_.stdioFraming == McpStdioFraming::Newline && frame.size() > kMaximumMessageBytes) {
        failCurrent(
            process_.data(),
            lifecycleGeneration_,
            QStringLiteral("JSON message for MCP server '%1' exceeds the 64 MiB limit")
                .arg(config_.name));
        return;
    }
    if (config_.stdioFraming == McpStdioFraming::Newline) {
        frame.append('\n');
    } else {
        frame = QByteArrayLiteral("Content-Length: ") + QByteArray::number(frame.size())
                + QByteArrayLiteral("\r\n\r\n") + frame;
    }

    QProcess                             *process    = process_.data();
    const quint64                         generation = lifecycleGeneration_;
    const QPointer<QSocMcpStdioTransport> guard(this);
    const qint64                          written = process->write(frame);
    if (guard.isNull() || !isCurrent(process, generation)) {
        return;
    }
    if (written != frame.size()) {
        failCurrent(
            process,
            generation,
            QStringLiteral("MCP server '%1' could not accept a JSON message").arg(config_.name));
    }
}

void QSocMcpStdioTransport::createProcess()
{
    auto *process = new QProcess(this);
    process_      = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = config_.env.constBegin(); it != config_.env.constEnd(); ++it) {
        env.insert(it.key(), it.value());
    }
    process->setProcessEnvironment(env);

    const quint64 generation = lifecycleGeneration_;
    connect(process, &QProcess::started, this, [this, process, generation]() {
        handleProcessStarted(process, generation);
    });
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, process, generation](int exitCode, QProcess::ExitStatus status) {
            handleProcessFinished(process, generation, exitCode, status);
        });
    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, process, generation](QProcess::ProcessError error) {
            handleProcessError(process, generation, error);
        });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, generation]() {
        handleStandardOutput(process, generation);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process, generation]() {
        handleStandardError(process, generation);
    });
}

void QSocMcpStdioTransport::handleProcessStarted(QProcess *process, quint64 generation)
{
    if (!isCurrent(process, generation)) {
        return;
    }
    if (state() == State::Stopping) {
        requestShutdown(process, generation, stopRequested_);
        return;
    }
    if (state() != State::Starting) {
        return;
    }
    setState(State::Running);
    emit started();
}

void QSocMcpStdioTransport::handleProcessFinished(
    QProcess *process, quint64 generation, int exitCode, QProcess::ExitStatus status)
{
    if (!isCurrent(process, generation)) {
        return;
    }

    setState(State::Stopping);
    handleStandardError(process, generation);
    if (acceptMessages_) {
        const QPointer<QSocMcpStdioTransport> guard(this);
        handleStandardOutput(process, generation, true);
        if (guard.isNull() || !isCurrent(process, generation)) {
            return;
        }
    } else {
        process->readAllStandardOutput();
    }

    if (stopRequested_ || errorReported_) {
        finishCurrent(process, generation);
        return;
    }
    if (status == QProcess::CrashExit) {
        failCurrent(process, generation, QStringLiteral("MCP server '%1' crashed").arg(config_.name));
        return;
    }
    if (exitCode != 0) {
        failCurrent(
            process,
            generation,
            QStringLiteral("MCP server '%1' exited with code %2").arg(config_.name).arg(exitCode));
        return;
    }
    finishCurrent(process, generation);
}

void QSocMcpStdioTransport::handleProcessError(
    QProcess *process, quint64 generation, QProcess::ProcessError error)
{
    if (!isCurrent(process, generation)) {
        return;
    }
    if (stopRequested_) {
        const QPointer<QProcess> processGuard(process);
        QTimer::singleShot(0, this, [this, processGuard, generation]() {
            if (!processGuard.isNull() && isCurrent(processGuard.data(), generation)
                && processGuard->state() == QProcess::NotRunning) {
                finishCurrent(processGuard.data(), generation);
            }
        });
        return;
    }

    QString message;
    if (error == QProcess::FailedToStart) {
        message = QStringLiteral("MCP server '%1' failed to start: %2")
                      .arg(config_.name, process->errorString());
    } else if (error == QProcess::Crashed) {
        message = QStringLiteral("MCP server '%1' crashed").arg(config_.name);
    } else {
        message = QStringLiteral("MCP server '%1' process error: %2")
                      .arg(config_.name, process->errorString());
    }
    failCurrent(process, generation, message);
}

void QSocMcpStdioTransport::handleStandardOutput(QProcess *process, quint64 generation, bool atEnd)
{
    if (!isCurrent(process, generation)) {
        return;
    }
    const QByteArray chunk = process->readAllStandardOutput();
    if (!acceptMessages_) {
        return;
    }
    readBuffer_.append(chunk);
    drainBuffer(process, generation, atEnd);
}

void QSocMcpStdioTransport::handleStandardError(QProcess *process, quint64 generation)
{
    if (!isCurrent(process, generation)) {
        return;
    }
    const QByteArray chunk = process->readAllStandardError();
    if (!chunk.isEmpty()) {
        QSocConsole::debug() << "mcp stdio stderr:" << QString::fromUtf8(chunk).trimmed();
    }
}

bool QSocMcpStdioTransport::drainBuffer(QProcess *process, quint64 generation, bool atEnd)
{
    if (config_.stdioFraming == McpStdioFraming::Newline) {
        return drainNewlineBuffer(process, generation, atEnd);
    }
    return drainContentLengthBuffer(process, generation, atEnd);
}

bool QSocMcpStdioTransport::drainNewlineBuffer(QProcess *process, quint64 generation, bool atEnd)
{
    while (true) {
        auto result
            = QSocMcpStdioInternal::takeLine(readBuffer_, scanOffset_, kMaximumMessageBytes, atEnd);
        if (result.status == QSocMcpStdioInternal::LineStatus::NeedMore) {
            return true;
        }
        if (result.status == QSocMcpStdioInternal::LineStatus::TooLarge) {
            failCurrent(
                process,
                generation,
                QStringLiteral("MCP server '%1' exceeded the 64 MiB message limit")
                    .arg(config_.name));
            return false;
        }
        if (result.status == QSocMcpStdioInternal::LineStatus::Unterminated) {
            failCurrent(
                process,
                generation,
                QStringLiteral("MCP server '%1' ended stdout with an unterminated JSON message")
                    .arg(config_.name));
            return false;
        }

        if (!deliverMessage(process, generation, result.message)) {
            return false;
        }
    }
}

bool QSocMcpStdioTransport::drainContentLengthBuffer(
    QProcess *process, quint64 generation, bool atEnd)
{
    while (true) {
        const qsizetype delimiter = readBuffer_.indexOf(kFrameDelimiter);
        if (delimiter < 0) {
            if (atEnd && !readBuffer_.isEmpty()) {
                failCurrent(
                    process,
                    generation,
                    QStringLiteral("MCP server '%1' ended stdout with an unterminated message")
                        .arg(config_.name));
                return false;
            }
            return true;
        }
        qint64           contentLength = -1;
        const QByteArray header        = readBuffer_.left(delimiter);
        for (QByteArray line : header.split('\n')) {
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (!line.startsWith(kContentLengthKey)) {
                continue;
            }
            bool parsed   = false;
            contentLength = line.mid(static_cast<qsizetype>(qstrlen(kContentLengthKey)))
                                .trimmed()
                                .toLongLong(&parsed);
            if (!parsed) {
                contentLength = -1;
            }
        }
        if (contentLength < 0) {
            failCurrent(
                process,
                generation,
                QStringLiteral("MCP server '%1' wrote invalid Content-Length framing")
                    .arg(config_.name));
            return false;
        }
        const qsizetype bodyStart = delimiter + kFrameDelimiterSize;
        if (readBuffer_.size() - bodyStart < contentLength) {
            if (atEnd) {
                failCurrent(
                    process,
                    generation,
                    QStringLiteral("MCP server '%1' ended stdout with an unterminated message")
                        .arg(config_.name));
                return false;
            }
            return true;
        }

        const QByteArray payload = readBuffer_.mid(bodyStart, static_cast<qsizetype>(contentLength));
        readBuffer_.remove(0, bodyStart + static_cast<qsizetype>(contentLength));
        if (!deliverMessage(process, generation, payload)) {
            return false;
        }
    }
}

bool QSocMcpStdioTransport::deliverMessage(
    QProcess *process, quint64 generation, const QByteArray &payload)
{
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(payload.toStdString());
    } catch (const std::exception &) {
        failCurrent(
            process,
            generation,
            QStringLiteral("MCP server '%1' wrote invalid JSON to stdout").arg(config_.name));
        return false;
    }

    const QPointer<QSocMcpStdioTransport> guard(this);
    emit                                  messageReceived(message);
    return !guard.isNull() && isCurrent(process, generation) && acceptMessages_;
}

void QSocMcpStdioTransport::failCurrent(QProcess *process, quint64 generation, const QString &message)
{
    if (!isCurrent(process, generation) || terminal_) {
        return;
    }
    if (!errorReported_) {
        errorReported_  = true;
        acceptMessages_ = false;
        clearInput();
        setState(State::Stopping);

        const QPointer<QSocMcpStdioTransport> guard(this);
        emit                                  errorOccurred(message);
        if (guard.isNull() || !isCurrent(process, generation)) {
            return;
        }
    }

    if (process->state() == QProcess::NotRunning) {
        const QPointer<QProcess> processGuard(process);
        QTimer::singleShot(0, this, [this, processGuard, generation]() {
            if (!processGuard.isNull() && isCurrent(processGuard.data(), generation)) {
                finishCurrent(processGuard.data(), generation);
            }
        });
        return;
    }
    requestShutdown(process, generation, false);
}

void QSocMcpStdioTransport::requestShutdown(QProcess *process, quint64 generation, bool graceful)
{
    if (!isCurrent(process, generation)) {
        return;
    }
    if (process->state() == QProcess::NotRunning) {
        finishCurrent(process, generation);
        return;
    }
    if (process->state() == QProcess::Running) {
        const QPointer<QSocMcpStdioTransport> guard(this);
        process->closeWriteChannel();
        if (guard.isNull() || !isCurrent(process, generation)) {
            return;
        }
    }

    QPointer<QProcess> processGuard(process);
    const int          terminateDelay = graceful ? kGracefulStopMs : 0;
    QTimer::singleShot(terminateDelay, this, [this, processGuard, generation]() {
        if (processGuard.isNull() || !isCurrent(processGuard.data(), generation)
            || processGuard->state() == QProcess::NotRunning) {
            return;
        }
        processGuard->terminate();
    });
    QTimer::singleShot(kForceStopMs, this, [this, processGuard, generation]() {
        if (processGuard.isNull() || !isCurrent(processGuard.data(), generation)
            || processGuard->state() == QProcess::NotRunning) {
            return;
        }
        processGuard->kill();
    });
}

void QSocMcpStdioTransport::finishCurrent(QProcess *process, quint64 generation)
{
    if (!isCurrent(process, generation) || terminal_) {
        return;
    }

    terminal_       = true;
    acceptMessages_ = false;
    clearInput();
    setState(State::Stopped);
    process_ = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    emit closed();
}

bool QSocMcpStdioTransport::isCurrent(QProcess *process, quint64 generation) const
{
    return process != nullptr && process_.data() == process && lifecycleGeneration_ == generation
           && !terminal_;
}

void QSocMcpStdioTransport::clearInput()
{
    readBuffer_.clear();
    scanOffset_ = 0;
}
