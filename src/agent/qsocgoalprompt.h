// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGOALPROMPT_H
#define QSOCGOALPROMPT_H

#include <QString>

struct QSocGoal;

/**
 * @brief Render free-text snippets injected into the user role at
 *        well-known points in the goal lifecycle.
 * @details Three templates mirror the codex thread-goal feature:
 *          - @c continuation runs every auto-continuation turn,
 *          - @c budgetLimit runs once when the token budget is hit,
 *          - @c objectiveUpdated runs once after the user replaces
 *            the objective of an in-flight goal.
 *
 *          All three wrap their body in @c <goal_context>…</goal_context>
 *          markers so the LLM can recognize untrusted user-supplied
 *          content versus system policy, and so a future trimmer can
 *          drop the block from the rolling history without breaking
 *          adjacent assistant turns.
 *
 *          The text intentionally repeats the rule "do not call
 *          goal_complete unless the goal is actually complete" so
 *          that a model that drops the system prompt still sees the
 *          guard in the user turn.
 */
namespace QSocGoalPrompt {

QString continuation(const QSocGoal &goal);
QString budgetLimit(const QSocGoal &goal);
QString objectiveUpdated(const QSocGoal &goal);

} // namespace QSocGoalPrompt

#endif // QSOCGOALPROMPT_H
