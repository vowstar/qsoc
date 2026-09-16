// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAXIVERIFICATION_H
#define QSOCMMIOAXIVERIFICATION_H

#include "qsocmmiogenerator.h"

namespace QSocMmioAxiVerification {
QString               formal(const QSocMmioPlan &plan);
QSocMmioUvmCollateral uvm(const QSocMmioPlan &plan);
} // namespace QSocMmioAxiVerification

#endif // QSOCMMIOAXIVERIFICATION_H
