// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocagentdaemonclient.h"
#include "agent/protocol/qsocagentprotocol.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsoclocalpeer.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

namespace {
constexpr int kHeaderBytes = QSocAgentProtocol::headerBytes;
using QSocAgentProtocol::frame;

QSocAgentRuntimeEvent eventFromJson(const QJsonObject &value)
{
    return QSocAgentRuntimeEvent::fromJson(
        nlohmann::json::parse(QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString()));
}
} // namespace

QSocAgentDaemonClient::QSocAgentDaemonClient(const QString &socketPath, QObject *parent)
    : QObject(parent)
    , m_socketPath(QSocLocalEndpoint::resolve(socketPath))
{
    m_socket.setReadBufferSize(QSocIpc::maxPayloadBytes + QSocIpc::headerBytes);
    connect(&m_socket, &QLocalSocket::readyRead, this, &QSocAgentDaemonClient::handleReadyRead);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this]() { emit disconnected(); });
}

QSocAgentDaemonClient::~QSocAgentDaemonClient() = default;

bool QSocAgentDaemonClient::hasCapability(const QString &name) const
{
    return m_greeting.value("capabilities").toArray().contains(name);
}

qint64 QSocAgentDaemonClient::daemonProcessId() const
{
    return QSocLocalPeer::processId(m_socket);
}

bool QSocAgentDaemonClient::connectToDaemon(int timeoutMs)
{
    m_error.clear();
    m_buffer.clear();
    m_greeting = {};
    m_socket.abort();
    m_socket.connectToServer(m_socketPath);
    if (!m_socket.waitForConnected(timeoutMs)) {
        m_error = m_socket.errorString();
        return false;
    }
    if (!QSocLocalPeer::sameUser(m_socket)) {
        m_error = QStringLiteral(
            "agent daemon security context differs or its identity is unavailable");
        m_socket.abort();
        return false;
    }
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto received = connect(this, &QSocAgentDaemonClient::replyReceived, &loop, [&] {
        if (!m_greeting.isEmpty())
            loop.quit();
    });
    auto lost     = connect(this, &QSocAgentDaemonClient::disconnected, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    if (m_greeting.isEmpty() && isConnected())
        loop.exec();
    disconnect(received);
    disconnect(lost);
    if (m_greeting.value("protocol").toInt() != QSocAgentProtocol::version
        || m_greeting.value("daemon").toString() != QStringLiteral("qsoc-agentd")) {
        m_error = QStringLiteral("missing or incompatible daemon protocol greeting");
        m_socket.abort();
        return false;
    }
    m_daemonVersion = m_greeting.value("version").toString();
    return true;
}

QJsonObject QSocAgentDaemonClient::request(
    const QString &method, const QJsonObject &params, int timeoutMs)
{
    const auto id = nextId();
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QJsonObject reply;
    auto        received
        = connect(this, &QSocAgentDaemonClient::replyReceived, &loop, [&](const QJsonObject &value) {
              if (value.value("id").toInteger() == id) {
                  reply = value;
                  loop.quit();
              }
          });
    auto lost = connect(this, &QSocAgentDaemonClient::disconnected, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (timeoutMs > 0)
        timer.start(timeoutMs);
    send({{"id", id}, {"method", method}, {"params", params}});
    if (reply.isEmpty() && isConnected())
        loop.exec();
    disconnect(received);
    disconnect(lost);
    if (reply.isEmpty())
        reply.insert(
            "error",
            isConnected() ? QStringLiteral("daemon request timed out")
                          : QStringLiteral("daemon disconnected"));
    return reply;
}

void QSocAgentDaemonClient::send(const QJsonObject &request)
{
    const auto encoded = frame(request);
    if (encoded.isEmpty()
        || m_socket.bytesToWrite() + encoded.size() > QSocIpc::maxPayloadBytes + kHeaderBytes) {
        m_error = QStringLiteral("daemon request buffer limit exceeded");
        m_socket.abort();
        return;
    }
    m_socket.write(encoded);
    m_socket.flush();
}

qint64 QSocAgentDaemonClient::nextId()
{
    return ++m_requestCounter;
}

void QSocAgentDaemonClient::handleReadyRead()
{
    if (m_dispatchPending)
        return;
    m_dispatchPending = true;
    QTimer::singleShot(0, this, [this] {
        m_dispatchPending = false;
        processFrame();
    });
}

void QSocAgentDaemonClient::processFrame()
{
    const auto capacity = QSocIpc::maxPayloadBytes + kHeaderBytes - m_buffer.size();
    if (capacity > 0 && m_socket.bytesAvailable() > 0)
        m_buffer += m_socket.read(capacity);
    QJsonObject frameObject;
    const auto  state = QSocIpc::decode(m_buffer, frameObject, QSocIpc::maxPayloadBytes, &m_error);
    if (state == QSocIpc::DecodeResult::Invalid) {
        m_buffer.clear();
        m_socket.abort();
        return;
    }
    if (state == QSocIpc::DecodeResult::Incomplete)
        return;
    // Schedule before emitting: an event handler can enter a nested request loop.
    if (!m_buffer.isEmpty() || m_socket.bytesAvailable() > 0)
        handleReadyRead();
    if (frameObject.contains(QStringLiteral("event"))) {
        if (!frameObject.value(QStringLiteral("event")).isObject()) {
            m_error = QStringLiteral("invalid daemon event");
            m_buffer.clear();
            m_socket.abort();
            return;
        }
        emit eventReceived(eventFromJson(frameObject.value(QStringLiteral("event")).toObject()));
    } else {
        if (frameObject.contains("daemon"))
            m_greeting = frameObject;
        emit replyReceived(frameObject);
    }
}

#include "moc_qsocagentdaemonclient.cpp"
