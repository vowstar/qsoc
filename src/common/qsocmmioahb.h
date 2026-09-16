// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAHB_H
#define QSOCMMIOAHB_H

#include "qsocmmiogenerator.h"

class QSocMmioAhb
{
public:
    static QList<QSocMmioPortDescription> ports(const QSocMmioPlan &plan);
    static QString                        generate(const QSocMmioPlan &plan);
};

#endif // QSOCMMIOAHB_H
