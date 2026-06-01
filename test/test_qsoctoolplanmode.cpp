// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "qsoc_test.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    /* Build a registry with one read tool, one write tool, and the two
     * plan tools. Ownership stays with the registry (QObject parent). */
    static QSocToolRegistry *makeRegistry(QObject *parent)
    {
        auto *reg = new QSocToolRegistry(parent);
        reg->registerTool(new QSocToolFileRead(reg, nullptr, nullptr));
        reg->registerTool(new QSocToolFileWrite(reg, nullptr));
        reg->registerTool(new QSocToolEnterPlanMode(reg));
        reg->registerTool(new QSocToolExitPlanMode(reg));
        reg->registerTool(new QSocToolAskUser(reg, nullptr));
        return reg;
    }

private slots:
    void isReadOnlyClassification();
    void planGateBlocksWritesAllowsReads();
    void effectiveDefsHideWritesInPlanMode();
    void approvedPlanRoundTrips();
    void exitToolNullCallbackErrors();
    void exitToolRequiresPlanText();
    void exitToolApproveAndReject();
    void enterToolFiresCallback();
    void subAgentInheritsPlanModeGate();
};

void Test::isReadOnlyClassification()
{
    QSocToolEnterPlanMode enter;
    QSocToolExitPlanMode  exit;
    QSocToolFileRead      read(nullptr, nullptr, nullptr);
    QSocToolFileWrite     write(nullptr, nullptr);
    QVERIFY(enter.isReadOnly());
    QVERIFY(exit.isReadOnly());
    QVERIFY(read.isReadOnly());
    QVERIFY(!write.isReadOnly());
}

void Test::planGateBlocksWritesAllowsReads()
{
    QObject           owner;
    QSocToolRegistry *reg = makeRegistry(&owner);
    /* Resolve real names from the instances so the test does not hardcode
     * tool-name spellings. */
    QString rd;
    QString wr;
    for (const QString &n : reg->toolNames()) {
        QSocTool *t = reg->getTool(n);
        if (t->isReadOnly() && n != QStringLiteral("enter_plan_mode")
            && n != QStringLiteral("exit_plan_mode")) {
            rd = n;
        } else if (!t->isReadOnly()) {
            wr = n;
        }
    }
    QVERIFY(!rd.isEmpty());
    QVERIFY(!wr.isEmpty());

    QSocAgent agent(&owner, nullptr, reg, QSocAgentConfig{});

    /* Plan mode off: both read and write pass. */
    QVERIFY(agent.isToolAllowed(rd));
    QVERIFY(agent.isToolAllowed(wr));

    QSocAgentConfig cfg;
    cfg.planMode = true;
    agent.setConfig(cfg);

    QVERIFY(agent.isToolAllowed(rd));                                 /* read allowed */
    QVERIFY(!agent.isToolAllowed(wr));                                /* write blocked */
    QVERIFY(agent.isToolAllowed(QStringLiteral("bash")));             /* shell name-gate */
    QVERIFY(agent.isToolAllowed(QStringLiteral("agent")));            /* spawn name-gate */
    QVERIFY(agent.isToolAllowed(QStringLiteral("exit_plan_mode")));   /* visible in plan */
    QVERIFY(!agent.isToolAllowed(QStringLiteral("enter_plan_mode"))); /* hidden in plan */
    QVERIFY(agent.isToolAllowed(QStringLiteral("ask_user")));         /* clarify in plan */

    cfg.planMode = false;
    agent.setConfig(cfg);
    QVERIFY(agent.isToolAllowed(QStringLiteral("enter_plan_mode"))); /* visible off-plan */
    QVERIFY(!agent.isToolAllowed(QStringLiteral("exit_plan_mode"))); /* hidden off-plan */
}

void Test::effectiveDefsHideWritesInPlanMode()
{
    QObject           owner;
    QSocToolRegistry *reg = makeRegistry(&owner);
    QString           wr;
    for (const QString &n : reg->toolNames()) {
        if (!reg->getTool(n)->isReadOnly()) {
            wr = n;
        }
    }
    QSocAgent       agent(&owner, nullptr, reg, QSocAgentConfig{});
    QSocAgentConfig cfg;
    cfg.planMode = true;
    agent.setConfig(cfg);

    const json defs     = agent.getEffectiveToolDefinitions();
    bool       sawWrite = false;
    bool       sawExit  = false;
    for (const auto &def : defs) {
        if (!def.contains("function") || !def["function"].contains("name")) {
            continue;
        }
        const QString name = QString::fromStdString(def["function"]["name"].get<std::string>());
        if (name == wr) {
            sawWrite = true;
        }
        if (name == QStringLiteral("exit_plan_mode")) {
            sawExit = true;
        }
    }
    QVERIFY(!sawWrite); /* mutating tool hidden from the sent list */
    QVERIFY(sawExit);   /* exit tool offered */
}

