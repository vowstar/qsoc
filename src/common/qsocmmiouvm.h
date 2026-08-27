// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOUVM_H
#define QSOCMMIOUVM_H

struct QSocMmioPlan;
struct QSocMmioUvmCollateral;

namespace QSocMmioUvm {

QSocMmioUvmCollateral generate(const QSocMmioPlan &plan);

} // namespace QSocMmioUvm

#endif // QSOCMMIOUVM_H
