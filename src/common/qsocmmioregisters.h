// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOREGISTERS_H
#define QSOCMMIOREGISTERS_H

#include "qsocmmiogenerator.h"

struct QSocMmioRegisterRtl
{
    QString storage;
    QString decode;
    QString write;
};

class QSocMmioRegisters
{
public:
    static QSocMmioRegisterRtl generate(
        const QSocMmioPlan &plan,
        const QString      &address,
        const QString      &data,
        const QString      &strobe);
};

#endif // QSOCMMIOREGISTERS_H
