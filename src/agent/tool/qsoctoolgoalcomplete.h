// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLGOALCOMPLETE_H
#define QSOCTOOLGOALCOMPLETE_H

#include "agent/qsoctool.h"

class QSocGoalCatalog;

/**
 * @brief LLM-facing tool that marks the active project goal complete.
 * @details Schema-restricted to a single status value (@c complete);
 *          pause / resume / budget-limited transitions are not
 *          reachable through this tool. Following codex's contract,
 *          the LLM must verify the objective is actually met before
 *          calling. The catalog drops the goal after recording the
 *          status change so a fresh /goal can be set without an
 *          extra /goal clear step.
 *
 *          Sub-agent dispatch leaves the catalog pointer unset so a
 *          child run cannot end the parent's goal; the parent must
 *          decide completion based on its own context.
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
    QSocGoalCatalog *catalog_ = nullptr;
};

#endif // QSOCTOOLGOALCOMPLETE_H
