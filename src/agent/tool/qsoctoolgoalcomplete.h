// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLGOALCOMPLETE_H
#define QSOCTOOLGOALCOMPLETE_H

#include "agent/qsoctool.h"

#include <QPointer>

class QSocGoalCatalog;

/**
 * @brief LLM-facing tool that marks the active project goal complete.
 * @details Schema-restricted to a single status value (@c complete);
 *          pause / resume / budget-limited transitions are not
 *          reachable through this tool. The objective must be achieved
 *          before calling. The catalog drops the goal after recording
 *          the status change so a fresh /goal can be set without an
 *          extra /goal clear step.
 *
 *          The agent gate excludes this shared tool from sub-agent
 *          runs; a child returns completion evidence to the parent.
 */
class QSocToolGoalComplete : public QSocTool
{
    Q_OBJECT

public:
    QSocToolGoalComplete(QObject *parent, QSocGoalCatalog *catalog);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QPointer<QSocGoalCatalog> catalog_;
};

#endif // QSOCTOOLGOALCOMPLETE_H
