// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcphttp.h"

#include "common/qsocconsole.h"
#include "common/qsocproxy.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace {

constexpr auto kAcceptHeader       = "application/json, text/event-stream";
constexpr auto kSseContentType     = "text/event-stream";
constexpr auto kMcpSessionIdHeader = "Mcp-Session-Id";

struct DeferredSignal
{
    nlohmann::json message;
    QString        error;
};

bool replyIsSse(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return false;
    }
    const QByteArray contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    return contentType.contains(kSseContentType);
}

QList<DeferredSignal> parseSseEvents(QByteArray &buffer)
{
    QList<DeferredSignal> events;

    /* SSE delimits events with any of \r\r, \n\n, or \r\n\r\n. Pick the
     * earliest occurrence so we honour whichever the server emitted. */
    while (true) {
        qsizetype  eventEnd   = -1;
        qsizetype  delimLen   = 0;
        const auto candidates = {
            std::make_pair(QByteArrayLiteral("\r\n\r\n"), qsizetype(4)),
            std::make_pair(QByteArrayLiteral("\n\n"), qsizetype(2)),
            std::make_pair(QByteArrayLiteral("\r\r"), qsizetype(2)),
        };
        for (const auto &[delim, len] : candidates) {
            const qsizetype idx = buffer.indexOf(delim);
            if (idx >= 0 && (eventEnd < 0 || idx < eventEnd)) {
                eventEnd = idx;
                delimLen = len;
            }
        }
        if (eventEnd < 0) {
            return events;
        }
        const QByteArray eventText = buffer.left(eventEnd);
        buffer.remove(0, eventEnd + delimLen);

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
            events.append(DeferredSignal{nlohmann::json::parse(dataPayload.toStdString()), {}});
        } catch (const std::exception &e) {
            events.append(
                DeferredSignal{
                    {},
                    QStringLiteral("SSE JSON parse error: %1").arg(QString::fromUtf8(e.what()))});
        }
    }
}

QList<DeferredSignal> parseJsonBody(const QByteArray &body)
{
    if (body.trimmed().isEmpty()) {
        return {}; /* Empty 200 is valid for one-way notifications. */
    }
    try {
        return QList<DeferredSignal>{DeferredSignal{nlohmann::json::parse(body.toStdString()), {}}};
    } catch (const std::exception &e) {
        return QList<DeferredSignal>{DeferredSignal{
            {}, QStringLiteral("JSON parse error: %1").arg(QString::fromUtf8(e.what()))}};
    }
}

} // namespace

QSocMcpHttpTransport::QSocMcpHttpTransport(McpServerConfig config, QObject *parent)
    : QSocMcpTransport(parent)
    , config_(std::move(config))
{}

QSocMcpHttpTransport::~QSocMcpHttpTransport()
{
    clearReplies();
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
    QSocProxy::apply(manager_, QSocProxy::resolve(config_.proxy, QSocProxy::qsocWideDefault()));
    setState(State::Running);
    emit started();
}

void QSocMcpHttpTransport::stop()
{
    if (state() == State::Stopped) {
        return;
    }
    setState(State::Stopping);
    clearReplies();
    setState(State::Stopped);
    emit closed();
}

void QSocMcpHttpTransport::sendMessage(const nlohmann::json &message)
{
    postMessage(message, 0);
}

void QSocMcpHttpTransport::sendTrackedMessage(const nlohmann::json &message, quint64 token)
{
    postMessage(message, token);
}

void QSocMcpHttpTransport::postMessage(const nlohmann::json &message, quint64 sendToken)
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
    /* Streamable HTTP servers issue a session id via Mcp-Session-Id on
     * the initialize response and require it back on every subsequent
     * request. Without it the server answers HTTP 400 / 404. */
    if (!sessionId_.isEmpty()) {
        request.setRawHeader(kMcpSessionIdHeader, sessionId_);
    }
    for (auto it = config_.headers.constBegin(); it != config_.headers.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    const QByteArray body = QByteArray::fromStdString(message.dump());

    QNetworkReply *reply = manager_->post(request, body);
    ReplyState     replyState;
    replyState.sendToken = sendToken;
    replies_.insert(reply, replyState);

    connect(
        reply, &QNetworkReply::metaDataChanged, this, &QSocMcpHttpTransport::onReplyMetaDataChanged);
    connect(reply, &QNetworkReply::readyRead, this, &QSocMcpHttpTransport::onReplyReadyRead);
    connect(reply, &QNetworkReply::finished, this, &QSocMcpHttpTransport::onReplyFinished);
}

