// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcphttp.h"

#include "common/qsocconsole.h"
#include "common/qsocproxy.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <limits>
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

int replyHttpStatus(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return 0;
    }
    bool      ok     = false;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(&ok);
    return ok ? status : 0;
}

bool httpStatusIsSuccessful(int status)
{
    return status >= 200 && status < 300;
}

bool isTransportOwnedHeader(const QByteArray &name)
{
    return name.compare("Accept", Qt::CaseInsensitive) == 0
           || name.compare("Content-Type", Qt::CaseInsensitive) == 0
           || name.compare(kMcpSessionIdHeader, Qt::CaseInsensitive) == 0;
}

QString httpFailureMessage(QNetworkReply *reply, int status)
{
    QString reason
        = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString().trimmed();
    if (reason.isEmpty()) {
        reason = QStringLiteral("Request failed");
    }
    return QStringLiteral("[HTTP %1] %2").arg(status).arg(reason);
}

bool isInitializeRequest(const nlohmann::json &message)
{
    if (!message.is_object() || !message.contains("jsonrpc") || !message["jsonrpc"].is_string()
        || message["jsonrpc"] != "2.0" || !message.contains("method")
        || !message["method"].is_string() || message["method"] != "initialize"
        || !message.contains("id")) {
        return false;
    }
    const auto &id = message["id"];
    return id.is_string() || id.is_number();
}

bool isValidSessionId(const QByteArray &sessionId)
{
    if (sessionId.isEmpty()) {
        return false;
    }
    for (char character : sessionId) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21 || byte > 0x7e) {
            return false;
        }
    }
    return true;
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

bool readClientRequestId(const nlohmann::json &message, int *requestId)
{
    if (!message.is_object() || !message.contains("method") || !message["method"].is_string()
        || !message.contains("id")) {
        return false;
    }

    const auto &id = message["id"];
    if (id.is_number_unsigned()) {
        const auto value = id.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        *requestId = static_cast<int>(value);
        return true;
    }
    if (!id.is_number_integer()) {
        return false;
    }
    const auto value = id.get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return false;
    }
    *requestId = static_cast<int>(value);
    return true;
}

