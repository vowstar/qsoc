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
};

QTEST_GUILESS_MAIN(Test)
#include "test_qsocsession.moc"
