// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSMTSERVICE_H
#define QSOCSMTSERVICE_H

#include <stop_token>
#include <QJsonObject>
#include <QString>

class QSocSmtService
{
public:
    static constexpr int inputLimit     = 256 * 1024;
    static constexpr int outputLimit    = 1024 * 1024;
    static constexpr int memoryLimitMiB = 512;

    static QString workerPath();
    static bool    supported();
    /* Blocking service for a deferred tool's background task. */
    static QJsonObject solve(
        const QJsonObject &request, std::stop_token stop = {}, const QString &executable = {});
    static QString     validateRequest(const QJsonObject &request);
    static QJsonObject failure(const QString &execution, const QString &reason);
};

#endif
