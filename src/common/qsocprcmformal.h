// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMFORMAL_H
#define QSOCPRCMFORMAL_H

#include "common/qsocprcmgenerator.h"

namespace QSocPrcmFormal {

QSocMmioFormalCollateral generate(
    const QSocPrcmBindingPlan &binding,
    const QSocPrcmCircuit     &circuit,
    const QString             &moduleName,
    int                        sampleStage);

} // namespace QSocPrcmFormal

#endif // QSOCPRCMFORMAL_H
