// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPSTDIO_H
#define QSOCMCPSTDIO_H

#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"

#include <QByteArray>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * @brief Stdio transport for an MCP server child process.
 * @details Spawns the server as a QProcess and exchanges UTF-8 JSON-RPC
 *          messages over stdin/stdout with the configured framing.
 *          Stderr is captured but not parsed; it is logged for debugging.
 */
class QSocMcpStdioTransport : public QSocMcpTransport
{
    Q_OBJECT

public:
    explicit QSocMcpStdioTransport(McpServerConfig config, QObject *parent = nullptr);
    ~QSocMcpStdioTransport() override;

    void start() override;
    void stop() override;
    void sendMessage(const nlohmann::json &message) override;

private:
    void createProcess();
    void handleProcessStarted(QProcess *process, quint64 generation);
    void handleProcessFinished(
        QProcess *process, quint64 generation, int exitCode, QProcess::ExitStatus status);
    void handleProcessError(QProcess *process, quint64 generation, QProcess::ProcessError error);
    void handleStandardOutput(QProcess *process, quint64 generation, bool atEnd = false);
    void handleStandardError(QProcess *process, quint64 generation);
    bool drainBuffer(QProcess *process, quint64 generation, bool atEnd);
    bool drainNewlineBuffer(QProcess *process, quint64 generation, bool atEnd);
    bool drainContentLengthBuffer(QProcess *process, quint64 generation, bool atEnd);
    bool deliverMessage(QProcess *process, quint64 generation, const QByteArray &payload);
    void failCurrent(QProcess *process, quint64 generation, const QString &message);
    void requestShutdown(QProcess *process, quint64 generation, bool graceful);
    void finishCurrent(QProcess *process, quint64 generation);
    bool isCurrent(QProcess *process, quint64 generation) const;
    void clearInput();

    McpServerConfig    config_;
    QPointer<QProcess> process_;
    QByteArray         readBuffer_;
    qsizetype          scanOffset_          = 0;
    quint64            lifecycleGeneration_ = 0;
    bool               terminal_            = true;
    bool               acceptMessages_      = false;
    bool               stopRequested_       = false;
    bool               errorReported_       = false;
};

#endif // QSOCMCPSTDIO_H
