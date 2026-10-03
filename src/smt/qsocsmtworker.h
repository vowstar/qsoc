// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSMTWORKER_H
#define QSOCSMTWORKER_H

#include <functional>
#include <QJsonObject>

namespace QSocSmtWorker {
using Executor = std::function<QJsonObject(const QJsonObject &)>;
int run(int argc, char **argv, const Executor &execute);
} // namespace QSocSmtWorker

#endif
