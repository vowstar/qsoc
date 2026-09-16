// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAPB_H
#define QSOCMMIOAPB_H

#include "qsocmmiogenerator.h"

namespace QSocMmioApb {
QList<QSocMmioPortDescription> ports(const QSocMmioPlan &plan);
QString                        generate(const QSocMmioPlan &plan);
} // namespace QSocMmioApb

#endif // QSOCMMIOAPB_H
