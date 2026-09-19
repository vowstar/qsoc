// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSERVICECHECK_H
#define QSOCPRCMSERVICECHECK_H

#include "common/qsocprcmsequencecheck.h"

enum class QSocPrcmServicePhase { Idle, Wait, Hold, Return };

struct QSocPrcmServiceFrame
{
    QSocPrcmSequenceFrame provider;
    QSocPrcmSequenceFrame consumer;
    QSocPrcmServicePhase  phase = QSocPrcmServicePhase::Idle;
    bool                  grant = false;
    bool                  other = false;
};

struct QSocPrcmServiceProgressResult
{
    QSocPrcmCheckStatus         status = QSocPrcmCheckStatus::Error;
    QString                     reason;
    QSocPrcmTarget              provider = QSocPrcmTarget::Off;
    QSocPrcmTarget              consumer = QSocPrcmTarget::Off;
    QList<QSocPrcmServiceFrame> loop;
};

class QSocPrcmServiceCheck
{
public:
    /* Normal service projection. Permission from other services may vary arbitrarily. */
    static QSocPrcmCheckResult safety(
        const QSocPrcmCheckBudget &budget = {}, std::stop_token stop = {});
    /* Requires held feedback to respond and other service permission to settle high. */
    static QSocPrcmServiceProgressResult progress(std::stop_token stop = {});
};

#endif // QSOCPRCMSERVICECHECK_H
