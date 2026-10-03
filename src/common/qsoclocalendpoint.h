// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOCALENDPOINT_H
#define QSOCLOCALENDPOINT_H

#include <QString>

namespace QSocLocalEndpoint {

QString resolve(const QString &path);
bool    prepareDirectory(const QString &path, QString *error);

} // namespace QSocLocalEndpoint

#endif // QSOCLOCALENDPOINT_H
