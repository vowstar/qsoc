// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspprocessbackend.h"
#include "common/qsocconsole.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

QLspProcessBackend::QLspProcessBackend(
    const QString &command, const QStringList &args, const QStringList &exts, QObject *parent)
    : QLspBackend(parent)
    , command(command)
    , args(args)
    , exts(exts)
{}

QLspProcessBackend::~QLspProcessBackend()
{
    stop();
}

bool QLspProcessBackend::start(const QString &workspaceFolder)
{
    if (process)
        return initialized;

    process = new QProcess(this);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, &QLspProcessBackend::onReadyRead);

    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        Q_UNUSED(err)
        initialized = false;
    });

    connect(
        process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this](int exitCode, QProcess::ExitStatus status) {
            Q_UNUSED(exitCode)
            Q_UNUSED(status)
            initialized = false;
            /* Unblock any callers stuck on loop.exec() so they don't wait
               for the request timeout when the server has already exited. */
            for (auto iter = pendingRequests.begin(); iter != pendingRequests.end(); ++iter) {
                if (iter->loop)
                    iter->loop->quit();
            }
        });

    process->start(command, args);
    if (!process->waitForStarted(5000)) {
        delete process;
        process = nullptr;
        return false;
    }

    return sendInitialize(workspaceFolder);
}

void QLspProcessBackend::stop()
{
    if (!process)
        return;

    if (initialized) {
        /* Send LSP shutdown request and exit notification. */
        QJsonObject shutdownMsg{
            {"jsonrpc", "2.0"},
            {"id", nextRequestId++},
            {"method", "shutdown"},
        };
        sendJsonRpc(shutdownMsg);
        process->waitForReadyRead(1000);

        QJsonObject exitMsg{
            {"jsonrpc", "2.0"},
            {"method", "exit"},
        };
        sendJsonRpc(exitMsg);
    }

    initialized = false;

    /* Unblock any pending synchronous requests before killing the process.
       Without this, request() callers stay stuck in loop.exec() forever. */
    for (auto iter = pendingRequests.begin(); iter != pendingRequests.end(); ++iter) {
        iter->done = false;
        if (iter->loop)
            iter->loop->quit();
    }
    pendingRequests.clear();

    process->waitForFinished(3000);
    if (process->state() != QProcess::NotRunning)
        process->kill();

    delete process;
    process = nullptr;
    readBuffer.clear();
    expectedContentLength = -1;
    serverCapabilities    = {};
}

bool QLspProcessBackend::isReady() const
{
    return initialized && process && process->state() == QProcess::Running;
}

QStringList QLspProcessBackend::extensions() const
{
    return exts;
}

QJsonObject QLspProcessBackend::capabilities() const
{
    return serverCapabilities;
}

QJsonValue QLspProcessBackend::request(const QString &method, const QJsonObject &params)
{
    if (!isReady())
        return QJsonValue();

    int         numericId = nextRequestId++;
    QString     requestId = QString::number(numericId);
    QJsonObject message{
        {"jsonrpc", "2.0"},
        {"id", numericId},
        {"method", method},
        {"params", params},
    };
    sendJsonRpc(message);

    /* Block until the response arrives, with a 30-second timeout. */
    QJsonValue result;
    QEventLoop loop;
    pendingRequests[requestId] = PendingRequest{&loop, &result, false};

    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();

    bool completed = pendingRequests[requestId].done;
    pendingRequests.remove(requestId);

    return completed ? result : QJsonValue();
}

void QLspProcessBackend::notify(const QString &method, const QJsonObject &params)
{
    if (!isReady())
        return;

    QJsonObject message{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
    };
    sendJsonRpc(message);
}

/* JSON-RPC framing: Content-Length header + body */

void QLspProcessBackend::sendJsonRpc(const QJsonObject &message)
{
    QByteArray body   = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray header = QStringLiteral("Content-Length: %1\r\n\r\n").arg(body.size()).toUtf8();
    process->write(header + body);
    process->waitForBytesWritten(1000);
}

void QLspProcessBackend::onReadyRead()
{
    readBuffer.append(process->readAllStandardOutput());
    while (tryParseMessage()) {
        /* Keep parsing while complete messages are available. */
    }
}

bool QLspProcessBackend::tryParseMessage()
{
    /* Parse Content-Length header if not yet known. */
    if (expectedContentLength < 0) {
        int headerEnd = readBuffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return false;

        /* Extract Content-Length value from header block. */
        QByteArray headerBlock = readBuffer.left(headerEnd);
        int        clIndex     = headerBlock.indexOf("Content-Length:");
        if (clIndex < 0) {
            /* Malformed header, skip past it. */
            readBuffer.remove(0, headerEnd + 4);
            return true;
        }

        int valueStart = clIndex + 15; /* length of "Content-Length:" */
        int lineEnd    = headerBlock.indexOf('\r', valueStart);
        if (lineEnd < 0)
            lineEnd = headerBlock.size();

        bool parseOk = false;
        expectedContentLength
            = headerBlock.mid(valueStart, lineEnd - valueStart).trimmed().toInt(&parseOk);
        if (!parseOk || expectedContentLength <= 0) {
            expectedContentLength = -1;
            readBuffer.remove(0, headerEnd + 4);
            return true;
        }

        /* Remove the header from the buffer. */
        readBuffer.remove(0, headerEnd + 4);
    }

    /* Wait for the full body. */
    if (readBuffer.size() < expectedContentLength)
        return false;

    QByteArray body = readBuffer.left(expectedContentLength);
    readBuffer.remove(0, expectedContentLength);
    expectedContentLength = -1;

    QJsonParseError parseError;
    QJsonDocument   doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return true;

    handleMessage(doc.object());
    return true;
}

