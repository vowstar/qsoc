// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSENDMESSAGE_H
#define QSOCTOOLSENDMESSAGE_H

#include "agent/qsoctool.h"

class QSocSubAgentTaskSource;

/**
 * @brief LLM-callable tool that pushes a follow-up user message
 *        into a Running async sub-agent's input queue.
 * @details Pairs with the `agent` spawn tool's
 *          `run_in_background=true` mode and the `agent_status`
 *          poll tool. Once the parent has a `task_id` for a child
 *          running asynchronously it can hand the child new
 *          instructions without waiting for the original prompt
 *          to finish; the child consumes the queued message at
 *          its next iteration boundary via QSocAgent::queueRequest.
 *          Single-recipient by design; multi-agent broadcast
 *          (`to: "*"`) requires a teammate model that is not in
 *          scope for qsoc.
 */
class QSocToolSendMessage : public QSocTool
{
    Q_OBJECT

public:
    QSocToolSendMessage(QObject *parent, QSocSubAgentTaskSource *taskSource);
    ~QSocToolSendMessage() override = default;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSubAgentTaskSource *taskSource_ = nullptr;
};

#endif /* QSOCTOOLSENDMESSAGE_H */
