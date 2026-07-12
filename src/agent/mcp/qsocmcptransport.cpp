// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptransport.h"

#include <QPointer>

QSocMcpTransport::QSocMcpTransport(QObject *parent)
    : QObject(parent)
{}

QSocMcpTransport::~QSocMcpTransport() = default;

QSocMcpTransport::State QSocMcpTransport::state() const
{
    return state_;
}

void QSocMcpTransport::sendTrackedMessage(const nlohmann::json &message, quint64 token)
{
    if (token == 0) {
        sendMessage(message);
        return;
    }

    bool                          failed          = false;
    const QMetaObject::Connection errorConnection = connect(
        this,
        &QSocMcpTransport::errorOccurred,
        this,
        [&failed](const QString &) { failed = true; },
        Qt::DirectConnection);
    const QPointer<QSocMcpTransport> guard(this);
    try {
        sendMessage(message);
    } catch (...) {
        if (!guard.isNull()) {
            disconnect(errorConnection);
        }
        throw;
    }
    if (guard.isNull()) {
        return;
    }
    disconnect(errorConnection);
    if (failed || state() != State::Running) {
        return;
    }
    emit messageSent(token);
}

void QSocMcpTransport::setState(State newState)
{
    state_ = newState;
}
