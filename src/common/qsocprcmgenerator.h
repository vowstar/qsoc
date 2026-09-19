// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMGENERATOR_H
#define QSOCPRCMGENERATOR_H

#include "common/qsocprcmbinding.h"

struct QSocPrcmCircuit
{
    QSocMmioPlan                           mmio;
    QMap<QString, QString>                 rtl;
    QMap<QString, QSocMmioPortDescription> port;
};

struct QSocPrcmGenerateResult
{
    std::optional<QSocPrcmCircuit> circuit;
    QList<QSocPrcmDiagnostic>      diagnostic;
};

class QSocPrcmGenerator
{
public:
    /* The caller supplies a bound resource plan and a physical sampling stage count. */
    static QSocPrcmGenerateResult generate(
        const QSocPrcmBindingPlan &binding, const QString &moduleName, int sampleStage);
};

#endif // QSOCPRCMGENERATOR_H
