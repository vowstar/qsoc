// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOAXI_H
#define QSOCMMIOAXI_H

#include "qsocmmiogenerator.h"

class QSocMmioAxi
{
public:
    static QList<QSocMmioPortDescription> ports(const QSocMmioPlan &plan);
    static QString                        generate(const QSocMmioPlan &plan);
};

#endif // QSOCMMIOAXI_H
