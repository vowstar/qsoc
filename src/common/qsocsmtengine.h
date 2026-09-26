// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSMTENGINE_H
#define QSOCSMTENGINE_H

#include <functional>
#include <QJsonObject>

namespace QSocSmtEngine {
/* These functions run only in the isolated worker. */
bool applyLimits();
enum class Phase { Parse, Solve, Verify, Serialize };
using PhaseObserver = std::function<void(Phase)>;
QJsonObject execute(const QJsonObject &request, const PhaseObserver &observer = {});
} // namespace QSocSmtEngine

#endif
