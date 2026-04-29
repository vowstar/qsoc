// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSUBAGENTTASKSOURCE_H
#define QSOCSUBAGENTTASKSOURCE_H

#include "agent/qsoctasksource.h"

/**
 * @brief Placeholder source for future in-process sub-agents.
 * @details Validates that the QSocTaskSource abstraction can absorb a
 *          third task kind without touching the overlay or registry.
 *          Today it always returns an empty task list; the real
 *          implementation lands when the sub-agent subsystem is wired.
 *          The class compiles, registers, and shows up in unit tests
 *          so a future contributor only fills in the four method
 *          bodies.
 */
class QSocSubAgentTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocSubAgentTaskSource(QObject *parent = nullptr);
    ~QSocSubAgentTaskSource() override = default;

    QString              sourceTag() const override { return QStringLiteral("agent"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;
};

#endif /* QSOCSUBAGENTTASKSOURCE_H */
