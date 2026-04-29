// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCBASHTASKSOURCE_H
#define QSOCBASHTASKSOURCE_H

#include "agent/qsoctasksource.h"

class QSocToolShellBash;

/**
 * @brief Adapter exposing background bash processes as task overlay rows.
 * @details Listens to bashTool::backgroundProcessFinished and
 *          ::processStuckDetected so the overlay refreshes when processes
 *          exit or get flagged. The bash tool's activeProcesses map is
 *          static so multiple sources would step on each other; the
 *          design intentionally has one source instance per agent.
 */
class QSocBashTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocBashTaskSource(QSocToolShellBash *bashTool, QObject *parent = nullptr);
    ~QSocBashTaskSource() override = default;

    QString              sourceTag() const override { return QStringLiteral("bg"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

private:
    QSocToolShellBash *bashTool_ = nullptr;
};

#endif /* QSOCBASHTASKSOURCE_H */
