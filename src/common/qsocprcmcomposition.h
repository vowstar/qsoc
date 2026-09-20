// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMCOMPOSITION_H
#define QSOCPRCMCOMPOSITION_H

#include "common/qsocprcmsequenceplan.h"

struct QSocPrcmServicePlan
{
    QString     consumer;
    QString     provider;
    QStringList source;
};

struct QSocPrcmCompositionPlan
{
    QMap<QString, QSocPrcmSequencePlan>                         domain;
    QList<QSocPrcmServicePlan>                                  service;
    QMap<quint64, QMap<QString, std::optional<QSocPrcmTarget>>> chip;
    quint64                                                     resetCode = 0;
};

struct QSocPrcmCompositionResult
{
    std::optional<QSocPrcmCompositionPlan> plan;
    QList<QSocPrcmDiagnostic>              diagnostic;
};

class QSocPrcmComposition
{
public:
    /* Select shared actions after resource binding and stable-mode checks. */
    static QSocPrcmCompositionResult build(const QSocPrcmInput &input);
};

#endif // QSOCPRCMCOMPOSITION_H