QList<int> requestIdsForMessage(const nlohmann::json &message)
{
    QList<int> requestIds;
    const auto appendRequestId = [&requestIds](const nlohmann::json &entry) {
        int requestId = -1;
        if (readClientRequestId(entry, &requestId) && !requestIds.contains(requestId)) {
            requestIds.append(requestId);
        }
    };

    if (message.is_array()) {
        for (const auto &entry : message) {
            appendRequestId(entry);
        }
    } else {
        appendRequestId(message);
    }
    return requestIds;
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
    if (state() != State::Idle && state() != State::Stopped) {
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
    const QList<int> requestIds = requestIdsForMessage(message);
    if (state() != State::Running || manager_ == nullptr) {
        emit errorOccurred(QStringLiteral("HTTP transport not running"));
        return;
    }

    const QUrl url(config_.url);
    if (!url.isValid()) {
        emit messageFailed(
            sendToken, requestIds, QStringLiteral("Invalid MCP URL: %1").arg(config_.url));
        return;
    }

    QNetworkRequest request(url);
    for (auto it = config_.headers.constBegin(); it != config_.headers.constEnd(); ++it) {
        const QByteArray name = it.key().toUtf8();
        if (!isTransportOwnedHeader(name)) {
            request.setRawHeader(name, it.value().toUtf8());
        }
    }
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", kAcceptHeader);
    /* Streamable HTTP servers issue a session id via Mcp-Session-Id on
     * the initialize response and require it back on every subsequent
     * request. Without it the server answers HTTP 400 / 404. */
    if (!sessionId_.isEmpty()) {
        request.setRawHeader(kMcpSessionIdHeader, sessionId_);
    }

    const QByteArray body = QByteArray::fromStdString(message.dump());

    QNetworkReply *reply = manager_->post(request, body);
    if (reply == nullptr) {
        emit messageFailed(sendToken, requestIds, QStringLiteral("HTTP request could not start"));
        return;
    }
    ReplyState replyState;
    replyState.requestIds       = requestIds;
    replyState.acceptsSessionId = isInitializeRequest(message);
    replyState.sessionBound     = !request.rawHeader(kMcpSessionIdHeader).isEmpty();
    replyState.sendToken        = sendToken;
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
    if (closeExpiredSession(reply)) {
        return;
    }
    if (!captureSessionId(reply)) {
        return;
    }
    if (reply->bytesAvailable() > 0) {
        processAvailableReplyData(reply);
    }
}

void QSocMcpHttpTransport::onReplyReadyRead()
{
    processAvailableReplyData(senderReply());
}

void QSocMcpHttpTransport::processAvailableReplyData(QNetworkReply *reply)
{
    if (reply == nullptr || !replies_.contains(reply)) {
        return;
    }
    const int status = replyHttpStatus(reply);
    if (status == 0) {
        return;
    }
    if (closeExpiredSession(reply)) {
        return;
    }
    if (!httpStatusIsSuccessful(status)) {
        reply->readAll();
        return;
    }
    if (!captureSessionId(reply)) {
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

    const int status = replyHttpStatus(reply);
    if (closeExpiredSession(reply)) {
        return;
    }
    if (status != 0 && !httpStatusIsSuccessful(status)) {
        failReply(reply, httpFailureMessage(reply, status));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString error = reply->errorString();
        failReply(reply, error);
        return;
    }
    if (status == 0) {
        failReply(reply, QStringLiteral("[HTTP 0] Response status unavailable"));
        return;
    }

    if (!captureSessionId(reply)) {
        return;
    }

    QList<DeferredSignal> events;
    QList<int>            requestIds;
    quint64               sendToken = 0;
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
        requestIds = it->requestIds;
        sendToken  = it->sendToken;
    }
    cleanupReply(reply, false);

    const quint64                  generation = lifecycleGeneration_;
    QPointer<QSocMcpHttpTransport> guard(this);
    for (const auto &event : std::as_const(events)) {
        if (event.error.isEmpty()) {
            emit messageReceived(event.message);
        } else {
            emit messageFailed(sendToken, requestIds, event.error);
            return;
        }
        if (guard.isNull() || lifecycleGeneration_ != generation) {
            return;
        }
    }
    if (sendToken != 0) {
        emit messageSent(sendToken);
    }
}

bool QSocMcpHttpTransport::captureSessionId(QNetworkReply *reply)
{
    auto it = replies_.find(reply);
    if (it == replies_.end() || !it->acceptsSessionId
        || !httpStatusIsSuccessful(replyHttpStatus(reply))) {
        return true;
    }
    if (!reply->hasRawHeader(kMcpSessionIdHeader)) {
        return true;
    }
    const QByteArray sessionId = reply->rawHeader(kMcpSessionIdHeader);
    if (!isValidSessionId(sessionId)) {
        failReply(reply, QStringLiteral("Invalid MCP session id"));
        return false;
    }
    if (sessionId_.isEmpty()) {
        sessionId_ = sessionId;
    }
    return true;
}

bool QSocMcpHttpTransport::closeExpiredSession(QNetworkReply *reply)
{
    auto it = replies_.find(reply);
    if (it == replies_.end() || !it->sessionBound || replyHttpStatus(reply) != 404) {
        return false;
    }

    const QString message
        = QStringLiteral("MCP session expired: %1").arg(httpFailureMessage(reply, 404));
    setState(State::Stopping);
    clearReplies();

    const quint64                  generation = lifecycleGeneration_;
    QPointer<QSocMcpHttpTransport> guard(this);
    emit                           errorOccurred(message);
    if (guard.isNull() || lifecycleGeneration_ != generation || state() != State::Stopping) {
        return true;
    }

    setState(State::Stopped);
    emit closed();
    return true;
}

void QSocMcpHttpTransport::handleSseChunk(QNetworkReply *reply)
{
    QList<DeferredSignal> events;
    QList<int>            requestIds;
    quint64               sendToken = 0;
    bool                  failed    = false;
    {
        auto it = replies_.find(reply);
        if (it == replies_.end()) {
            return;
        }
        it->sseBuffer += reply->readAll();
        events = parseSseEvents(it->sseBuffer);
        for (const auto &event : std::as_const(events)) {
            if (!event.error.isEmpty()) {
                failed = true;
                break;
            }
        }
        if (failed) {
            requestIds = it->requestIds;
            sendToken  = it->sendToken;
        }
    }
    if (failed) {
        cleanupReply(reply, true);
    }

    const quint64                  generation = lifecycleGeneration_;
    QPointer<QSocMcpHttpTransport> guard(this);
    for (const auto &event : std::as_const(events)) {
        if (event.error.isEmpty()) {
            emit messageReceived(event.message);
        } else {
            emit messageFailed(sendToken, requestIds, event.error);
            return;
        }
        if (guard.isNull() || lifecycleGeneration_ != generation) {
            return;
        }
    }
}

void QSocMcpHttpTransport::failReply(QNetworkReply *reply, const QString &message)
{
    auto it = replies_.find(reply);
    if (it == replies_.end()) {
        return;
    }
    const QList<int> requestIds = it->requestIds;
    const quint64    sendToken  = it->sendToken;
    cleanupReply(reply, true);
    emit messageFailed(sendToken, requestIds, message);
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

void QSocMcpHttpTransport::cleanupReply(QNetworkReply *reply, bool abort)
{
    if (reply == nullptr) {
        return;
    }
    replies_.remove(reply);
    disconnect(reply, nullptr, this, nullptr);
    if (abort && reply->isRunning()) {
        reply->abort();
    }
    reply->deleteLater();
}

QNetworkReply *QSocMcpHttpTransport::senderReply()
{
    return qobject_cast<QNetworkReply *>(sender());
}
