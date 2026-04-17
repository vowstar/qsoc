// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLSPPROCESSBACKEND_H
#define QLSPPROCESSBACKEND_H

#include "common/qlspbackend.h"

#include <QByteArray>
#include <QMap>
#include <QProcess>

class QEventLoop;

/**
 * @brief External LSP server backend via JSON-RPC over stdio.
 * @details Spawns an LSP server process and communicates using the standard
 *          LSP protocol (JSON-RPC 2.0 with Content-Length framing). Works
 *          with any compliant server such as slang-server, vhdl_ls, or verible.
 */
class QLspProcessBackend : public QLspBackend
{
    Q_OBJECT

public:
    /**
     * @brief Construct an external LSP backend.
     * @param command Server executable, e.g. "slang-server" or "vhdl_ls".
     * @param args Command-line arguments, e.g. {"--stdio"}.
     * @param exts File extensions this server handles.
     * @param parent Parent QObject.
     */
    QLspProcessBackend(
        const QString     &command,
        const QStringList &args,
        const QStringList &exts,
        QObject           *parent = nullptr);
    ~QLspProcessBackend() override;

    bool        start(const QString &workspaceFolder) override;
    void        stop() override;
    bool        isReady() const override;
    QStringList extensions() const override;
    QJsonObject capabilities() const override;
    QJsonValue  request(const QString &method, const QJsonObject &params) override;
    void        notify(const QString &method, const QJsonObject &params) override;

private:
    QString     command;
    QStringList args;
    QStringList exts;

    QProcess   *process       = nullptr;
    bool        initialized   = false;
    int         nextRequestId = 1;
    QJsonObject serverCapabilities;

    /* Pending synchronous requests waiting for a response. Keyed by
       stringified id to handle servers that use string ids (per JSON-RPC
       spec, id may be integer or string). */
    struct PendingRequest
    {
        QEventLoop *loop   = nullptr;
        QJsonValue *result = nullptr;
        bool        done   = false;
    };
    QMap<QString, PendingRequest> pendingRequests;

    /* JSON-RPC message framing */
    QByteArray readBuffer;
    int        expectedContentLength = -1;

    void sendJsonRpc(const QJsonObject &message);
    void onReadyRead();
    bool tryParseMessage();
    void handleMessage(const QJsonObject &message);

    /* LSP initialize handshake */
    bool sendInitialize(const QString &workspaceFolder);
};

#endif // QLSPPROCESSBACKEND_H