void QLspProcessBackend::handleMessage(const QJsonObject &message)
{
    if (message.contains("id") && !message.contains("method")) {
        /* Response to one of our requests. Per JSON-RPC, id may be integer
           or string; stringify uniformly to match pendingRequests. */
        QString responseId = message["id"].toVariant().toString();
        if (pendingRequests.contains(responseId)) {
            PendingRequest &pending = pendingRequests[responseId];
            if (message.contains("result")) {
                *pending.result = message["result"];
            } else if (message.contains("error")) {
                /* Surface the error in logs so failures aren't silent. */
                QJsonObject err = message["error"].toObject();
                QSocConsole::warn()
                    << "LSP error response:" << err["code"].toInt() << err["message"].toString();
            }
            pending.done = true;
            if (pending.loop)
                pending.loop->quit();
        }
    } else if (message.contains("method") && !message.contains("id")) {
        /* Server-to-client notification. */
        emit notification(message["method"].toString(), message["params"].toObject());
    } else if (message.contains("method") && message.contains("id")) {
        /* Server-to-client request. We handle workspace/configuration
           (return empty config); reject everything else per LSP spec. */
        QString method = message["method"].toString();
        if (method == "workspace/configuration") {
            /* Return empty config for each requested item. */
            QJsonArray items = message["params"].toObject()["items"].toArray();
            QJsonArray results;
            for (int idx = 0; idx < items.size(); ++idx)
                results.append(QJsonValue());

            sendJsonRpc(
                QJsonObject{
                    {"jsonrpc", "2.0"},
                    {"id", message["id"]},
                    {"result", results},
                });
        } else {
            /* Method not supported. */
            sendJsonRpc(
                QJsonObject{
                    {"jsonrpc", "2.0"},
                    {"id", message["id"]},
                    {"error",
                     QJsonObject{
                         {"code", -32601},
                         {"message", "Method not found"},
                     }},
                });
        }
    }
}

bool QLspProcessBackend::sendInitialize(const QString &workspaceFolder)
{
    int     numericId    = nextRequestId++;
    QString requestId    = QString::number(numericId);
    QString workspaceUri = QUrl::fromLocalFile(workspaceFolder).toString();

    QJsonObject initParams{
        {"processId", static_cast<int>(QCoreApplication::applicationPid())},
        {"rootUri", workspaceUri},
        {"rootPath", workspaceFolder},
        {"workspaceFolders",
         QJsonArray{
             QJsonObject{
                 {"uri", workspaceUri},
                 {"name", QDir(workspaceFolder).dirName()},
             },
         }},
        {"capabilities",
         QJsonObject{
             {"textDocument",
              QJsonObject{
                  {"synchronization",
                   QJsonObject{
                       {"dynamicRegistration", false},
                       {"didSave", true},
                   }},
                  {"publishDiagnostics",
                   QJsonObject{
                       {"relatedInformation", true},
                   }},
                  {"hover",
                   QJsonObject{
                       {"dynamicRegistration", false},
                       {"contentFormat", QJsonArray{"markdown", "plaintext"}},
                   }},
                  {"definition",
                   QJsonObject{
                       {"dynamicRegistration", false},
                   }},
                  {"references",
                   QJsonObject{
                       {"dynamicRegistration", false},
                   }},
                  {"documentSymbol",
                   QJsonObject{
                       {"dynamicRegistration", false},
                       {"hierarchicalDocumentSymbolSupport", true},
                   }},
              }},
             {"workspace",
              QJsonObject{
                  {"configuration", false},
                  {"workspaceFolders", false},
              }},
         }},
        {"initializationOptions", QJsonObject{}},
    };

    QJsonObject message{
        {"jsonrpc", "2.0"},
        {"id", numericId},
        {"method", "initialize"},
        {"params", initParams},
    };
    sendJsonRpc(message);

    /* Wait for initialize response with a timeout. */
    QJsonValue result;
    QEventLoop loop;
    pendingRequests[requestId] = PendingRequest{&loop, &result, false};

    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();

    bool success = pendingRequests[requestId].done;
    pendingRequests.remove(requestId);

    if (!success)
        return false;

    /* Cache server capabilities for later capability gating. */
    if (result.isObject()) {
        serverCapabilities = result.toObject()["capabilities"].toObject();
    }

    /* Send initialized notification. */
    QJsonObject initializedMsg{
        {"jsonrpc", "2.0"},
        {"method", "initialized"},
        {"params", QJsonObject{}},
    };
    sendJsonRpc(initializedMsg);

    initialized = true;
    return true;
}
