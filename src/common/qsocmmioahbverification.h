// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAHBVERIFICATION_H
#define QSOCMMIOAHBVERIFICATION_H

#include "qsocmmiogenerator.h"

namespace QSocMmioAhbVerification {
QString               formal(const QSocMmioPlan &plan);
QSocMmioUvmCollateral uvm(const QSocMmioPlan &plan);
} // namespace QSocMmioAhbVerification

#endif // QSOCMMIOAHBVERIFICATION_H
