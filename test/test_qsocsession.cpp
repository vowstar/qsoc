// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsession.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

QSocSession::RunRecord startedRun(
    const QString &runId, const QString &input, const QString &goalId = QString())
{
    return {
        .runId           = runId,
        .event           = QSocSession::RunEvent::Started,
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
        .remoteMode      = false,
        .remoteName      = QString(),
        .projectRoot     = QStringLiteral("/workspace/project"),
        .workingDir      = QStringLiteral("/workspace/project"),
    };
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testGenerateIdProducesValidUuid()
    {
        const QString id = QSocSession::generateId();
        QCOMPARE(id.size(), 36); /* 8-4-4-4-12 with hyphens */
        QVERIFY(id.contains('-'));
    }

    void testSessionsDirIsUnderProject()
    {
        const QString dir = QSocSession::sessionsDir(QStringLiteral("/tmp/proj"));
        QCOMPARE(dir, QStringLiteral("/tmp/proj/.qsoc/sessions"));
    }

    void testAppendAndLoadRoundtrip()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-04-11T00:00:00Z"));
        session.appendMessage({{"role", "user"}, {"content", "hello"}});
        session.appendMessage({{"role", "assistant"}, {"content", "hi"}});

        const json restored = QSocSession::loadMessages(path);
        QVERIFY(restored.is_array());
        QCOMPARE(static_cast<int>(restored.size()), 2);
        QCOMPARE(restored[0]["role"].get<std::string>(), std::string("user"));
        QCOMPARE(restored[0]["content"].get<std::string>(), std::string("hello"));
        QCOMPARE(restored[1]["role"].get<std::string>(), std::string("assistant"));
    }

    void testReadMetaLatestWins()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        /* A message flushes buffered meta; later meta lines override. */
        session.appendMessage({{"role", "user"}, {"content", "hi"}});
        session.appendMeta(QStringLiteral("last_memory_index"), QStringLiteral("2"));
        session.appendMeta(QStringLiteral("last_memory_index"), QStringLiteral("5"));

        QCOMPARE(
            QSocSession::readMeta(path, QStringLiteral("last_memory_index")), QStringLiteral("5"));
        QVERIFY(QSocSession::readMeta(path, QStringLiteral("absent")).isEmpty());
        QVERIFY(
            QSocSession::readMeta(QStringLiteral("/no/such/file"), QStringLiteral("x")).isEmpty());
    }

    void testLoadIgnoresPartialTail()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMessage({{"role", "user"}, {"content", "first"}});
        session.appendMessage({{"role", "assistant"}, {"content", "second"}});

        /* Append a deliberately torn last line to simulate a crash mid-write. */
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        file.write("{\"type\":\"message\",\"role\":\"user\",\"content\":\"truncat");
        file.close();

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(static_cast<int>(restored.size()), 2);
    }

    void testHistoryDigestSurvivesLoadNormalization()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        const json    messages = json::array({{{"role", "assistant"}, {"content", nullptr}}});
        QVERIFY(session.appendMessage(messages[0]));

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(restored[0]["content"].get<std::string>(), std::string());
        QCOMPARE(QSocSession::historyDigest(messages), QSocSession::historyDigest(restored));
    }

    void testReadInfoExtractsFirstPrompt()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-04-11T10:00:00.000Z"));
        session.appendMessage({{"role", "user"}, {"content", "describe the bus"}});
        session.appendMessage({{"role", "assistant"}, {"content", "the apb bus has..."}});
        session.appendMessage({{"role", "user"}, {"content", "now write rtl"}});

        const QSocSession::Info info = QSocSession::readInfo(path);
        QCOMPARE(info.id, id);
        QCOMPARE(info.firstPrompt, QStringLiteral("describe the bus"));
        QCOMPARE(info.messageCount, 3);
    }

    void testListAllReturnsNewestFirst()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString idA = QSocSession::generateId();
        const QString idB = QSocSession::generateId();
        const QString pathA
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idA + ".jsonl");
        const QString pathB
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idB + ".jsonl");

        QSocSession a(idA, pathA);
        a.appendMessage({{"role", "user"}, {"content", "old session"}});

        QTest::qWait(50);

        QSocSession b(idB, pathB);
        b.appendMessage({{"role", "user"}, {"content", "new session"}});

        const auto sessions = QSocSession::listAll(tempDir.path());
        QCOMPARE(static_cast<int>(sessions.size()), 2);
        QCOMPARE(sessions.first().id, idB);
        QCOMPARE(sessions.last().id, idA);
    }

    void testResolveIdMatchesPrefix()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString idA = QStringLiteral("9c1e7f7d-2e3a-4f4f-a8e3-d8c7f3e1d4a1");
        const QString idB = QStringLiteral("8b2c6e7d-1c2b-3e4d-9f8a-c7b6f5e4d3c2");
        const QString pathA
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idA + ".jsonl");
        const QString pathB
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idB + ".jsonl");
        QSocSession a(idA, pathA);
        a.appendMessage({{"role", "user"}, {"content", "a"}});
        QSocSession b(idB, pathB);
        b.appendMessage({{"role", "user"}, {"content", "b"}});

        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("9c1e")), idA);
        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("8b2c")), idB);
        /* Exact full id wins immediately */
        QCOMPARE(QSocSession::resolveId(tempDir.path(), idB), idB);
    }

    void testResolveIdEmptyOnNoMatch()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("ffff")), QString());
    }

    void testResolveIdMatchesTitleAndBranch()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString idA = QStringLiteral("11111111-aaaa-bbbb-cccc-000000000001");
        const QString idB = QStringLiteral("22222222-aaaa-bbbb-cccc-000000000002");
        const QString pathA
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idA + ".jsonl");
        const QString pathB
            = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(idB + ".jsonl");

        QSocSession a(idA, pathA);
        a.appendMeta(QStringLiteral("title"), QStringLiteral("Refactor APB bridge"));
        a.appendMessage({{"role", "user"}, {"content", "a"}});
        QSocSession b(idB, pathB);
        b.appendMeta(QStringLiteral("branch"), QStringLiteral("experiment-dma"));
        b.appendMessage({{"role", "user"}, {"content", "b"}});

        /* Case-insensitive substring of a title resolves to its session. */
        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("apb")), idA);
        /* A branch label substring resolves too. */
        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("dma")), idB);
        /* A token matching nothing stays empty. */
        QCOMPARE(QSocSession::resolveId(tempDir.path(), QStringLiteral("zzz")), QString());
    }

    void testRewriteMessagesTruncates()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMessage({{"role", "user"}, {"content", "first"}});
        session.appendMessage({{"role", "assistant"}, {"content", "second"}});
        QCOMPARE(static_cast<int>(QSocSession::loadMessages(path).size()), 2);

        json fresh = json::array();
        fresh.push_back({{"role", "user"}, {"content", "only"}});
        QVERIFY(session.rewriteMessages(fresh));

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(static_cast<int>(restored.size()), 1);
        QCOMPARE(restored[0]["content"].get<std::string>(), std::string("only"));
    }

    void testCompactionRewritePreservesMetaAndHistory()
    {
        /* Mirrors persistCompactedSession: after compaction shrinks the
         * message array, the JSONL is rewritten to the compacted messages
         * and meta is re-emitted (rewriteMessages truncates it). On reload
         * the history must be the compacted form, created preserved, and
         * last_memory_index reset to the new size. */
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-05-01T08:00:00.000Z"));
        session.appendMeta(QStringLiteral("cwd"), tempDir.path());
        for (int i = 0; i < 10; i++) {
            session.appendMessage(
                {{"role", "user"}, {"content", QString("msg %1").arg(i).toStdString()}});
        }
        QCOMPARE(static_cast<int>(QSocSession::loadMessages(path).size()), 10);

        /* Simulate compaction output: summary + last 2 verbatim. */
        const QSocSession::Info origInfo  = QSocSession::readInfo(path);
        json                    compacted = json::array();
        compacted.push_back({{"role", "user"}, {"content", "[Conversation Summary]\nrolled up"}});
        compacted.push_back({{"role", "user"}, {"content", "msg 8"}});
        compacted.push_back({{"role", "user"}, {"content", "msg 9"}});

        QVERIFY(session.rewriteMessages(compacted));
        session.appendMeta(QStringLiteral("created"), origInfo.createdAt.toString(Qt::ISODateWithMs));
        session.appendMeta(QStringLiteral("cwd"), tempDir.path());
        const int newSize = static_cast<int>(compacted.size());
        session.appendMeta(QStringLiteral("last_memory_index"), QString::number(newSize));

        /* Reload: only the compacted history survives, no stale pre-compact
         * lines, summary first. */
        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(static_cast<int>(restored.size()), 3);
        QVERIFY(
            QString::fromStdString(restored[0]["content"].get<std::string>())
                .startsWith("[Conversation Summary]"));
        QCOMPARE(restored[2]["content"].get<std::string>(), std::string("msg 9"));

        /* Meta survived the rewrite. */
        const QSocSession::Info info = QSocSession::readInfo(path);
        QCOMPARE(
            info.createdAt.toUTC().toString(Qt::ISODateWithMs),
            QStringLiteral("2026-05-01T08:00:00.000Z"));
        QCOMPARE(
            QSocSession::readMeta(path, QStringLiteral("last_memory_index")),
            QString::number(newSize));
    }

    void testReadMetasSinglePass()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMessage({{"role", "user"}, {"content", "hi"}});
        session.appendMeta(QStringLiteral("title"), QStringLiteral("first"));
        session.appendMeta(QStringLiteral("title"), QStringLiteral("latest")); /* latest wins */
        session.appendMeta(QStringLiteral("auto_title"), QStringLiteral("auto"));

        const QMap<QString, QString> got = QSocSession::readMetas(
            path, {QStringLiteral("title"), QStringLiteral("auto_title"), QStringLiteral("absent")});
        QCOMPARE(got.value(QStringLiteral("title")), QStringLiteral("latest"));
        QCOMPARE(got.value(QStringLiteral("auto_title")), QStringLiteral("auto"));
        QVERIFY(!got.contains(QStringLiteral("absent")));
    }

    void testAutoTitlePreservedAcrossRewrite()
    {
        /* Mirrors persistCompactedSession's meta handling: an auto title must
         * survive a compaction rewrite as auto_title, not be promoted to a
         * manual title (which would block future regeneration). */
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);

        session.appendMeta(QStringLiteral("auto_title"), QStringLiteral("Wire Up Reset"));
        session.appendMessage({{"role", "user"}, {"content", "hi"}});

        /* Re-emit only the keys the raw metas carried (manual title empty). */
        const QString manualTitle = QSocSession::readMeta(path, QStringLiteral("title"));
        const QString autoTitle   = QSocSession::readMeta(path, QStringLiteral("auto_title"));
        QVERIFY(manualTitle.isEmpty());
        QCOMPARE(autoTitle, QStringLiteral("Wire Up Reset"));

        json compacted = json::array();
        compacted.push_back({{"role", "user"}, {"content", "[Conversation Summary]\nx"}});
        QVERIFY(session.rewriteMessages(compacted));
        if (!autoTitle.isEmpty()) {
            session.appendMeta(QStringLiteral("auto_title"), autoTitle);
        }

        /* After the rewrite: auto_title persists, no manual title appeared,
         * and readInfo still surfaces it via the fallback. */
        QCOMPARE(
            QSocSession::readMeta(path, QStringLiteral("auto_title")),
            QStringLiteral("Wire Up Reset"));
        QVERIFY(QSocSession::readMeta(path, QStringLiteral("title")).isEmpty());
        QCOMPARE(QSocSession::readInfo(path).title, QStringLiteral("Wire Up Reset"));
    }

    void testMetaOnlySessionLeavesNoFile()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        {
            QSocSession session(id, path);
            session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-04-27T00:00:00Z"));
            session.appendMeta(QStringLiteral("cwd"), tempDir.path());
        } /* destructor: nothing should land on disk */

        QVERIFY2(!QFile::exists(path), "meta-only session must not create the JSONL file");
        QCOMPARE(QSocSession::listAll(tempDir.path()).size(), qsizetype(0));
    }

    void testFirstMessageFlushesBufferedMeta()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-04-27T00:00:00Z"));
        session.appendMeta(QStringLiteral("cwd"), tempDir.path());
        QVERIFY(!QFile::exists(path));

        session.appendMessage({{"role", "user"}, {"content", "kicked off"}});
        QVERIFY(QFile::exists(path));

        const auto info = QSocSession::readInfo(path);
        QCOMPARE(info.firstPrompt, QStringLiteral("kicked off"));
        QCOMPARE(
            info.createdAt.toUTC().toString(Qt::ISODateWithMs),
            QStringLiteral("2026-04-27T00:00:00.000Z"));
        QCOMPARE(info.messageCount, 1);
    }

    void testClearOnEmptySessionStaysOffDisk()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-04-27T00:00:00Z"));

        QVERIFY(session.rewriteMessages(json::array()));

        QVERIFY2(
            !QFile::exists(path),
            "/clear on a never-persisted session must not create the JSONL file");
    }

    void testAppendFailureIsReported()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QSocSession session(QStringLiteral("blocked"), tempDir.path());
        QVERIFY(!session.appendMeta(QStringLiteral("created"), QStringLiteral("value")));
        QVERIFY(!session.appendMessage({{"role", "user"}, {"content", "cannot write here"}}));
        QVERIFY(!session.rewriteMessages(json::array({{{"role", "user"}, {"content", "x"}}})));
    }

    void testRecoveryClaimLifecycleAndPathIsolation()
    {
        QStandardPaths::setTestModeEnabled(true);
        const QString suffix    = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString runId     = QStringLiteral("run-") + suffix;
        const QString traversal = QStringLiteral("../../outside-") + suffix;
        const auto    cleanup   = qScopeGuard([&]() {
            QSocSession::removeRecoveryClaim(runId);
            QSocSession::removeRecoveryClaim(traversal);
            QStandardPaths::setTestModeEnabled(false);
        });

        const QString dataRoot = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
        QVERIFY(!dataRoot.isEmpty());

        QVERIFY(!QSocSession::hasRecoveryClaim(runId));
        QVERIFY(!QSocSession::createRecoveryClaim(QString()));
        QVERIFY(!QSocSession::hasRecoveryClaim(QStringLiteral("   ")));
        QVERIFY(!QSocSession::removeRecoveryClaim(QString()));
        QVERIFY(QSocSession::createRecoveryClaim(runId));
        QVERIFY(QSocSession::hasRecoveryClaim(runId));
        QVERIFY(!QSocSession::hasRecoveryClaim(QStringLiteral("missing-") + suffix));
        QVERIFY(QSocSession::removeRecoveryClaim(runId));
        QVERIFY(!QSocSession::hasRecoveryClaim(runId));

        QVERIFY(QSocSession::createRecoveryClaim(traversal));
        QVERIFY(QSocSession::hasRecoveryClaim(traversal));
        const QByteArray claimName
            = QCryptographicHash::hash(traversal.toUtf8(), QCryptographicHash::Sha256).toHex();
        const QDir claimDir(QDir(dataRoot).filePath(QStringLiteral("recovery-claims")));
        QVERIFY(QFileInfo(claimDir.filePath(QString::fromLatin1(claimName) + ".claim")).isFile());
        QVERIFY(!QFile::exists(QDir(dataRoot).filePath(traversal)));
        QVERIFY(QSocSession::removeRecoveryClaim(traversal));
    }

    void testLatestRunFoldsLifecycleRecords()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(
            session.appendMeta(QStringLiteral("created"), QStringLiteral("2026-07-22T00:00:00Z")));
        auto started = startedRun(
            QStringLiteral("run-a"), QStringLiteral("finish the task"), QStringLiteral("goal-a"));
        started.reasoningModel = QStringLiteral("model-reasoning");
        started.planMode       = true;
        started.remoteMode     = true;
        started.remoteName     = QStringLiteral("remote-a");
        started.projectRoot    = QStringLiteral("/workspace/remote-root");
        started.workingDir     = QStringLiteral("/workspace/remote-project");
        QVERIFY(session.appendRun(started));
        QVERIFY(QFile::exists(path));
        QVERIFY(session.appendRun(
            {.runId = QStringLiteral("run-a"), .event = QSocSession::RunEvent::Checkpoint}));
        QVERIFY(session.appendRun(
            {.runId      = QStringLiteral("run-a"),
             .event      = QSocSession::RunEvent::ToolStarted,
             .toolCallId = QStringLiteral("call-a")}));
        QVERIFY(session.appendRun(
            {.runId      = QStringLiteral("run-a"),
             .event      = QSocSession::RunEvent::ToolStarted,
             .toolCallId = QStringLiteral("call-b")}));

        auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::ToolStarted);
        QCOMPARE(latest->runId, QStringLiteral("run-a"));
        QCOMPARE(latest->input, QStringLiteral("finish the task"));
        QCOMPARE(latest->goalId, QStringLiteral("goal-a"));
        QCOMPARE(latest->toolCallId, QStringLiteral("call-b"));
        QCOMPARE(latest->messageCount, 0);
        QCOMPARE(latest->historyDigest, QSocSession::historyDigest(json::array()));
        QVERIFY(latest->inputReplaySafe);
        QVERIFY(latest->contextPresent);
        QVERIFY(latest->registryModel);
        QCOMPARE(latest->modelId, QStringLiteral("model-primary"));
        QCOMPARE(latest->effortLevel, QStringLiteral("high"));
        QCOMPARE(latest->reasoningModel, QStringLiteral("model-reasoning"));
        QVERIFY(latest->planMode);
        QVERIFY(latest->remoteMode);
        QCOMPARE(latest->remoteName, QStringLiteral("remote-a"));
        QCOMPARE(latest->projectRoot, QStringLiteral("/workspace/remote-root"));
        QCOMPARE(latest->workingDir, QStringLiteral("/workspace/remote-project"));
        QCOMPARE(
            latest->startedToolCallIds,
            QStringList({QStringLiteral("call-a"), QStringLiteral("call-b")}));

        QVERIFY(session.appendRun(
            {.runId = QStringLiteral("run-a"), .event = QSocSession::RunEvent::Completed}));
        latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Completed);
        QVERIFY(latest->toolCallId.isEmpty());
        QCOMPARE(latest->input, QStringLiteral("finish the task"));
        QCOMPARE(latest->goalId, QStringLiteral("goal-a"));
        QVERIFY(latest->contextPresent);
        QCOMPARE(latest->modelId, QStringLiteral("model-primary"));
    }

    void testLatestRunUsesNewestRun()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("old"), QStringLiteral("old input"))));
        QVERIFY(session.appendRun(
            {.runId = QStringLiteral("old"), .event = QSocSession::RunEvent::Aborted}));
        QVERIFY(session.appendRun(startedRun(QStringLiteral("new"), QStringLiteral("new input"))));

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Started);
        QCOMPARE(latest->runId, QStringLiteral("new"));
        QCOMPARE(latest->input, QStringLiteral("new input"));
    }

    void testLatestRunRejectsTornTail()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"))));
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        QCOMPARE(file.write("{\"type\":\"run\""), qint64(13));
        file.close();

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Invalid);
    }

    void testLatestRunRejectsMalformedRecord()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"))));
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        const QByteArray malformed = QByteArrayLiteral(
            "{\"type\":\"run\",\"run_id\":\"run-a\",\"event\":\"unknown\"}\n");
        QCOMPARE(file.write(malformed), qint64(malformed.size()));
        file.close();

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Invalid);
    }

    void testLatestRunRejectsNonStringType()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"))));
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        const QByteArray malformed = QByteArrayLiteral("{\"type\":123}\n");
        QCOMPARE(file.write(malformed), qint64(malformed.size()));
        file.close();

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Invalid);
    }

    void testStartedRunRequiresCompleteContext()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(!session.appendRun(
            {.runId = QStringLiteral("run-a"),
             .event = QSocSession::RunEvent::Started,
             .input = QStringLiteral("resume me")}));

        auto invalid         = startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"));
        invalid.messageCount = -1;
        QVERIFY(!session.appendRun(invalid));
        invalid.messageCount = 0;
        invalid.historyDigest.clear();
        QVERIFY(!session.appendRun(invalid));
        invalid.historyDigest = QSocSession::historyDigest(json::array());
        invalid.remoteMode    = true;
        QVERIFY(!session.appendRun(invalid));
        invalid.remoteName = QStringLiteral("remote-a");
        invalid.workingDir.clear();
        QVERIFY(!session.appendRun(invalid));
        invalid.event = QSocSession::RunEvent::Checkpoint;
        QVERIFY(!session.appendRun(invalid));
        invalid.event      = QSocSession::RunEvent::Started;
        invalid.workingDir = QStringLiteral("/workspace/project");
        invalid.projectRoot.clear();
        QVERIFY(!session.appendRun(invalid));
        invalid.projectRoot = QStringLiteral("/workspace/project");
        invalid.modelId.clear();
        QVERIFY(!session.appendRun(invalid));
        invalid.registryModel = false;
        QVERIFY(session.appendRun(invalid));
        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(!latest->registryModel);
        QVERIFY(latest->modelId.isEmpty());
        QCOMPARE(latest->projectRoot, QStringLiteral("/workspace/project"));
    }

    void testLatestRunAcceptsLegacyContextAsAbsent()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray line = QByteArray::fromStdString(
            json({{"type", "run"},
                  {"run_id", "legacy-run"},
                  {"event", "started"},
                  {"input", "resume me"}})
                .dump()
            + "\n");
        QCOMPARE(file.write(line), qint64(line.size()));
        file.close();

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Started);
        QVERIFY(!latest->contextPresent);
        QCOMPARE(latest->messageCount, -1);
        QVERIFY(latest->historyDigest.isEmpty());
        QVERIFY(!latest->inputReplaySafe);
        QVERIFY(!latest->registryModel);
        QVERIFY(latest->modelId.isEmpty());
        QVERIFY(latest->projectRoot.isEmpty());
        QVERIFY(latest->workingDir.isEmpty());
    }

    void testLatestRunUpdatesContextAtCheckpoint()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"))));

        QSocSession::RunRecord checkpoint{
            .runId          = QStringLiteral("run-a"),
            .event          = QSocSession::RunEvent::Checkpoint,
            .contextPresent = true,
            .registryModel  = true,
            .modelId        = QStringLiteral("model-updated"),
            .effortLevel    = QStringLiteral("medium"),
            .reasoningModel = QStringLiteral("reasoning-updated"),
            .planMode       = true,
            .remoteMode     = true,
            .remoteName     = QStringLiteral("remote-b"),
            .projectRoot    = QStringLiteral("/workspace/updated-root"),
            .workingDir     = QStringLiteral("/workspace/updated"),
        };
        QVERIFY(session.appendRun(checkpoint));
        QVERIFY(session.appendRun(
            {.runId      = QStringLiteral("run-a"),
             .event      = QSocSession::RunEvent::ToolStarted,
             .toolCallId = QStringLiteral("call-a")}));

        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::ToolStarted);
        QVERIFY(latest->contextPresent);
        QVERIFY(latest->registryModel);
        QCOMPARE(latest->modelId, QStringLiteral("model-updated"));
        QCOMPARE(latest->effortLevel, QStringLiteral("medium"));
        QCOMPARE(latest->reasoningModel, QStringLiteral("reasoning-updated"));
        QVERIFY(latest->planMode);
        QVERIFY(latest->remoteMode);
        QCOMPARE(latest->remoteName, QStringLiteral("remote-b"));
        QCOMPARE(latest->projectRoot, QStringLiteral("/workspace/updated-root"));
        QCOMPARE(latest->workingDir, QStringLiteral("/workspace/updated"));
    }

    void testLatestRunRejectsMalformedContext()
    {
        json validContext = {
            {"model_id", "model-primary"},
            {"registry_model", true},
            {"effort_level", "high"},
            {"reasoning_model", ""},
            {"plan_mode", false},
            {"remote_mode", false},
            {"remote_name", ""},
            {"project_root", "/workspace/project"},
            {"working_dir", "/workspace/project"},
        };
        json missingField = validContext;
        missingField.erase("effort_level");
        json wrongType                   = validContext;
        wrongType["plan_mode"]           = "false";
        json emptyDirectory              = validContext;
        emptyDirectory["working_dir"]    = "";
        json emptyProjectRoot            = validContext;
        emptyProjectRoot["project_root"] = "";
        json missingProjectRoot          = validContext;
        missingProjectRoot.erase("project_root");
        json missingModelKind = validContext;
        missingModelKind.erase("registry_model");
        json remoteMismatch           = validContext;
        remoteMismatch["remote_mode"] = true;

        const json malformedContexts = json::array(
            {missingField,
             wrongType,
             emptyDirectory,
             emptyProjectRoot,
             missingProjectRoot,
             missingModelKind,
             remoteMismatch});
        for (const json &context : malformedContexts) {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString path = QDir(tempDir.path()).filePath(QStringLiteral("session.jsonl"));
            QFile         file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            const QByteArray line = QByteArray::fromStdString(
                json({{"type", "run"},
                      {"run_id", "run-a"},
                      {"event", "started"},
                      {"input", "resume me"},
                      {"context", context}})
                    .dump()
                + "\n");
            QCOMPARE(file.write(line), qint64(line.size()));
            file.close();

            const auto latest = QSocSession::latestRun(path);
            QVERIFY(latest.has_value());
            QVERIFY(latest->event == QSocSession::RunEvent::Invalid);
        }

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = QDir(tempDir.path()).filePath(QStringLiteral("session.jsonl"));
        QSocSession   session(QStringLiteral("run-a"), path);
        QVERIFY(session.appendRun(startedRun(QStringLiteral("run-a"), QStringLiteral("resume me"))));
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        const QByteArray line = QByteArray::fromStdString(
            json({{"type", "run"},
                  {"run_id", "run-a"},
                  {"event", "checkpoint"},
                  {"context", missingField}})
                .dump()
            + "\n");
        QCOMPARE(file.write(line), qint64(line.size()));
        file.close();
        const auto latest = QSocSession::latestRun(path);
        QVERIFY(latest.has_value());
        QVERIFY(latest->event == QSocSession::RunEvent::Invalid);
    }

    void testSnapshotReplacesEarlierMessages()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendMessage({{"role", "user"}, {"content", "old request"}}));
        QVERIFY(session.appendMessage({{"role", "assistant"}, {"content", "old response"}}));

        const json snapshot = json::array(
            {{{"role", "user"}, {"content", "summary"}},
             {{"role", "assistant"}, {"content", "recent"}}});
        QVERIFY(session.appendSnapshot(snapshot));
        QVERIFY(session.appendMessage({{"role", "user"}, {"content", "new request"}}));

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(restored.size(), json::size_type(3));
        QCOMPARE(restored[0]["content"].get<std::string>(), std::string("summary"));
        QCOMPARE(restored[1]["content"].get<std::string>(), std::string("recent"));
        QCOMPARE(restored[2]["content"].get<std::string>(), std::string("new request"));
        QCOMPARE(QSocSession::readInfo(path).messageCount, 3);
    }

    void testTornSnapshotKeepsEarlierMessages()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString id   = QSocSession::generateId();
        const QString path = QDir(QSocSession::sessionsDir(tempDir.path())).filePath(id + ".jsonl");
        QSocSession   session(id, path);
        QVERIFY(session.appendMessage({{"role", "user"}, {"content", "preserved"}}));

        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        const QByteArray torn = QByteArrayLiteral("{\"type\":\"snapshot\",\"messages\":[");
        QCOMPARE(file.write(torn), qint64(torn.size()));
        file.close();

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(restored.size(), json::size_type(1));
        QCOMPARE(restored[0]["content"].get<std::string>(), std::string("preserved"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocsession.moc"
