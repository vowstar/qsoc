// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPHTTP_H
#define QSOCMCPHTTP_H

#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief Streamable HTTP transport for an MCP server.
 * @details Each outbound message is POSTed to the server URL. The
 *          response is either an `application/json` document with the
 *          single matching response, or an `text/event-stream` of SSE
 *          events (each event payload is one JSON-RPC message). Both
 *          shapes are supported on the same endpoint per the MCP
 *          Streamable HTTP transport spec.
 */
class QSocMcpHttpTransport : public QSocMcpTransport
{
    Q_OBJECT

public:
    explicit QSocMcpHttpTransport(McpServerConfig config, QObject *parent = nullptr);
    ~QSocMcpHttpTransport() override;

    void start() override;
    void stop() override;
    void sendMessage(const nlohmann::json &message) override;

private slots:
    void onReplyMetaDataChanged();
    void onReplyReadyRead();
    void onReplyFinished();
    void onReplyError();

private:
    struct ReplyState
    {
        QByteArray sseBuffer;
        bool       isSse = false;
    };

    void           handleSseChunk(QNetworkReply *reply);
    void           handleFinalJson(QNetworkReply *reply);
    void           cleanupReply(QNetworkReply *reply);
    QNetworkReply *senderReply();

    McpServerConfig                    config_;
    QNetworkAccessManager             *manager_ = nullptr;
    QHash<QNetworkReply *, ReplyState> replies_;
    QByteArray                         sessionId_;
};

#endif // QSOCMCPHTTP_H
