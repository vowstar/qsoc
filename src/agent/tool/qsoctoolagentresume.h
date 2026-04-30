// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENTRESUME_H
#define QSOCTOOLAGENTRESUME_H

#include "agent/qsoctool.h"

class QSocSubAgentTaskSource;

/**
 * @brief LLM-callable tool that prepares a resume payload from a
 *        backgrounded sub-agent run that survived a process restart.
 * @details Reads the run's `.meta.json` sidecar (subagent_type,
 *          label, status, isolation, worktree) and tail of the
 *          `.jsonl` transcript, and returns a synthesized
 *          `resume_prompt` plus the original `subagent_type`.
 *          The LLM is expected to follow up with the regular
 *          `agent` tool using those fields, optionally seeding it
 *          with new instructions appended to `resume_prompt`.
 *          This is the qsoc equivalent of claude-code's resumeAgent
 *          path: a fresh query() with the prior context restored,
 *          not a live LLM stream reattach.
 */
class QSocToolAgentResume : public QSocTool
{
    Q_OBJECT

public:
    QSocToolAgentResume(QObject *parent, QSocSubAgentTaskSource *taskSource);
    ~QSocToolAgentResume() override = default;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSubAgentTaskSource *taskSource_ = nullptr;
};

#endif /* QSOCTOOLAGENTRESUME_H */
