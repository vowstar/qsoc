// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAPBVERIFICATION_H
#define QSOCMMIOAPBVERIFICATION_H

#include "qsocmmiogenerator.h"

namespace QSocMmioApbVerification {
QString               formal(const QSocMmioPlan &plan);
QSocMmioUvmCollateral uvm(const QSocMmioPlan &plan);
} // namespace QSocMmioApbVerification

#endif // QSOCMMIOAPBVERIFICATION_H
