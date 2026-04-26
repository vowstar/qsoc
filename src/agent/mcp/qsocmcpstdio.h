// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPSTDIO_H
#define QSOCMCPSTDIO_H

#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * @brief Stdio transport for an MCP server child process.
 * @details Spawns the server as a QProcess and exchanges JSON-RPC
 *          messages over stdin/stdout using LSP-style framing:
 *          `Content-Length: <N>\r\n\r\n<body>`. Stderr is captured
 *          but not parsed; it is logged for debugging.
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

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessErrorOccurred(QProcess::ProcessError error);
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();

private:
    void drainBuffer();

    McpServerConfig config_;
    QProcess       *process_ = nullptr;
    QByteArray      readBuffer_;
};

#endif // QSOCMCPSTDIO_H
