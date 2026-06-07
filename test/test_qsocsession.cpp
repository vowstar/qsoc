// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsession.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

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

        /* Append a deliberately torn last line — simulates a crash mid-write. */
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        file.write("{\"type\":\"message\",\"role\":\"user\",\"content\":\"truncat");
        file.close();

        const json restored = QSocSession::loadMessages(path);
        QCOMPARE(static_cast<int>(restored.size()), 2);
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
        session.rewriteMessages(fresh);

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

        session.rewriteMessages(compacted);
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
        session.rewriteMessages(compacted);
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

        session.rewriteMessages(json::array());

        QVERIFY2(
            !QFile::exists(path),
            "/clear on a never-persisted session must not create the JSONL file");
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocsession.moc"
