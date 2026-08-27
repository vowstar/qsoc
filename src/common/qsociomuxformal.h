// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIOMUXFORMAL_H
#define QSOCIOMUXFORMAL_H

struct QSocIomuxFormalCollateral;
struct QSocIomuxPlan;

namespace QSocIomuxFormal {

QSocIomuxFormalCollateral generate(const QSocIomuxPlan &plan);

} // namespace QSocIomuxFormal

#endif // QSOCIOMUXFORMAL_H
