// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCRESOURCEUSAGE_H
#define QSOCRESOURCEUSAGE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace QSocResourceUsage {

// Cumulative CPU counters use nanoseconds. Unavailable measurements are null.
QJsonObject system();
QJsonObject process(qint64 pid);
// A best-effort ancestry snapshot, not a security or ownership boundary.
QJsonArray  processTree(qint64 rootPid);
QJsonObject storage(const QString &path);

} // namespace QSocResourceUsage

#endif // QSOCRESOURCEUSAGE_H
