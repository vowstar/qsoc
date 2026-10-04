// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCDAEMONCONNECTION_H
#define QSOCDAEMONCONNECTION_H

#include "agent/client/qsocagentdaemonclient.h"
#include <memory>
#include <QProcess>
#include <QTemporaryDir>

class QSocDaemonConnection
{
public:
    explicit QSocDaemonConnection(const QString &endpoint);
    ~QSocDaemonConnection();
    bool                   start();
    QSocAgentDaemonClient &client() { return *client_; }
    QString                error() const { return error_; }

private:
    QTemporaryDir                          directory_;
    QProcess                               child_;
    QString                                endpoint_;
    QString                                error_;
    bool                                   owned_;
    std::unique_ptr<QSocAgentDaemonClient> client_;
};

#endif