void QSocMcpHttpTransport::onReplyMetaDataChanged()
{
    QNetworkReply *reply = senderReply();
    if (reply == nullptr) {
        return;
    }
    /* Capture Mcp-Session-Id as soon as headers are parsed, before any
     * SSE body triggers a downstream POST. The body handler later on
     * may immediately send notifications/initialized + tools/list, and
     * those follow-ups must already carry the session id. */
    const QByteArray sessionId = reply->rawHeader(kMcpSessionIdHeader);
    if (!sessionId.isEmpty()) {
        sessionId_ = sessionId;
    }
}

void QSocMcpHttpTransport::onReplyReadyRead()
{
    QNetworkReply *reply = senderReply();
    if (reply == nullptr || !replies_.contains(reply)) {
        return;
    }
    bool isSse = false;
    {
        auto it = replies_.find(reply);
        if (!it->isSse) {
            it->isSse = replyIsSse(reply);
        }
        isSse = it->isSse;
    }
    if (isSse) {
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
        const QString error = reply->errorString();
        cleanupReply(reply);
        emit errorOccurred(error);
        return;
    }

    /* Late-arriving session id: if headers were not parsed before the
     * body finished (rare, but possible with very small responses),
     * pick it up now. The early metaDataChanged hook handles the
     * common case. */
    const QByteArray sessionId = reply->rawHeader(kMcpSessionIdHeader);
    if (!sessionId.isEmpty()) {
        sessionId_ = sessionId;
    }

    QList<DeferredSignal> events;
    quint64               sendToken  = 0;
    bool                  sendFailed = false;
    {
        auto it = replies_.find(reply);
        if (!it->isSse) {
            it->isSse = replyIsSse(reply);
        }
        if (it->isSse) {
            it->sseBuffer += reply->readAll();
            events = parseSseEvents(it->sseBuffer);
        } else {
            events = parseJsonBody(reply->readAll());
        }
        for (const auto &event : std::as_const(events)) {
            if (!event.error.isEmpty()) {
                it->sendFailed = true;
                break;
            }
        }
        sendToken  = it->sendToken;
        sendFailed = it->sendFailed;
    }
    cleanupReply(reply);

    const quint64                  generation = lifecycleGeneration_;
    QPointer<QSocMcpHttpTransport> guard(this);
    for (const auto &event : std::as_const(events)) {
        if (event.error.isEmpty()) {
            emit messageReceived(event.message);
        } else {
            emit errorOccurred(event.error);
        }
        if (guard.isNull() || lifecycleGeneration_ != generation) {
            return;
        }
    }
    if (sendToken != 0 && !sendFailed) {
        emit messageSent(sendToken);
    }
}

void QSocMcpHttpTransport::handleSseChunk(QNetworkReply *reply)
{
    QList<DeferredSignal> events;
    {
        auto it = replies_.find(reply);
        if (it == replies_.end()) {
            return;
        }
        it->sseBuffer += reply->readAll();
        events = parseSseEvents(it->sseBuffer);
        for (const auto &event : std::as_const(events)) {
            if (!event.error.isEmpty()) {
                it->sendFailed = true;
                break;
            }
        }
    }

    const quint64                  generation = lifecycleGeneration_;
    QPointer<QSocMcpHttpTransport> guard(this);
    for (const auto &event : std::as_const(events)) {
        if (event.error.isEmpty()) {
            emit messageReceived(event.message);
        } else {
            emit errorOccurred(event.error);
        }
        if (guard.isNull() || lifecycleGeneration_ != generation) {
            return;
        }
    }
}

void QSocMcpHttpTransport::clearReplies()
{
    lifecycleGeneration_++;
    sessionId_.clear();
    const QList<QNetworkReply *> replies = replies_.keys();
    replies_.clear();
    for (QNetworkReply *reply : replies) {
        if (reply == nullptr) {
            continue;
        }
        disconnect(reply, nullptr, this, nullptr);
        if (reply->isRunning()) {
            reply->abort();
        }
        reply->deleteLater();
    }
}

void QSocMcpHttpTransport::cleanupReply(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return;
    }
    replies_.remove(reply);
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
}

QNetworkReply *QSocMcpHttpTransport::senderReply()
{
    return qobject_cast<QNetworkReply *>(sender());
}
