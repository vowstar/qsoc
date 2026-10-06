// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENTRESUME_H
#define QSOCTOOLAGENTRESUME_H

#include "agent/qsoctool.h"

class QSocSubAgentTaskSource;
class QSocToolAgent;

/**
 * @brief LLM-callable tool that continues a sub-agent run.
 * @details A live child is woken or queued, and a finished child with a
 *          stored history is rebuilt from it (QSocToolAgent::resumeRun).
 *          Otherwise the tool reads the `.meta.json` sidecar and the
 *          transcript tail and returns a synthesized `resume_prompt` plus
 *          the original `subagent_type` for a fresh `agent` call.
 */
class QSocToolAgentResume : public QSocTool
{
    Q_OBJECT

public:
    QSocToolAgentResume(
        QObject *parent, QSocSubAgentTaskSource *taskSource, QSocToolAgent *spawner = nullptr);
    ~QSocToolAgentResume() override = default;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSubAgentTaskSource *taskSource_ = nullptr;
    QSocToolAgent          *spawner_    = nullptr;
};

#endif /* QSOCTOOLAGENTRESUME_H */
