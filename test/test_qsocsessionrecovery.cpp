// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsessionrecovery.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QtTest>

namespace {

using json = nlohmann::json;

json toolCall(const char *id, const char *name = "read_file")
{
    return {
        {"id", id},
        {"type", "function"},
        {"function", {{"name", name}, {"arguments", "{}"}}},
    };
}

QSocSession::RunRecord runRecord(
    QSocSession::RunEvent event,
    const QString        &input  = QStringLiteral("continue the task"),
    const QString        &goalId = QString())
{
    return {
        .runId           = QStringLiteral("run-a"),
        .event           = event,
        .input           = input,
        .goalId          = goalId,
        .messageCount    = 0,
        .historyDigest   = QSocSession::historyDigest(json::array()),
        .inputReplaySafe = true,
        .contextPresent  = true,
        .registryModel   = true,
        .modelId         = QStringLiteral("model-primary"),
        .effortLevel     = QStringLiteral("high"),
        .reasoningModel  = QString(),
        .planMode        = false,
        .remoteMode      = true,
        .remoteName      = QStringLiteral("remote-a"),
        .projectRoot     = QStringLiteral("/workspace/project"),
        .workingDir      = QStringLiteral("/workspace/project"),
    };
}

QSocSession::RunRecord currentContext()
{
    return runRecord(QSocSession::RunEvent::Started);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void legacyAndTerminalRunsWait();
    void executionContextMustMatch_data();
    void executionContextMustMatch();
    void goalBindingMustMatch();
    void stagedInputReplaysOnlyWhenAbsent();
    void startedHistoryDeltaMustMatch();
    void checkpointContinuesValidHistory();
    void assistantTailContinuesOnlyActiveGoal();
    void checkpointClosesUnstartedToolBatch();
    void toolCheckpointRepairsMissingResults();
    void completedStartedToolsSkipRemainingAndResume();
    void persistedToolResultsResumeWithoutRepair();
    void repairIsIdempotent();
    void contextChangingToolCheckpointWaits_data();
    void contextChangingToolCheckpointWaits();
    void malformedHistoryFailsClosed();
    void newTurnRequiresClosedWellFormedHistory();
};

void Test::legacyAndTerminalRunsWait()
{
    const json messages = json::array({{{"role", "user"}, {"content", "task"}}});
    auto       plan     = QSocSessionRecovery::makePlan(std::nullopt, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    for (QSocSession::RunEvent event :
         {QSocSession::RunEvent::Completed,
          QSocSession::RunEvent::Error,
          QSocSession::RunEvent::Aborted,
          QSocSession::RunEvent::Invalid}) {
        plan = QSocSessionRecovery::makePlan(runRecord(event), messages, currentContext());
        QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    }

    auto running           = runRecord(QSocSession::RunEvent::Checkpoint);
    running.contextPresent = false;
    plan                   = QSocSessionRecovery::makePlan(running, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(plan.reason.contains(QStringLiteral("context"), Qt::CaseInsensitive));

    auto unavailableContext           = currentContext();
    unavailableContext.contextPresent = false;
    plan                              = QSocSessionRecovery::makePlan(
        runRecord(QSocSession::RunEvent::Checkpoint), messages, unavailableContext);
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(plan.reason.contains(QStringLiteral("context"), Qt::CaseInsensitive));

    auto emptyModelRun              = runRecord(QSocSession::RunEvent::Checkpoint);
    auto emptyModelContext          = currentContext();
    emptyModelRun.registryModel     = false;
    emptyModelContext.registryModel = false;
    emptyModelRun.modelId.clear();
    emptyModelContext.modelId.clear();
    plan = QSocSessionRecovery::makePlan(emptyModelRun, messages, emptyModelContext);
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);
}

void Test::executionContextMustMatch_data()
{
    QTest::addColumn<QString>("field");
    QTest::newRow("model") << QStringLiteral("model");
    QTest::newRow("model-kind") << QStringLiteral("model-kind");
    QTest::newRow("effort") << QStringLiteral("effort");
    QTest::newRow("reasoning-model") << QStringLiteral("reasoning-model");
    QTest::newRow("plan-mode") << QStringLiteral("plan-mode");
    QTest::newRow("remote-mode") << QStringLiteral("remote-mode");
    QTest::newRow("remote-name") << QStringLiteral("remote-name");
    QTest::newRow("project-root") << QStringLiteral("project-root");
    QTest::newRow("working-directory") << QStringLiteral("working-directory");
}

void Test::executionContextMustMatch()
{
    QFETCH(QString, field);
    auto context = currentContext();
    if (field == QStringLiteral("model")) {
        context.modelId = QStringLiteral("model-other");
    } else if (field == QStringLiteral("model-kind")) {
        context.registryModel = false;
    } else if (field == QStringLiteral("effort")) {
        context.effortLevel = QStringLiteral("medium");
    } else if (field == QStringLiteral("reasoning-model")) {
        context.reasoningModel = QStringLiteral("reasoning-other");
    } else if (field == QStringLiteral("plan-mode")) {
        context.planMode = true;
    } else if (field == QStringLiteral("remote-mode")) {
        context.remoteMode = false;
        context.remoteName.clear();
    } else if (field == QStringLiteral("remote-name")) {
        context.remoteName = QStringLiteral("remote-b");
    } else if (field == QStringLiteral("project-root")) {
        context.projectRoot = QStringLiteral("/workspace/other-root");
    } else {
        context.workingDir = QStringLiteral("/workspace/other");
    }

    const json messages = json::array({{{"role", "user"}, {"content", "task"}}});
    const auto plan     = QSocSessionRecovery::makePlan(
        runRecord(QSocSession::RunEvent::Checkpoint), messages, context);
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(plan.reason.contains(QStringLiteral("differs")));
}

void Test::goalBindingMustMatch()
{
    const json messages = json::array({{{"role", "user"}, {"content", "task"}}});

    auto run = runRecord(QSocSession::RunEvent::Checkpoint, {}, QStringLiteral("goal-a"));
    auto plan
        = QSocSessionRecovery::makePlan(run, messages, currentContext(), QStringLiteral("goal-a"));
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);

    plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    run  = runRecord(QSocSession::RunEvent::Checkpoint);
    plan = QSocSessionRecovery::makePlan(run, messages, currentContext(), QStringLiteral("goal-a"));
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
}

void Test::stagedInputReplaysOnlyWhenAbsent()
{
    auto run  = runRecord(QSocSession::RunEvent::Started, QStringLiteral("original input"));
    auto plan = QSocSessionRecovery::makePlan(run, json::array(), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ReplayInput);
    QCOMPARE(plan.input, QStringLiteral("original input"));

    run.inputReplaySafe = false;
    plan                = QSocSessionRecovery::makePlan(run, json::array(), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    run.inputReplaySafe = true;

    const json priorUser = json::array({{{"role", "user"}, {"content", "prior input"}}});
    run.messageCount     = 1;
    run.historyDigest    = QSocSession::historyDigest(priorUser);
    plan                 = QSocSessionRecovery::makePlan(run, priorUser, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ReplayInput);

    const json persisted = json::array(
        {{{"role", "assistant"}, {"content", "prior answer"}},
         {{"role", "user"}, {"content", "rewritten by hook"}}});
    run.messageCount  = 1;
    run.historyDigest = QSocSession::historyDigest(
        json::array({{{"role", "assistant"}, {"content", "prior answer"}}}));
    run.inputReplaySafe = false;
    plan                = QSocSessionRecovery::makePlan(run, persisted, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);

    run  = runRecord(QSocSession::RunEvent::Started, QString());
    plan = QSocSessionRecovery::makePlan(run, json::array(), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
}

void Test::startedHistoryDeltaMustMatch()
{
    auto run          = runRecord(QSocSession::RunEvent::Started, QStringLiteral("new input"));
    run.messageCount  = 1;
    run.historyDigest = QSocSession::historyDigest(
        json::array({{{"role", "assistant"}, {"content", "prior"}}}));

    const json tooMany = json::array(
        {{{"role", "assistant"}, {"content", "prior"}},
         {{"role", "user"}, {"content", "new"}},
         {{"role", "user"}, {"content", "unexpected"}}});
    auto plan = QSocSessionRecovery::makePlan(run, tooMany, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json wrongTail = json::array(
        {{{"role", "user"}, {"content", "prior"}},
         {{"role", "assistant"}, {"content", "unexpected"}}});
    plan = QSocSessionRecovery::makePlan(run, wrongTail, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json wrongPrefix = json::array(
        {{{"role", "user"}, {"content", "changed"}}, {{"role", "user"}, {"content", "new"}}});
    plan = QSocSessionRecovery::makePlan(run, wrongPrefix, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json changedTail = json::array(
        {{{"role", "assistant"}, {"content", "prior"}}, {{"role", "user"}, {"content", "changed"}}});
    plan = QSocSessionRecovery::makePlan(run, changedTail, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json malformedTail = json::array(
        {{{"role", "assistant"}, {"content", "prior"}},
         {{"role", "user"}, {"content", json::array()}}});
    plan = QSocSessionRecovery::makePlan(run, malformedTail, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json changedSameCount = json::array({{{"role", "user"}, {"content", "changed"}}});
    plan = QSocSessionRecovery::makePlan(run, changedSameCount, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    const json toolTail = json::array(
        {{{"role", "assistant"}, {"content", "prior"}},
         {{"role", "tool"}, {"tool_call_id", "unexpected"}, {"content", "done"}}});
    plan = QSocSessionRecovery::makePlan(run, toolTail, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    run.messageCount = -1;
    plan             = QSocSessionRecovery::makePlan(run, json::array(), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    run.messageCount = 0;
    run.historyDigest.clear();
    plan = QSocSessionRecovery::makePlan(run, json::array(), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
}

void Test::checkpointContinuesValidHistory()
{
    const auto run  = runRecord(QSocSession::RunEvent::Checkpoint);
    auto       plan = QSocSessionRecovery::makePlan(
        run, json::array({{{"role", "user"}, {"content", "task"}}}), currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);

    const json completedToolBatch = json::array(
        {{{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"}, {"tool_calls", json::array({toolCall("a")})}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}});
    plan = QSocSessionRecovery::makePlan(run, completedToolBatch, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);
}

void Test::assistantTailContinuesOnlyActiveGoal()
{
    const json messages = json::array(
        {{{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"}, {"content", "iteration complete"}}});

    auto run  = runRecord(QSocSession::RunEvent::Checkpoint);
    auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);

    run  = runRecord(QSocSession::RunEvent::Checkpoint, {}, QStringLiteral("goal-a"));
    plan = QSocSessionRecovery::makePlan(run, messages, currentContext(), QStringLiteral("goal-a"));
    QVERIFY(plan.action == QSocSessionRecovery::Action::ContinueGoal);
}

void Test::checkpointClosesUnstartedToolBatch()
{
    const json messages = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a"), toolCall("b")})}}});
    const auto run  = runRecord(QSocSession::RunEvent::Checkpoint);
    const auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());

    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);
    QCOMPARE(plan.messages.size(), json::size_type(3));
    QCOMPARE(plan.messages[1]["_qsoc_tool_state"].get<std::string>(), std::string("skipped"));
    QCOMPARE(plan.messages[2]["_qsoc_tool_state"].get<std::string>(), std::string("skipped"));

    json inconsistent = messages;
    inconsistent.push_back({{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}});
    const auto inconsistentPlan = QSocSessionRecovery::makePlan(run, inconsistent, currentContext());
    QVERIFY(inconsistentPlan.action == QSocSessionRecovery::Action::Wait);
}

void Test::toolCheckpointRepairsMissingResults()
{
    const json messages = json::array(
        {{{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"},
          {"tool_calls", json::array({toolCall("a"), toolCall("b"), toolCall("c")})}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}});
    auto run               = runRecord(QSocSession::RunEvent::ToolStarted);
    run.toolCallId         = QStringLiteral("b");
    run.startedToolCallIds = {QStringLiteral("a"), QStringLiteral("b")};

    const auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(plan.requiresUserInput);
    QCOMPARE(plan.messages.size(), json::size_type(5));
    QCOMPARE(plan.messages[3]["tool_call_id"].get<std::string>(), std::string("b"));
    QCOMPARE(plan.messages[4]["tool_call_id"].get<std::string>(), std::string("c"));
    QCOMPARE(plan.messages[3]["_qsoc_tool_state"].get<std::string>(), std::string("uncertain"));
    QCOMPARE(plan.messages[4]["_qsoc_tool_state"].get<std::string>(), std::string("skipped"));
    const QString uncertain = QString::fromStdString(plan.messages[3]["content"].get<std::string>());
    const QString notStarted = QString::fromStdString(
        plan.messages[4]["content"].get<std::string>());
    QVERIFY(uncertain.contains(QStringLiteral("Completion is uncertain")));
    QVERIFY(uncertain.contains(QStringLiteral("side effects may have occurred")));
    QVERIFY(notStarted.startsWith(QStringLiteral("Not executed")));
}

void Test::completedStartedToolsSkipRemainingAndResume()
{
    const json messages = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a"), toolCall("b")})}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}});
    auto run               = runRecord(QSocSession::RunEvent::ToolStarted);
    run.toolCallId         = QStringLiteral("a");
    run.startedToolCallIds = {QStringLiteral("a")};

    const auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);
    QVERIFY(!plan.requiresUserInput);
    QCOMPARE(plan.messages.size(), json::size_type(3));
    QCOMPARE(plan.messages.back()["tool_call_id"].get<std::string>(), std::string("b"));
    QCOMPARE(plan.messages.back()["_qsoc_tool_state"].get<std::string>(), std::string("skipped"));
}

void Test::persistedToolResultsResumeWithoutRepair()
{
    const json messages = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a")})}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}});
    auto run               = runRecord(QSocSession::RunEvent::ToolStarted);
    run.toolCallId         = QStringLiteral("a");
    run.startedToolCallIds = {QStringLiteral("a")};

    const auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::ResumeHistory);
    QVERIFY(plan.messages == messages);

    json queued = messages;
    queued.push_back({{"role", "user"}, {"content", "queued input"}});
    QVERIFY(
        QSocSessionRecovery::makePlan(run, queued, currentContext()).action
        == QSocSessionRecovery::Action::ResumeHistory);

    json attachment = messages;
    attachment.push_back(
        {{"role", "user"},
         {"content", json::array({{{"type", "text"}, {"text", "queued attachment"}}})}});
    QVERIFY(
        QSocSessionRecovery::makePlan(run, attachment, currentContext()).action
        == QSocSessionRecovery::Action::ResumeHistory);

    json impossible = messages;
    impossible.push_back({{"role", "assistant"}, {"content", "unexpected"}});
    QVERIFY(
        QSocSessionRecovery::makePlan(run, impossible, currentContext()).action
        == QSocSessionRecovery::Action::Wait);
}

void Test::repairIsIdempotent()
{
    const json messages = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a"), toolCall("b")})}}});
    auto run               = runRecord(QSocSession::RunEvent::ToolStarted);
    run.toolCallId         = QStringLiteral("a");
    run.startedToolCallIds = {QStringLiteral("a")};

    const auto first  = QSocSessionRecovery::makePlan(run, messages, currentContext());
    const auto second = QSocSessionRecovery::makePlan(run, first.messages, currentContext());
    QVERIFY(first.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(second.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(first.requiresUserInput);
    QVERIFY(second.requiresUserInput);
    QVERIFY(second.messages == first.messages);
}

void Test::contextChangingToolCheckpointWaits_data()
{
    QTest::addColumn<QString>("toolName");
    QTest::newRow("enter") << QStringLiteral("enter_plan_mode");
    QTest::newRow("exit") << QStringLiteral("exit_plan_mode");
    QTest::newRow("working-directory") << QStringLiteral("path_context");
}

void Test::contextChangingToolCheckpointWaits()
{
    QFETCH(QString, toolName);
    const json messages = json::array(
        {{{"role", "assistant"},
          {"tool_calls", json::array({toolCall("mode-change", toolName.toUtf8().constData())})}}});
    auto run               = runRecord(QSocSession::RunEvent::ToolStarted);
    run.toolCallId         = QStringLiteral("mode-change");
    run.startedToolCallIds = {QStringLiteral("mode-change")};

    const auto plan = QSocSessionRecovery::makePlan(run, messages, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    QVERIFY(plan.requiresUserInput);
    QCOMPARE(plan.messages.size(), json::size_type(2));
    QCOMPARE(plan.messages.back()["_qsoc_tool_state"].get<std::string>(), std::string("uncertain"));

    json completed = messages;
    completed.push_back({{"role", "tool"}, {"tool_call_id", "mode-change"}, {"content", "done"}});
    const auto completedPlan = QSocSessionRecovery::makePlan(run, completed, currentContext());
    QVERIFY(completedPlan.action == QSocSessionRecovery::Action::Wait);
}

void Test::malformedHistoryFailsClosed()
{
    auto checkpoint = runRecord(QSocSession::RunEvent::Checkpoint);
    for (const json &messages : {
             json::object(),
             json::array({json::object()}),
             json::array({{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "x"}}}),
             json::array(
                 {{{"role", "assistant"},
                   {"tool_calls", json::array({toolCall("dup"), toolCall("dup")})}}}),
         }) {
        const auto plan = QSocSessionRecovery::makePlan(checkpoint, messages, currentContext());
        QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
    }

    const json openBatch = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a")})}}});
    auto toolRun               = runRecord(QSocSession::RunEvent::ToolStarted);
    toolRun.toolCallId         = QStringLiteral("other");
    toolRun.startedToolCallIds = {QStringLiteral("other")};
    const auto plan = QSocSessionRecovery::makePlan(toolRun, openBatch, currentContext());
    QVERIFY(plan.action == QSocSessionRecovery::Action::Wait);
}

void Test::newTurnRequiresClosedWellFormedHistory()
{
    QVERIFY(QSocSessionRecovery::historySafeForNewTurn(json::array()));
    QVERIFY(
        QSocSessionRecovery::historySafeForNewTurn(
            json::array({{{"role", "user"}, {"content", "task"}}})));

    const json completeBatch = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a")})}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}});
    QVERIFY(QSocSessionRecovery::historySafeForNewTurn(completeBatch));

    const json openBatch = json::array(
        {{{"role", "assistant"}, {"tool_calls", json::array({toolCall("a")})}}});
    QVERIFY(!QSocSessionRecovery::historySafeForNewTurn(openBatch));
    QVERIFY(!QSocSessionRecovery::historySafeForNewTurn(
        json::array({{{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}})));
    QVERIFY(!QSocSessionRecovery::historySafeForNewTurn(
        json::array({{{"role", 7}, {"content", "bad"}}})));
    QVERIFY(!QSocSessionRecovery::historySafeForNewTurn(
        json::array({{{"role", "user"}, {"content", 7}}})));

    json invalidState                       = completeBatch;
    invalidState.back()["_qsoc_tool_state"] = "unknown";
    QVERIFY(!QSocSessionRecovery::historySafeForNewTurn(invalidState));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsessionrecovery.moc"
