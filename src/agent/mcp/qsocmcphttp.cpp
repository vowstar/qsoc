// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcphttp.h"

#include "common/qsocconsole.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

constexpr auto kAcceptHeader   = "application/json, text/event-stream";
constexpr auto kSseContentType = "text/event-stream";

bool replyIsSse(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return false;
    }
    const QByteArray contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    return contentType.contains(kSseContentType);
}

} // namespace

QSocMcpHttpTransport::QSocMcpHttpTransport(McpServerConfig config, QObject *parent)
    : QSocMcpTransport(parent)
    , config_(std::move(config))
{}

QSocMcpHttpTransport::~QSocMcpHttpTransport()
{
    for (auto it = replies_.constBegin(); it != replies_.constEnd(); ++it) {
        QNetworkReply *reply = it.key();
        if (reply != nullptr) {
            reply->abort();
            reply->deleteLater();
        }
    }
    replies_.clear();
}

void QSocMcpHttpTransport::start()
{
    if (state() == State::Running) {
        return;
    }
    setState(State::Starting);
    if (manager_ == nullptr) {
        manager_ = new QNetworkAccessManager(this);
    }
    setState(State::Running);
    emit started();
}

void QSocMcpHttpTransport::stop()
{
    if (state() == State::Stopped) {
        return;
    }
    setState(State::Stopping);
    for (auto it = replies_.constBegin(); it != replies_.constEnd(); ++it) {
        QNetworkReply *reply = it.key();
        if (reply != nullptr) {
            reply->abort();
        }
    }
    replies_.clear();
    setState(State::Stopped);
    emit closed();
}

void QSocMcpHttpTransport::sendMessage(const nlohmann::json &message)
{
    if (state() != State::Running || manager_ == nullptr) {
        emit errorOccurred(QStringLiteral("HTTP transport not running"));
        return;
    }

    const QUrl url(config_.url);
    if (!url.isValid()) {
        emit errorOccurred(QStringLiteral("Invalid MCP URL: %1").arg(config_.url));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", kAcceptHeader);
    for (auto it = config_.headers.constBegin(); it != config_.headers.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    const QByteArray body = QByteArray::fromStdString(message.dump());

    QNetworkReply *reply = manager_->post(request, body);
    replies_.insert(reply, ReplyState{});

    connect(reply, &QNetworkReply::readyRead, this, &QSocMcpHttpTransport::onReplyReadyRead);
    connect(reply, &QNetworkReply::finished, this, &QSocMcpHttpTransport::onReplyFinished);
    connect(reply, &QNetworkReply::errorOccurred, this, &QSocMcpHttpTransport::onReplyError);
}

void QSocMcpHttpTransport::onReplyReadyRead()
{
    QNetworkReply *reply = senderReply();
    if (reply == nullptr || !replies_.contains(reply)) {
        return;
    }
    auto &meta = replies_[reply];
    if (!meta.isSse) {
        meta.isSse = replyIsSse(reply);
    }
    if (meta.isSse) {
        handleSseChunk(reply);
    }
    /* Non-SSE bodies are read in onReplyFinished where we have the
     * complete document. */
}

void QSocMcpHttpTransport::onReplyFinished()
{
    QNetworkReply *reply = senderReply();
    if (reply == nullptr || !replies_.contains(reply)) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(reply->errorString());
        cleanupReply(reply);
        return;
    }

    auto &meta = replies_[reply];
    if (meta.isSse) {
        /* Drain any remaining buffered SSE bytes. */
        handleSseChunk(reply);
    } else {
        handleFinalJson(reply);
    }
    cleanupReply(reply);
}

void QSocMcpHttpTransport::onReplyError()
{
    QNetworkReply *reply = senderReply();
    if (reply == nullptr) {
        return;
    }
    emit errorOccurred(reply->errorString());
}

void QSocMcpHttpTransport::handleSseChunk(QNetworkReply *reply)
{
    auto &meta = replies_[reply];
    meta.sseBuffer += reply->readAll();

    while (true) {
        const qsizetype eventEnd = meta.sseBuffer.indexOf("\n\n");
        if (eventEnd < 0) {
            return;
        }
        const QByteArray eventText = meta.sseBuffer.left(eventEnd);
        meta.sseBuffer.remove(0, eventEnd + 2);

        QByteArray dataPayload;
        for (const QByteArray &lineRaw : eventText.split('\n')) {
            QByteArray line = lineRaw;
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith("data:")) {
                QByteArray value = line.mid(5);
                if (value.startsWith(' ')) {
                    value = value.mid(1);
                }
                if (!dataPayload.isEmpty()) {
                    dataPayload.append('\n');
                }
                dataPayload.append(value);
            }
        }
        if (dataPayload.isEmpty()) {
            continue;
        }
        try {
            auto msg = nlohmann::json::parse(dataPayload.toStdString());
            emit messageReceived(msg);
        } catch (const std::exception &e) {
            emit errorOccurred(QStringLiteral("SSE JSON parse error: %1").arg(e.what()));
        }
    }
}

void QSocMcpHttpTransport::handleFinalJson(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll();
    if (body.trimmed().isEmpty()) {
        return; /* Empty 200 is valid for one-way notifications. */
    }
    try {
        auto msg = nlohmann::json::parse(body.toStdString());
        emit messageReceived(msg);
    } catch (const std::exception &e) {
        emit errorOccurred(QStringLiteral("JSON parse error: %1").arg(e.what()));
    }
}

void QSocMcpHttpTransport::cleanupReply(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return;
    }
    replies_.remove(reply);
    reply->deleteLater();
}

QNetworkReply *QSocMcpHttpTransport::senderReply()
{
    return qobject_cast<QNetworkReply *>(sender());
}
