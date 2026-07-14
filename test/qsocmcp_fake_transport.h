// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCP_FAKE_TRANSPORT_H
#define QSOCMCP_FAKE_TRANSPORT_H

#include "agent/mcp/qsocmcptransport.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <utility>

#include <QList>
#include <QObject>

/* Q_OBJECT is intentionally omitted: this class only emits signals
 * declared in QSocMcpTransport, which already has its own MOC. Adding
 * Q_OBJECT here would force every test that includes the header to add
 * a moc_ source to its target. */
class QsocMcpFakeTransport : public QSocMcpTransport
{
public:
    explicit QsocMcpFakeTransport(QObject *parent = nullptr)
        : QSocMcpTransport(parent)
    {}

    void start() override
    {
        if (state() == State::Running) {
            return;
        }
        startCount_++;
        setState(State::Running);
        emit started();
    }

    void stop() override
    {
        stopCount_++;
        if (state() == State::Stopping || state() == State::Stopped) {
            return;
        }
        setState(State::Stopping);
        if (closeOnStop_) {
            simulateClosed();
        }
    }

    void sendMessage(const nlohmann::json &message) override
    {
        sent_ << message;
        if (sendHook_) {
            sendHook_(message);
        }
    }

    void sendTrackedMessage(const nlohmann::json &message, quint64 token) override
    {
        lastTrackedToken_ = token;
        if (autoCompleteTrackedMessages_) {
            QSocMcpTransport::sendTrackedMessage(message, token);
            return;
        }
        sendMessage(message);
    }

    void abandonTrackedMessage(quint64 token) override
    {
        if (token != 0) {
            abandonTrackedCalls_ << token;
        }
    }

    void abandonRequest(int requestId) override { abandonedRequestIds_ << requestId; }

    void simulateMessage(const nlohmann::json &message) { emit messageReceived(message); }
    void simulateError(const QString &message) { emit errorOccurred(message); }
    void simulateMessageFailure(quint64 token, const QList<int> &requestIds, const QString &message)
    {
        emit messageFailed(token, requestIds, message);
    }
    void setSendHook(std::function<void(const nlohmann::json &)> hook)
    {
        sendHook_ = std::move(hook);
    }
    void setCloseOnStop(bool enabled) { closeOnStop_ = enabled; }
    void setAutoCompleteTrackedMessages(bool enabled) { autoCompleteTrackedMessages_ = enabled; }

    void simulateClosed()
    {
        if (state() == State::Stopped) {
            return;
        }
        setState(State::Stopped);
        emit closed();
    }

    void simulateDuplicateClosed() { emit closed(); }

    int                          firstSentId() const { return sent_.first()["id"].get<int>(); }
    int                          lastSentId() const { return sent_.last()["id"].get<int>(); }
    int                          startCount() const { return startCount_; }
    int                          stopCount() const { return stopCount_; }
    quint64                      lastTrackedToken() const { return lastTrackedToken_; }
    qsizetype                    sentCount() const { return sent_.size(); }
    const QList<nlohmann::json> &sent() const { return sent_; }
    const QList<int>            &abandonedRequestIds() const { return abandonedRequestIds_; }
    const QList<quint64>        &abandonTrackedCalls() const { return abandonTrackedCalls_; }

private:
    QList<nlohmann::json>                       sent_;
    QList<int>                                  abandonedRequestIds_;
    QList<quint64>                              abandonTrackedCalls_;
    std::function<void(const nlohmann::json &)> sendHook_;
    bool                                        closeOnStop_                 = true;
    bool                                        autoCompleteTrackedMessages_ = true;
    int                                         startCount_                  = 0;
    int                                         stopCount_                   = 0;
    quint64                                     lastTrackedToken_            = 0;
};

#endif // QSOCMCP_FAKE_TRANSPORT_H
