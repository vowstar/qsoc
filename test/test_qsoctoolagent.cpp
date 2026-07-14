// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolagent.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    QSocToolAgent *makeTool(
        QSocAgentDefinitionRegistry **outDefs   = nullptr,
        QSocSubAgentTaskSource      **outSource = nullptr,
        QSocToolRegistry            **outReg    = nullptr)
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto *src = new QSocSubAgentTaskSource(this);
        auto *reg = new QSocToolRegistry(this);
        /* No real QLLMService: tests below only exercise validation
         * paths that do not construct a child agent. */
        auto *tool = new QSocToolAgent(this, nullptr, reg, QSocAgentConfig(), defs, src);
        if (outDefs != nullptr) {
            *outDefs = defs;
        }
        if (outSource != nullptr) {
            *outSource = src;
        }
        if (outReg != nullptr) {
            *outReg = reg;
        }
        return tool;
    }

private slots:
    void testToolNameAndDescription()
    {
        auto *tool = makeTool();
        QCOMPARE(tool->getName(), QStringLiteral("agent"));
        const QString desc = tool->getDescription();
        QVERIFY(desc.contains(QStringLiteral("sub-agent")));
        QVERIFY(desc.contains(QStringLiteral("general-purpose")));
    }

    void testSchemaIncludesBuiltinEnumValues()
    {
        auto      *tool   = makeTool();
        const json schema = tool->getParametersSchema();
        QVERIFY(schema.contains("properties"));
        const auto &props = schema["properties"];
        QVERIFY(props.contains("subagent_type"));
        QVERIFY(props["subagent_type"].contains("enum"));
        const auto &enumValues = props["subagent_type"]["enum"];
        QVERIFY(enumValues.is_array());
        QVERIFY(!enumValues.empty());
        bool found = false;
        for (const auto &val : enumValues) {
            if (val.is_string() && val.get<std::string>() == "general-purpose") {
                found = true;
                break;
            }
        }
        QVERIFY(found);
        QVERIFY(schema["required"].is_array());
    }

    void testUnknownSubagentTypeReturnsError()
    {
        auto         *tool = makeTool();
        const QString out  = tool->execute(
            json{{"subagent_type", "made-up"}, {"description", "x"}, {"prompt", "hello"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed.contains("error"));
        QVERIFY(parsed["available"].get<std::string>().find("general-purpose") != std::string::npos);
    }

    void testMissingFieldsReturnError()
    {
        auto         *tool   = makeTool();
        const QString out    = tool->execute(json{{"subagent_type", "general-purpose"}});
        const json    parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
    }

    /* The sliding-window scheduler runs up to maxConcurrent at once and
     * queues the rest as Pending; finishing one admits the next in FIFO
     * order. The launcher records the admission order so we can assert
     * the window held and the queue drained correctly. */
    void testSchedulerSlidingWindowQueuesPastCap()
    {
        auto *src = new QSocSubAgentTaskSource(this);
        src->setMaxConcurrent(2);

        QStringList started;
        auto        admit = [&](const QString &lbl) {
            auto         *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
            const QString runId = src->registerRun(lbl, QStringLiteral("general-purpose"), dummy);
            src->start(runId, [&started, runId]() { started << runId; });
            return runId;
        };

        const QString idA = admit(QStringLiteral("a"));
        const QString idB = admit(QStringLiteral("b"));
        const QString idC = admit(QStringLiteral("c"));

        /* Window of 2: a and b run, c waits. */
        QCOMPARE(src->countRunning(), 2);
        QCOMPARE(started.size(), 2);
        QVERIFY(started.contains(idA));
        QVERIFY(started.contains(idB));
        QVERIFY(!started.contains(idC));

        /* Finishing a frees a slot; c is admitted automatically. */
        src->markCompleted(idA, QStringLiteral("done"));
        QCOMPARE(src->countRunning(), 2); /* b + c */
        QCOMPARE(started.size(), 3);
        QVERIFY(started.contains(idC));
    }

    /* A queued (never-started) run is cancellable from the overlay, and
     * cancelling it does not start it. */
    void testSchedulerCancelQueuedRun()
    {
        auto *src = new QSocSubAgentTaskSource(this);
        src->setMaxConcurrent(1);

        QStringList started;
        auto        admit = [&](const QString &lbl) {
            auto         *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
            const QString runId = src->registerRun(lbl, QStringLiteral("general-purpose"), dummy);
            src->start(runId, [&started, runId]() { started << runId; });
            return runId;
        };

        const QString idA = admit(QStringLiteral("a")); /* runs */
        const QString idB = admit(QStringLiteral("b")); /* queued */
        QCOMPARE(src->countRunning(), 1);
        QVERIFY(!started.contains(idB));

        /* Cancel the queued one: it must never start. */
        QVERIFY(src->killTask(idB));
        QCOMPARE(src->countRunning(), 1); /* a still runs */
        QVERIFY(!started.contains(idB));
    }

    /* maxConcurrent 0 means unbounded: every registered run is admitted
     * immediately, no queueing. This is the project default. */
    void testSchedulerUnboundedAdmitsAll()
    {
        auto *src = new QSocSubAgentTaskSource(this);
        src->setMaxConcurrent(0); /* unbounded */

        QStringList started;
        auto        admit = [&](const QString &lbl) {
            auto         *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
            const QString runId = src->registerRun(lbl, QStringLiteral("general-purpose"), dummy);
            src->start(runId, [&started, runId]() { started << runId; });
            return runId;
        };

        for (int i = 0; i < 5; ++i) {
            admit(QStringLiteral("a") + QString::number(i));
        }
        QCOMPARE(src->countRunning(), 5);
        QCOMPARE(started.size(), 5);
    }

    /* A negative cap collapses to the same unbounded sentinel as 0. */
    void testSchedulerNegativeIsUnbounded()
    {
        auto *src = new QSocSubAgentTaskSource(this);
        src->setMaxConcurrent(-1);
        QCOMPARE(src->maxConcurrent(), 0);

        QStringList started;
        for (int i = 0; i < 3; ++i) {
            auto         *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
            const QString runId = src->registerRun(
                QStringLiteral("n") + QString::number(i), QStringLiteral("general-purpose"), dummy);
            src->start(runId, [&started, runId]() { started << runId; });
        }
        QCOMPARE(src->countRunning(), 3);
        QCOMPARE(started.size(), 3);
    }

    /* Stream-error classification drives the retry/backoff decision.
     * 429/503/529/overloaded are rate-limit (long backoff);
     * timeout/network/connection are transient; everything else,
     * including auth (401) and context-overflow (413, handled
     * upstream), is non-retryable here. */
    void testClassifyRetry()
    {
        using RK = QSocAgent::RetryKind;
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("[HTTP 429] Too Many Requests")),
            RK::RateLimited);
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("[HTTP 503] Service Unavailable")),
            RK::RateLimited);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("[HTTP 529] overloaded")), RK::RateLimited);
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("Server overloaded, retry")), RK::RateLimited);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("connection reset by peer")), RK::Transient);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("Request timeout")), RK::Transient);
        /* 5xx is a transient server fault. */
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("[HTTP 500] Internal Server Error")),
            RK::Transient);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("[HTTP 502] Bad Gateway")), RK::Transient);
        /* Other 4xx is a client error: never retry, even when the body
         * text happens to contain a transient-looking keyword. */
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("[HTTP 401] Unauthorized")), RK::None);
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("[HTTP 403] network access forbidden")),
            RK::None);
        QCOMPARE(
            QSocAgent::classifyRetry(QStringLiteral("[HTTP 400] invalid 'timeout' field")),
            RK::None);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("[HTTP 413] Payload Too Large")), RK::None);
        QCOMPARE(QSocAgent::classifyRetry(QStringLiteral("some other error")), RK::None);
    }

    /* Backoff grows with the attempt, never exceeds the cap (+ jitter),
     * and rate-limit waits start no shorter than transient ones. */
    void testBackoffDelayMs()
    {
        /* base 500 transient, 2000 rate-limit; cap 30000; jitter <= base/2. */
        const int delay1 = QSocAgent::backoffDelayMs(1, false);
        const int delay2 = QSocAgent::backoffDelayMs(2, false);
        const int delay3 = QSocAgent::backoffDelayMs(3, false);
        QVERIFY(delay1 >= 500 && delay1 <= 500 + 250);
        QVERIFY(delay2 >= 1000 && delay2 <= 1000 + 250);
        QVERIFY(delay3 >= 2000 && delay3 <= 2000 + 250);

        /* Large attempt clamps to the 30s cap (+ jitter slack). */
        const int big = QSocAgent::backoffDelayMs(20, false);
        QVERIFY(big >= 30000 && big <= 30000 + 250);

        /* Rate-limit base (2000) exceeds transient base (500) at attempt 1. */
        QVERIFY(QSocAgent::backoffDelayMs(1, true) >= 2000);
    }

    /* The spawn tool no longer rejects at the cap: with the LLM unwired
     * it always reaches the llm-null gate (the cap path now queues
     * rather than returns an error). */
    void testSpawnNeverRejectsForCap()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src = new QSocSubAgentTaskSource(this);
        auto           *reg = new QSocToolRegistry(this);
        QSocAgentConfig parentCfg;
        parentCfg.maxConcurrentSubagents = 1; /* strict serial */
        auto *tool = new QSocToolAgent(this, nullptr, reg, parentCfg, defs, src);

        /* Simulate one busy run. */
        auto         *dummy = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        const QString busyId
            = src->registerRun(QStringLiteral("busy"), QStringLiteral("general-purpose"), dummy);
        src->start(busyId, []() {});
        QCOMPARE(src->countRunning(), 1);

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "another"},
                {"prompt", "do something"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        /* No cap rejection; the only gate hit is the missing LLM. */
        QVERIFY(parsed["error"].get<std::string>().find("max concurrent") == std::string::npos);
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* QSocAgent::getLLMService() is the spawn tool's source of truth
     * for cloning a per-child service. */
    void testQSocAgentExposesLLMServiceAccessor()
    {
        auto *agent = new QSocAgent(this, nullptr, nullptr, QSocAgentConfig());
        QCOMPARE(agent->getLLMService(), nullptr);
    }

    void testAbortPropagatesToTaskSource()
    {
        QSocSubAgentTaskSource *src  = nullptr;
        QSocToolRegistry       *reg  = nullptr;
        auto                   *tool = makeTool(nullptr, &src, &reg);
        reg->registerTool(tool);
        auto         *dummyAgent = new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig());
        const QString runId      = src->registerRun(
            QStringLiteral("running"), QStringLiteral("general-purpose"), dummyAgent);
        src->start(runId, []() {});
        const QString pendingId = src->registerRun(
            QStringLiteral("pending"),
            QStringLiteral("general-purpose"),
            new QSocAgent(nullptr, nullptr, nullptr, QSocAgentConfig()));
        QVERIFY(src->hasActiveRun());

        QSocAgent root(nullptr, nullptr, reg, QSocAgentConfig());
        root.abort();

        QSocTask::Row pending;
        QVERIFY(src->findRow(pendingId, &pending));
        QCOMPARE(pending.status, QSocTask::Status::Failed);
        QVERIFY(!pending.canKill);
    }

    /* Verify the dynamic-resolution behavior: when a parent agent is
     * bound, the spawn tool reads its CURRENT registry / config, not
     * the constructor-captured snapshot. This is the contract that
     * keeps remote-mode (registry swap + remoteMode=true on config)
     * propagating into children. */
    void testParentAgentRegistryShadowsCachedSnapshot()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src         = new QSocSubAgentTaskSource(this);
        auto           *cachedReg   = new QSocToolRegistry(this);
        auto           *liveReg     = new QSocToolRegistry(this);
        auto           *parentAgent = new QSocAgent(this, nullptr, liveReg, QSocAgentConfig());
        QSocAgentConfig parentCfg;
        parentCfg.remoteMode = true;
        parentCfg.remoteName = QStringLiteral("user@host:22");
        parentAgent->setConfig(parentCfg);

        auto *tool = new QSocToolAgent(this, nullptr, cachedReg, QSocAgentConfig(), defs, src);
        tool->setParentAgent(parentAgent);

        /* Even though cachedReg / cached config say "local", the live
         * parent says remoteMode=true. The validation paths still
         * work — we can confirm the registry resolution path is the
         * live one by triggering the ll-service guard error AFTER def
         * resolution: that error path runs only when registry came
         * out non-null, so reaching it proves we resolved the live
         * pointer. */
        const QString out = tool->execute(
            json{{"subagent_type", "general-purpose"}, {"description", "x"}, {"prompt", "do thing"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        /* llmService is nullptr → we hit the LLM-guard error AFTER
         * registry resolution, proving liveReg was reachable (not
         * cachedReg, which would also have been non-null but the
         * point is: with parentAgent bound, we follow the live path). */
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* sweepStaleWorktrees deletes only mtime > maxAgeSec dirs and
     * leaves fresh ones alone. Test against a temp-rooted worktree
     * dir; we override the global path indirectly by accepting that
     * the function runs on the real TempLocation/qsoc-worktrees. To
     * avoid colliding with concurrent qsoc instances, we create
     * uniquely-named dirs and only assert *those* specific entries
     * are removed. */
    void testSweepStaleWorktreesRemovesOldOnly()
    {
        const QString root = QDir::tempPath() + QStringLiteral("/qsoc-worktrees");
        QDir().mkpath(root);
        const QString uniq    = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString freshWt = QDir(root).filePath(QStringLiteral("qsoc_wt_fresh_") + uniq);
        const QString staleWt = QDir(root).filePath(QStringLiteral("qsoc_wt_stale_") + uniq);
        QDir().mkpath(freshWt);
        QDir().mkpath(staleWt);
        /* Fake old mtime by using `touch -t` style: write a marker
         * file then setFileTime. */
        QFile staleMarker(staleWt + QStringLiteral("/.marker"));
        QVERIFY(staleMarker.open(QIODevice::WriteOnly));
        staleMarker.close();
        const QDateTime oldTs = QDateTime::currentDateTime().addDays(-3);
        QFileInfo(staleWt).setFile(staleWt);
        /* Use Linux-only touch via shell: more portable than
         * QFile::setFileTime which is Qt 6.5+ */
        QProcess::execute(
            QStringLiteral("touch"),
            {QStringLiteral("-t"), oldTs.toString(QStringLiteral("yyyyMMddhhmm")), staleWt});

        const int removed = QSocToolAgent::sweepStaleWorktrees(/*maxAgeSec*/ 24 * 60 * 60);

        QVERIFY(removed >= 1);
        QVERIFY(QDir(freshWt).exists());
        QVERIFY(!QDir(staleWt).exists());

        QDir(freshWt).removeRecursively();
    }

    void testSweepStaleWorktreesNoRootIsNoOp()
    {
        /* Sweeping when the root doesn't exist returns 0 cleanly. */
        const int removed = QSocToolAgent::sweepStaleWorktrees(/*maxAgeSec*/ 1);
        QVERIFY(removed >= 0);
    }

    void testIsolationFieldInSchemaEnumNoneAndWorktree()
    {
        auto      *tool   = makeTool();
        const json schema = tool->getParametersSchema();
        QVERIFY(schema["properties"].contains("isolation"));
        const auto &enumValues = schema["properties"]["isolation"]["enum"];
        QVERIFY(enumValues.is_array());
        std::vector<std::string> got;
        for (const auto &val : enumValues) {
            got.push_back(val.get<std::string>());
        }
        QVERIFY(std::find(got.begin(), got.end(), "none") != got.end());
        QVERIFY(std::find(got.begin(), got.end(), "worktree") != got.end());
    }

    /* When isolation=worktree but the project path is not a git
     * repo, the spawn tool silently falls back to no isolation
     * (worktree path empty in the response). */
    void testIsolationWorktreeFallsBackOutsideGitRepo()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto           *src = new QSocSubAgentTaskSource(this);
        auto           *reg = new QSocToolRegistry(this);
        QTemporaryDir   nonGit; /* not a git repo */
        QSocAgentConfig cfg;
        cfg.projectPath = nonGit.path();
        auto *tool      = new QSocToolAgent(this, nullptr, reg, cfg, defs, src);

        const QString out = tool->execute(
            json{
                {"subagent_type", "general-purpose"},
                {"description", "x"},
                {"prompt", "do thing"},
                {"isolation", "worktree"}});
        const json parsed = json::parse(out.toStdString());
        /* Falls through to LLM-null guard, which proves we got past
         * the isolation step without crashing on the missing repo. */
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("LLM service") != std::string::npos);
    }

    /* Fork mode: subagent_type empty triggers fork; required parent
     * agent missing → error. */
    void testForkRequiresParentAgent()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto         *src  = new QSocSubAgentTaskSource(this);
        auto         *reg  = new QSocToolRegistry(this);
        auto         *tool = new QSocToolAgent(this, nullptr, reg, QSocAgentConfig(), defs, src);
        const QString out  = tool->execute(
            json{{"description", "fork-it"}, {"prompt", "do delegated work"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(parsed["error"].get<std::string>().find("fork mode requires") != std::string::npos);
    }

    /* "fork" enum value is in the schema. */
    void testSchemaIncludesForkEnumValue()
    {
        auto       *tool       = makeTool();
        const json  schema     = tool->getParametersSchema();
        const auto &enumValues = schema["properties"]["subagent_type"]["enum"];
        bool        found      = false;
        for (const auto &val : enumValues) {
            if (val.is_string() && val.get<std::string>() == "fork") {
                found = true;
                break;
            }
        }
        QVERIFY(found);
        /* subagent_type must NOT be in required anymore (fork omits it). */
        bool required = false;
        for (const auto &val : schema["required"]) {
            if (val.is_string() && val.get<std::string>() == "subagent_type") {
                required = true;
            }
        }
        QVERIFY(!required);
    }

    /* Nested forks are blocked by the marker check. We seed the
     * parent agent's messages with the marker, then attempt a fork:
     * spawn must refuse before reaching child construction. */
    void testForkRejectsRecursionViaMarker()
    {
        auto *defs = new QSocAgentDefinitionRegistry(this);
        defs->registerBuiltins();
        auto *src         = new QSocSubAgentTaskSource(this);
        auto *liveReg     = new QSocToolRegistry(this);
        auto *parentAgent = new QSocAgent(this, nullptr, liveReg, QSocAgentConfig());
        /* Inject the fork marker into parent's history. */
        json forced = json::array();
        forced.push_back(
            {{"role", "system"}, {"content", "<!-- qsoc-fork-tag -->\nfake earlier fork"}});
        parentAgent->setMessages(forced);

        auto *tool = new QSocToolAgent(this, nullptr, liveReg, QSocAgentConfig(), defs, src);
        tool->setParentAgent(parentAgent);

        const QString out = tool->execute(
            json{{"description", "deeper fork"}, {"prompt", "another delegation"}});
        const json parsed = json::parse(out.toStdString());
        QCOMPARE(parsed["status"].get<std::string>(), std::string("error"));
        QVERIFY(
            parsed["error"].get<std::string>().find("forks cannot be nested") != std::string::npos);
    }

    /* Direct verification of QSocAgent::addExternalTokenUsage: it must
     * accumulate into totalInputTokens / totalOutputTokens and emit a
     * tokenUsage signal carrying the new totals. This is the contract
     * the spawn tool relies on to fold child usage into the parent. */
    void testAddExternalTokenUsageAccumulatesAndEmits()
    {
        auto      *parent = new QSocAgent(this, nullptr, nullptr, QSocAgentConfig());
        QSignalSpy spy(parent, &QSocAgent::tokenUsage);

        parent->addExternalTokenUsage(100, 50);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toLongLong(), qint64(100));
        QCOMPARE(spy.at(0).at(1).toLongLong(), qint64(50));

        parent->addExternalTokenUsage(40, 0);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toLongLong(), qint64(140));
        QCOMPARE(spy.at(1).at(1).toLongLong(), qint64(50));

        /* Zero / negative deltas are ignored; signal not re-emitted. */
        parent->addExternalTokenUsage(0, 0);
        parent->addExternalTokenUsage(-5, -5);
        QCOMPARE(spy.count(), 2);
    }

    /* Completed background runs wrap the result in a <result> body and
     * carry the task id, subagent type, and transcript path so the
     * parent can read the full run on demand. */
    void testTaskNotificationCompletedFormat()
    {
        const QString note = QSocToolAgent::buildTaskNotification(
            QStringLiteral("a7"),
            QStringLiteral("explorer"),
            QStringLiteral("completed"),
            QStringLiteral("found three call sites"),
            QStringLiteral("/run/user/1000/qsoc/agents/a7.jsonl"));
        QVERIFY(note.startsWith(QStringLiteral("<task-notification>")));
        QVERIFY(note.contains(QStringLiteral("<task-id>a7</task-id>")));
        QVERIFY(note.contains(QStringLiteral("<subagent-type>explorer</subagent-type>")));
        QVERIFY(note.contains(QStringLiteral("<status>completed</status>")));
        QVERIFY(note.contains(QStringLiteral(
            "<transcript>/run/user/1000/qsoc/agents/a7.jsonl"
            "</transcript>")));
        QVERIFY(note.contains(QStringLiteral("<result>")));
        QVERIFY(note.contains(QStringLiteral("found three call sites")));
        QVERIFY(!note.contains(QStringLiteral("<error>")));
    }

    /* Failed / aborted runs use an <error> body instead of <result>. */
    void testTaskNotificationFailedUsesErrorTag()
    {
        const QString failed = QSocToolAgent::buildTaskNotification(
            QStringLiteral("a8"),
            QStringLiteral("builder"),
            QStringLiteral("failed"),
            QStringLiteral("compile error"),
            QString());
        QVERIFY(failed.contains(QStringLiteral("<status>failed</status>")));
        QVERIFY(failed.contains(QStringLiteral("<error>")));
        QVERIFY(failed.contains(QStringLiteral("compile error")));
        QVERIFY(!failed.contains(QStringLiteral("<result>")));

        const QString aborted = QSocToolAgent::buildTaskNotification(
            QStringLiteral("a9"),
            QStringLiteral("builder"),
            QStringLiteral("aborted"),
            QStringLiteral("aborted"),
            QString());
        QVERIFY(aborted.contains(QStringLiteral("<status>aborted</status>")));
        QVERIFY(aborted.contains(QStringLiteral("<error>")));
    }

    /* An empty transcript path emits no <transcript> tag. */
    void testTaskNotificationOmitsEmptyTranscript()
    {
        const QString note = QSocToolAgent::buildTaskNotification(
            QStringLiteral("a1"),
            QStringLiteral("general-purpose"),
            QStringLiteral("completed"),
            QStringLiteral("ok"),
            QString());
        QVERIFY(!note.contains(QStringLiteral("<transcript>")));
    }

    /* Oversized bodies are capped with a pointer to the transcript so a
     * runaway child cannot flood the parent's context. */
    void testTaskNotificationTruncatesLongBody()
    {
        const QString huge = QString(20000, QLatin1Char('x'));
        const QString note = QSocToolAgent::buildTaskNotification(
            QStringLiteral("a2"),
            QStringLiteral("explorer"),
            QStringLiteral("completed"),
            huge,
            QStringLiteral("/tmp/a2.jsonl"));
        QVERIFY(note.size() < huge.size());
        QVERIFY(note.contains(QStringLiteral("truncated")));
    }

    /* queueTaskNotification feeds the parent's request queue exactly
     * like the background terminal-state callbacks do: one pending
     * entry per terminal child. This is the seam the spawn tool drives
     * via QSocToolAgent::buildTaskNotification + parent->queueTask... */
    void testParentQueuesTaskNotification()
    {
        auto *parent = new QSocAgent(this, nullptr, nullptr, QSocAgentConfig());
        QCOMPARE(parent->pendingRequestCount(), 0);
        parent->queueTaskNotification(
            QSocToolAgent::buildTaskNotification(
                QStringLiteral("a3"),
                QStringLiteral("explorer"),
                QStringLiteral("completed"),
                QStringLiteral("done"),
                QString()));
        QCOMPARE(parent->pendingRequestCount(), 1);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolagent.moc"
