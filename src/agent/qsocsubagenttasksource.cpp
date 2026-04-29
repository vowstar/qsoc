// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsubagenttasksource.h"

QSocSubAgentTaskSource::QSocSubAgentTaskSource(QObject *parent)
    : QSocTaskSource(parent)
{}

QList<QSocTask::Row> QSocSubAgentTaskSource::listTasks() const
{
    /* Empty until sub-agents land. Returning {} here keeps the registry
     * happy and proves a third source plugs into the abstraction with
     * zero changes to overlay / compositor / aggregator. */
    return {};
}

QString QSocSubAgentTaskSource::tailFor(const QString &id, int maxBytes) const
{
    Q_UNUSED(id);
    Q_UNUSED(maxBytes);
    return {};
}

bool QSocSubAgentTaskSource::killTask(const QString &id)
{
    Q_UNUSED(id);
    return false;
}
