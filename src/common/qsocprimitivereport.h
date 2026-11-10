// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRIMITIVEREPORT_H
#define QSOCPRIMITIVEREPORT_H

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include <QJsonArray>
#include <QJsonObject>

namespace QSocPrimitiveReport {
QJsonObject graph(const QJsonArray &panels);
QJsonObject clock(const QSocClockPrimitive::ClockControllerConfig &config);
QJsonObject reset(const QSocResetPrimitive::ResetControllerConfig &config);
QJsonObject power(const QSocPowerPrimitive::PowerControllerConfig &config);
bool        stage(const QString &directory, const QString &name, const QJsonObject &data);
void        render(const QString &directory);
} // namespace QSocPrimitiveReport

#endif
