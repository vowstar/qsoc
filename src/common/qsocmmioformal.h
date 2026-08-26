// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOFORMAL_H
#define QSOCMMIOFORMAL_H

struct QSocMmioFormalCollateral;
struct QSocMmioPlan;

namespace QSocMmioFormal {

QSocMmioFormalCollateral generate(const QSocMmioPlan &plan);

} // namespace QSocMmioFormal

#endif // QSOCMMIOFORMAL_H