void Test::approvedPlanRoundTrips()
{
    QObject   owner;
    QSocAgent agent(&owner, nullptr, makeRegistry(&owner), QSocAgentConfig{});
    agent.setApprovedPlan(QStringLiteral("plan A"));
    QCOMPARE(agent.approvedPlan(), QStringLiteral("plan A"));
    agent.setApprovedPlan(QStringLiteral("plan B")); /* single slot: overwrite */
    QCOMPARE(agent.approvedPlan(), QStringLiteral("plan B"));
}

void Test::exitToolNullCallbackErrors()
{
    QSocToolExitPlanMode exit;
    const QString        out = exit.execute(json{{"plan", "do the thing"}});
    QVERIFY(out.contains(QStringLiteral("error")));
}

void Test::exitToolRequiresPlanText()
{
    QSocToolExitPlanMode exit;
    exit.setCallback([](const QString &) { return QSocPlanApproval{true, QString()}; });
    const QString out = exit.execute(json{{"plan", "   "}});
    QVERIFY(out.contains(QStringLiteral("plan is required")));
}

void Test::exitToolApproveAndReject()
{
    QSocToolExitPlanMode exit;

    exit.setCallback([](const QString &) {
        QSocPlanApproval ok;
        ok.approved = true;
        return ok;
    });
    const QString approved = exit.execute(json{{"plan", "step 1\nstep 2"}});
    QVERIFY(approved.contains(QStringLiteral("approved")));
    QVERIFY(approved.contains(QStringLiteral("step 1")));

    exit.setCallback([](const QString &) {
        QSocPlanApproval no;
        no.approved = false;
        no.feedback = QStringLiteral("narrow the scope");
        return no;
    });
    const QString rejected = exit.execute(json{{"plan", "step 1"}});
    QVERIFY(rejected.contains(QStringLiteral("did not approve")));
    QVERIFY(rejected.contains(QStringLiteral("narrow the scope")));
}

void Test::enterToolFiresCallback()
{
    QSocToolEnterPlanMode enter;
    bool                  fired = false;
    enter.setCallback([&fired]() { fired = true; });
    const QString out = enter.execute(json::object());
    QVERIFY(fired);
    QVERIFY(!out.isEmpty());
}

void Test::subAgentInheritsPlanModeGate()
{
    /* A spawned child carries planMode (copied from the parent config) and
     * isSubAgent=true. No model needed: this is pure gate logic with a
     * null LLM service, the same pattern the sub-agent-guard tests use. */
    QObject           owner;
    QSocToolRegistry *reg = makeRegistry(&owner);
    QString           rd;
    QString           wr;
    for (const QString &n : reg->toolNames()) {
        QSocTool *t = reg->getTool(n);
        if (t->isReadOnly() && n != QStringLiteral("enter_plan_mode")
            && n != QStringLiteral("exit_plan_mode")) {
            rd = n;
        } else if (!t->isReadOnly()) {
            wr = n;
        }
    }
    QSocAgentConfig childCfg;
    childCfg.planMode   = true;
    childCfg.isSubAgent = true;
    QSocAgent child(&owner, nullptr, reg, childCfg);

    QVERIFY(child.isToolAllowed(rd));                                 /* read OK */
    QVERIFY(!child.isToolAllowed(wr));                                /* write blocked */
    QVERIFY(child.isToolAllowed(QStringLiteral("bash")));             /* shell judged */
    QVERIFY(!child.isToolAllowed(QStringLiteral("agent")));           /* no recursion */
    QVERIFY(!child.isToolAllowed(QStringLiteral("exit_plan_mode")));  /* child can't exit */
    QVERIFY(!child.isToolAllowed(QStringLiteral("enter_plan_mode"))); /* child can't enter */
}

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolplanmode.moc"
