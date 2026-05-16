// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "agent/qsocgoalprompt.h"
#include "qsoc_test.h"

#include <QtTest>

namespace {

QSocGoal sampleGoal(int budget, int used)
{
    QSocGoal goal;
    goal.id          = QStringLiteral("test-goal-uuid");
    goal.objective   = QStringLiteral("Build top RTL and verify");
    goal.status      = QSocGoalStatus::Active;
    goal.tokenBudget = budget;
    goal.tokensUsed  = used;
    return goal;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void continuationWrapsObjective()
    {
        const QString body = QSocGoalPrompt::continuation(sampleGoal(0, 0));
        QVERIFY(body.startsWith(QStringLiteral("<goal_context>")));
        QVERIFY(body.endsWith(QStringLiteral("</goal_context>")));
        QVERIFY(
            body.contains(QStringLiteral("<objective>\nBuild top RTL and verify\n</objective>")));
        QVERIFY(body.contains(QStringLiteral("Completion audit")));
        QVERIFY(
            body.contains(QStringLiteral("do not call goal_complete").toLower())
            || body.contains(QStringLiteral("Do not call goal_complete")));
    }

    void continuationIncludesBudgetWhenSet()
    {
        const QString body = QSocGoalPrompt::continuation(sampleGoal(1000, 250));
        QVERIFY2(body.contains(QStringLiteral("250 / 1000 (750 left)")), qPrintable(body));
    }

    void continuationShowsNoBudgetWhenZero()
    {
        const QString body = QSocGoalPrompt::continuation(sampleGoal(0, 300));
        QVERIFY2(body.contains(QStringLiteral("300 (no budget set)")), qPrintable(body));
    }

    void budgetLimitMentionsWrapUp()
    {
        const QString body = QSocGoalPrompt::budgetLimit(sampleGoal(500, 500));
        QVERIFY(body.contains(QStringLiteral("budget_limited")));
        QVERIFY(body.contains(QStringLiteral("Wrap up this turn")));
        QVERIFY(body.contains(QStringLiteral("Do not call goal_complete")));
    }

    void objectiveUpdatedUsesUntrustedMarker()
    {
        const QString body = QSocGoalPrompt::objectiveUpdated(sampleGoal(0, 0));
        QVERIFY(body.contains(QStringLiteral("<untrusted_objective>")));
        QVERIFY(body.contains(QStringLiteral("</untrusted_objective>")));
        QVERIFY(body.contains(QStringLiteral("Build top RTL and verify")));
    }

    void allTemplatesWrapInGoalContext()
    {
        const QSocGoal goal = sampleGoal(100, 10);
        for (const QString &body :
             {QSocGoalPrompt::continuation(goal),
              QSocGoalPrompt::budgetLimit(goal),
              QSocGoalPrompt::objectiveUpdated(goal)}) {
            QVERIFY(body.startsWith(QStringLiteral("<goal_context>")));
            QVERIFY(body.endsWith(QStringLiteral("</goal_context>")));
        }
    }

    void objectiveWithoutTrailingNewlineIsHandled()
    {
        QSocGoal goal;
        goal.objective     = QStringLiteral("no trailing newline");
        const QString body = QSocGoalPrompt::continuation(goal);
        QVERIFY(body.contains(QStringLiteral("<objective>\nno trailing newline\n</objective>")));
    }

    void objectiveWithTrailingNewlineNoDouble()
    {
        QSocGoal goal;
        goal.objective     = QStringLiteral("two lines\nsecond\n");
        const QString body = QSocGoalPrompt::continuation(goal);
        QVERIFY(!body.contains(QStringLiteral("second\n\n</objective>")));
        QVERIFY(body.contains(QStringLiteral("second\n</objective>")));
    }
};

} // namespace

#include "test_qsocgoalprompt.moc"

QSOC_TEST_MAIN(Test)
