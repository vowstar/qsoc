// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpstdio.h"

#include "common/qsocconsole.h"

#include <QProcessEnvironment>

namespace {

constexpr auto kFrameDelimiter    = "\r\n\r\n";
constexpr int  kFrameDelimiterLen = 4;
constexpr auto kContentLengthKey  = "Content-Length:";

} // namespace

QSocMcpStdioTransport::QSocMcpStdioTransport(McpServerConfig config, QObject *parent)
    : QSocMcpTransport(parent)
    , config_(std::move(config))
{}

QSocMcpStdioTransport::~QSocMcpStdioTransport()
{
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(500);
    }
}

void QSocMcpStdioTransport::start()
{
    if (state() != State::Idle && state() != State::Stopped) {
        return;
    }
    setState(State::Starting);

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = config_.env.constBegin(); it != config_.env.constEnd(); ++it) {
        env.insert(it.key(), it.value());
    }
    process_->setProcessEnvironment(env);

    connect(process_, &QProcess::started, this, &QSocMcpStdioTransport::onProcessStarted);
    connect(process_, &QProcess::finished, this, &QSocMcpStdioTransport::onProcessFinished);
    connect(process_, &QProcess::errorOccurred, this, &QSocMcpStdioTransport::onProcessErrorOccurred);
    connect(
        process_,
        &QProcess::readyReadStandardOutput,
        this,
        &QSocMcpStdioTransport::onReadyReadStandardOutput);
    connect(
        process_,
        &QProcess::readyReadStandardError,
        this,
        &QSocMcpStdioTransport::onReadyReadStandardError);

    process_->start(config_.command, config_.args);
}

void QSocMcpStdioTransport::stop()
{
    if (process_ == nullptr) {
        setState(State::Stopped);
        return;
    }
    if (state() == State::Stopping || state() == State::Stopped) {
        return;
    }
    setState(State::Stopping);

    if (process_->state() == QProcess::NotRunning) {
        setState(State::Stopped);
        emit closed();
        return;
    }

    process_->closeWriteChannel();
    if (!process_->waitForFinished(200)) {
        process_->terminate();
        if (!process_->waitForFinished(400)) {
            process_->kill();
            process_->waitForFinished(200);
        }
    }
}

void QSocMcpStdioTransport::sendMessage(const nlohmann::json &message)
{
    if (process_ == nullptr || state() != State::Running) {
        emit errorOccurred(QStringLiteral("Transport not running"));
        return;
    }

    const std::string body    = message.dump();
    const QByteArray  frame   = QByteArray("Content-Length: ")
                                + QByteArray::number(static_cast<qsizetype>(body.size()))
                                + QByteArray("\r\n\r\n") + QByteArray::fromStdString(body);
    const qint64      written = process_->write(frame);
    if (written != frame.size()) {
        emit errorOccurred(QStringLiteral("Short write to MCP stdio transport"));
    }
}

void QSocMcpStdioTransport::onProcessStarted()
{
    setState(State::Running);
    emit started();
}

void QSocMcpStdioTransport::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(exitCode);
    Q_UNUSED(status);
    setState(State::Stopped);
    emit closed();
}

void QSocMcpStdioTransport::onProcessErrorOccurred(QProcess::ProcessError error)
{
    const QString message = process_ != nullptr ? process_->errorString()
                                                : QString::number(static_cast<int>(error));
    emit          errorOccurred(message);
    setState(State::Stopped);
    emit closed();
}

void QSocMcpStdioTransport::onReadyReadStandardOutput()
{
    if (process_ == nullptr) {
        return;
    }
    readBuffer_.append(process_->readAllStandardOutput());
    drainBuffer();
}

void QSocMcpStdioTransport::onReadyReadStandardError()
{
    if (process_ == nullptr) {
        return;
    }
    const QByteArray chunk = process_->readAllStandardError();
    if (!chunk.isEmpty()) {
        QSocConsole::debug() << "mcp stdio stderr:" << QString::fromUtf8(chunk).trimmed();
    }
}

void QSocMcpStdioTransport::drainBuffer()
{
    while (true) {
        const qsizetype delim = readBuffer_.indexOf(kFrameDelimiter);
        if (delim < 0) {
            return;
        }

        const QByteArray header     = readBuffer_.left(delim);
        qsizetype        contentLen = -1;
        for (const QByteArray &lineRaw : header.split('\n')) {
            QByteArray line = lineRaw;
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith(kContentLengthKey)) {
                bool       parsed = false;
                const auto value
                    = line.mid(static_cast<qsizetype>(qstrlen(kContentLengthKey))).trimmed();
                contentLen = static_cast<qsizetype>(value.toLongLong(&parsed));
                if (!parsed) {
                    contentLen = -1;
                }
            }
        }

        if (contentLen < 0) {
            emit errorOccurred(QStringLiteral("Missing or invalid Content-Length header"));
            stop();
            return;
        }

        const qsizetype frameEnd = delim + kFrameDelimiterLen + contentLen;
        if (readBuffer_.size() < frameEnd) {
            return;
        }

        const QByteArray body = readBuffer_.mid(delim + kFrameDelimiterLen, contentLen);
        readBuffer_.remove(0, frameEnd);

        try {
            auto msg = nlohmann::json::parse(body.toStdString());
            emit messageReceived(msg);
        } catch (const std::exception &e) {
            emit errorOccurred(QStringLiteral("JSON parse error: %1").arg(e.what()));
            stop();
            return;
        }
    }
}
