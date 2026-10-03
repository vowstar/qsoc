// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocagentdaemonclient.h"
#include "agent/protocol/qsocagentprotocol.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsoclocalpeer.h"

#include <QEventLoop>
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
    connect(&m_socket, &QLocalSocket::readyRead, this, &QSocAgentDaemonClient::handleReadyRead);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this]() { emit disconnected(); });
}

QSocAgentDaemonClient::~QSocAgentDaemonClient() = default;

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
            "agent daemon belongs to another user or its identity is unavailable");
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
    if (isConnected())
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
    m_socket.write(frame(request));
    m_socket.flush();
}

qint64 QSocAgentDaemonClient::nextId()
{
    return ++m_requestCounter;
}

void QSocAgentDaemonClient::handleReadyRead()
{
    m_buffer += m_socket.readAll();
    while (true) {
        if (m_buffer.size() < kHeaderBytes) {
            return;
        }
        const int length = QSocAgentProtocol::payloadLength(m_buffer);
        if (length < 0) {
            m_socket.disconnectFromServer();
            return;
        }
        if (m_buffer.size() < kHeaderBytes + length) {
            return;
        }
        const QByteArray payload = m_buffer.mid(kHeaderBytes, length);
        m_buffer.remove(0, kHeaderBytes + length);
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject frameObject = doc.object();
        if (frameObject.contains(QStringLiteral("event"))) {
            const auto event = eventFromJson(frameObject.value(QStringLiteral("event")).toObject());
            QTimer::singleShot(0, this, [this, event] { emit eventReceived(event); });
        } else {
            if (frameObject.contains("daemon"))
                m_greeting = frameObject;
            QTimer::singleShot(0, this, [this, frameObject] { emit replyReceived(frameObject); });
        }
    }
}

#include "moc_qsocagentdaemonclient.cpp"
