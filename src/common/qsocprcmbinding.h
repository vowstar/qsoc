// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMBINDING_H
#define QSOCPRCMBINDING_H

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivereset.h"
#include "common/qsocprcminput.h"

struct QSocPrcmDomainResource
{
    QString clockEnable;
    bool    resetSourceActiveLow = true;
    bool    resetTargetActiveLow = true;
};

struct QSocPrcmBindingPlan
{
    QSocPrcmInput                             input;
    QSocClockPrimitive::ClockControllerConfig clock;
    QSocResetPrimitive::ResetControllerConfig reset;
    QMap<QString, QSocPrcmDomainResource>     domain;
};

struct QSocPrcmBindingResult
{
    std::optional<QSocPrcmBindingPlan> plan;
    QList<QSocPrcmDiagnostic>          diagnostic;
};

class QSocPrcmBinding
{
public:
    /* Bind direct clock gates and reset synchronizers. No transition proof. */
    static QSocPrcmBindingResult resolve(const YAML::Node &netlist, const QString &file);
};

#endif // QSOCPRCMBINDING_H
