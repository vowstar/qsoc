// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSESSIONRECOVERY_H
#define QSOCSESSIONRECOVERY_H

#include "agent/qsocsession.h"

#include <nlohmann/json.hpp>

#include <optional>

#include <QString>

/**
 * @brief Pure recovery decision for an explicitly resumed main-agent session.
 */
class QSocSessionRecovery
{
public:
    enum class Action {
        Wait,
        ReplayInput,
        ResumeHistory,
        ContinueGoal,
    };

    struct Plan
    {
        Action         action = Action::Wait;
        nlohmann::json messages;
        QString        input;
        QString        reason;
        bool           requiresUserInput = false;
    };

    /**
     * @brief Decide whether and how to continue the latest run.
     * @param run Latest folded run record; empty means a legacy session.
     * @param messages Persisted OpenAI-style conversation history.
     * @param currentContext Execution context configured for the new process.
     * @param activeGoalId Current active goal id, or empty when none is active.
     */
    static Plan makePlan(
        const std::optional<QSocSession::RunRecord> &run,
        const nlohmann::json                        &messages,
        const QSocSession::RunRecord                &currentContext,
        const QString                               &activeGoalId = QString());

    /**
     * @brief True when a normal user turn can be appended without repairing history.
     */
    static bool historySafeForNewTurn(const nlohmann::json &messages);
};

#endif // QSOCSESSIONRECOVERY_H
