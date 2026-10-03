// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCRESOURCEUSAGE_P_H
#define QSOCRESOURCEUSAGE_P_H

#include <QJsonObject>
#include <QString>

namespace QSocResourceUsage::detail {

inline bool followsParentStart(const QString &parentStart, const QString &childStart)
{
    bool       parentValid = false;
    bool       childValid  = false;
    const auto parent      = parentStart.toULongLong(&parentValid);
    const auto child       = childStart.toULongLong(&childValid);
    return parentValid && childValid && parent > 0 && child >= parent;
}

#ifdef Q_OS_LINUX
void sampleCgroupMemory(QJsonObject &result, const QString &procDirectory);
#endif

} // namespace QSocResourceUsage::detail

#endif // QSOCRESOURCEUSAGE_P_H
