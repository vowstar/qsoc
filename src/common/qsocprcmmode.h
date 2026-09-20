// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMMODE_H
#define QSOCPRCMMODE_H

#include "common/qsocprcminput.h"
#include "common/qsocprcmsolver.h"

struct QSocPrcmModeCase
{
    QString             name;
    QSocPrcmCheckResult result;
};

struct QSocPrcmModeResult
{
    QList<QSocPrcmDiagnostic> diagnostic;
    QList<QSocPrcmModeCase>   check;
};

class QSocPrcmModeCheck
{
public:
    /* Check declared stable modes. This does not check transitions or resource wiring. */
    static QSocPrcmModeResult check(
        const QSocPrcmInput       &input,
        const QSocPrcmCheckBudget &budget = {},
        std::stop_token            stop   = {});
};

#endif // QSOCPRCMMODE_H
