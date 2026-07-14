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
 *          successful response may be empty when the POST contains no
 *          JSON-RPC request. Otherwise it is an `application/json`
 *          document or `text/event-stream`. Each JSON document or SSE
 *          data event carries one JSON-RPC message or batch.
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
    void sendTrackedMessage(const nlohmann::json &message, quint64 token) override;
    void abandonTrackedMessage(quint64 token) override;
    void abandonRequest(int requestId) override;

private slots:
    void onReplyMetaDataChanged();
    void onReplyReadyRead();
    void onReplyFinished();

private:
    struct ReplyState
    {
        QByteArray sseBuffer;
        QList<int> pendingRequestIds;
        bool       acceptsSessionId = false;
        bool       isSse            = false;
        bool       requestBearing   = false;
        bool       sessionBound     = false;
        quint64    sendToken        = 0;
    };

    void           postMessage(const nlohmann::json &message, quint64 sendToken);
    void           processAvailableReplyData(QNetworkReply *reply);
    bool           captureSessionId(QNetworkReply *reply);
    bool           closeExpiredSession(QNetworkReply *reply);
    void           handleSseChunk(QNetworkReply *reply);
    void           failReply(QNetworkReply *reply, const QString &message);
    void           clearReplies();
    void           cleanupReply(QNetworkReply *reply, bool abort);
    QNetworkReply *senderReply();

    McpServerConfig                    config_;
    QNetworkAccessManager             *manager_ = nullptr;
    QHash<QNetworkReply *, ReplyState> replies_;
    QByteArray                         sessionId_;
    quint64                            lifecycleGeneration_ = 0;
};

#endif // QSOCMCPHTTP_H
