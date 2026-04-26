// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCP_FAKE_TRANSPORT_H
#define QSOCMCP_FAKE_TRANSPORT_H

#include "agent/mcp/qsocmcptransport.h"

#include <nlohmann/json.hpp>

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
        setState(State::Running);
        emit started();
    }

    void stop() override
    {
        if (state() == State::Stopped) {
            return;
        }
        setState(State::Stopped);
        emit closed();
    }

    void sendMessage(const nlohmann::json &message) override { sent_ << message; }

    void simulateMessage(const nlohmann::json &message) { emit messageReceived(message); }

    void simulateClosed()
    {
        if (state() == State::Stopped) {
            return;
        }
        setState(State::Stopped);
        emit closed();
    }

    int                          firstSentId() const { return sent_.first()["id"].get<int>(); }
    int                          lastSentId() const { return sent_.last()["id"].get<int>(); }
    qsizetype                    sentCount() const { return sent_.size(); }
    const QList<nlohmann::json> &sent() const { return sent_; }

private:
    QList<nlohmann::json> sent_;
};

#endif // QSOCMCP_FAKE_TRANSPORT_H
