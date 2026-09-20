// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSHARED_H
#define QSOCPRCMSHARED_H

#include "common/qsocprcmgenerator.h"

namespace QSocPrcmShared {

QSocPrcmGenerateResult generate(
    const QSocPrcmBindingPlan &binding, const QString &moduleName, int sampleStage);

} // namespace QSocPrcmShared

#endif // QSOCPRCMSHARED_H
