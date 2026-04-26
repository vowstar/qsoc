// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptransport.h"

QSocMcpTransport::QSocMcpTransport(QObject *parent)
    : QObject(parent)
{}

QSocMcpTransport::~QSocMcpTransport() = default;

QSocMcpTransport::State QSocMcpTransport::state() const
{
    return state_;
}

void QSocMcpTransport::setState(State newState)
{
    state_ = newState;
}
