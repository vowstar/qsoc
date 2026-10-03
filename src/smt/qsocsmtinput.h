// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSMTINPUT_H
#define QSOCSMTINPUT_H

#include <vector>
#include <QByteArray>
#include <QStringList>

struct QSocSmtObjective
{
    bool    maximize = false;
    QString expression;
};

struct QSocSmtInput
{
    QString                       error;
    QStringList                   labels;
    std::vector<QSocSmtObjective> objectives;
    static QSocSmtInput           scan(const QByteArray &input, bool optimize);
};

#endif
