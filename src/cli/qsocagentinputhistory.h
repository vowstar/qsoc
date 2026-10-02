// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#ifndef QSOCAGENTINPUTHISTORY_H
#define QSOCAGENTINPUTHISTORY_H
#include <QMap>
#include <QStringList>

// Presentation state only: durable readline history and large-paste chips.
class QSocAgentInputHistory
{
public:
    QStringList load(const QString &project);
    QString     paste(const QString &text);
    QString     expand(const QString &display) const;
    void        append(const QString &display, const QString &project, const QString &host);

private:
    QMap<int, QString> pastes_;
    int                nextPaste_ = 1;
    static QString     path();
};
#endif
