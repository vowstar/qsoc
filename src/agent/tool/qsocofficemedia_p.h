// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#ifndef QSOCOFFICEMEDIA_P_H
#define QSOCOFFICEMEDIA_P_H

#include <functional>
#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QList>
#include <QString>

namespace QSocOfficeMedia {
struct Part
{
    int     page = 0;
    QString title;
    QString path;
};
struct Package
{
    std::function<bool(const QString &)>                  contains;
    std::function<QByteArray(const QString &, QString *)> read;
    std::function<bool()>                                 cancelled;
};
nlohmann::json discover(
    const QString &format, const QList<Part> &parts, const Package &package, QString *error);
} // namespace QSocOfficeMedia
#endif
