// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSEQUENCEPLAN_H
#define QSOCPRCMSEQUENCEPLAN_H

#include "common/qsocprcminput.h"
#include "common/qsocprcmsequence.h"

struct QSocPrcmSequencePlan
{
    QString                       domain;
    QMap<quint64, QSocPrcmTarget> mode;
    quint64                       resetCode = 0;
};

struct QSocPrcmSequencePlanResult
{
    std::optional<QSocPrcmSequencePlan> plan;
    QList<QSocPrcmDiagnostic>           diagnostic;
};

class QSocPrcmSequencePlanner
{
public:
    /* Select the single-domain template after resource and stable-mode checks. */
    static QSocPrcmSequencePlanResult build(const QSocPrcmInput &input);
    /* Select local actions. The caller must validate service and chip composition. */
    static QSocPrcmSequencePlanResult buildDomain(const QSocPrcmInput &input, const QString &name);
};

#endif // QSOCPRCMSEQUENCEPLAN_H
