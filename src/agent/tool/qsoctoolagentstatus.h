// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENTSTATUS_H
#define QSOCTOOLAGENTSTATUS_H

#include "agent/qsoctool.h"

class QSocSubAgentTaskSource;

/**
 * @brief LLM-callable tool that reports the status of an async
 *        sub-agent run by task_id.
 * @details Pairs with the `agent` spawn tool: when the parent fires
 *          `agent` with `run_in_background=true`, it gets back a
 *          `task_id`. This tool returns that run's current status,
 *          elapsed time, and a tail of the captured transcript so
 *          the parent can decide whether to wait, query again, or
 *          abandon the run, without needing the human to open the
 *          Ctrl+B overlay.
 */
class QSocToolAgentStatus : public QSocTool
{
    Q_OBJECT

public:
    QSocToolAgentStatus(QObject *parent, QSocSubAgentTaskSource *taskSource);
    ~QSocToolAgentStatus() override = default;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSubAgentTaskSource *taskSource_ = nullptr;
};

#endif /* QSOCTOOLAGENTSTATUS_H */
