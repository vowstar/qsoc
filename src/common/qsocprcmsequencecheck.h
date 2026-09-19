// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSEQUENCECHECK_H
#define QSOCPRCMSEQUENCECHECK_H

#include "common/qsocprcmsequenceplan.h"
#include "common/qsocprcmsolver.h"

struct QSocPrcmSequenceFrame
{
    QSocPrcmSequenceState state;
    QSocPrcmObservation   observation;
};

struct QSocPrcmProgressResult
{
    QSocPrcmCheckStatus           status = QSocPrcmCheckStatus::Error;
    QString                       reason;
    std::optional<QSocPrcmTarget> target;
    /* The last frame repeats the first frame to close the walk. */
    QList<QSocPrcmSequenceFrame> loop;
};

class QSocPrcmSequenceCheck
{
public:
    /* Feedback holds or follows each request. Independent reset and faults are outside this model. */
    /* UNSAT excludes a normal-sequence safety violation in the reachable model. */
    static QSocPrcmCheckResult safety(
        const QSocPrcmSequencePlan &plan,
        const QSocPrcmCheckBudget  &budget = {},
        std::stop_token             stop   = {});
    /* UNSAT excludes noncompletion if the target settles and held requests receive feedback. */
    static QSocPrcmProgressResult progress(
        const QSocPrcmSequencePlan &plan, std::stop_token stop = {});
};

#endif // QSOCPRCMSEQUENCECHECK_H
