// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagentcompletion.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

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

    /* Helper: create a file with an empty content at the given relative path */
    static void touchFile(const QString &dirPath, const QString &rel)
    {
        QString   abs = QDir(dirPath).filePath(rel);
        QFileInfo fileInfo(abs);
        QDir().mkpath(fileInfo.absolutePath());
        QFile file(abs);
        if (file.open(QIODevice::WriteOnly)) {
            file.close();
        }
    }

    void testFuzzyScoreExactBasename()
    {
        int score = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("src/foo.cpp"), QStringLiteral("foo.cpp"));
        QVERIFY(score > 0);
    }

    void testFuzzyScoreBasenameBeatsPath()
    {
        int exact = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("src/foo.cpp"), QStringLiteral("foo.cpp"));
        int subsequence = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("src/subdir/deep.cpp"), QStringLiteral("foo.cpp"));
        /* Exact basename match beats subsequence of some other file */
        QVERIFY(exact > subsequence);
    }

    void testFuzzyScoreNoMatchReturnsNegative()
    {
        int score = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("README.md"), QStringLiteral("xyzzy"));
        QCOMPARE(score, -1);
    }

    void testFuzzyScoreSubstringBeatsSubsequence()
    {
        int substr = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("src/hello.cpp"), QStringLiteral("hello"));
        int subseq = QAgentCompletionEngine::fuzzyScore(
            QStringLiteral("hpelelo.cpp"), QStringLiteral("hello"));
        QVERIFY(substr > subseq);
    }

    void testFuzzyScoreEmptyQuery()
    {
        int score = QAgentCompletionEngine::fuzzyScore(QStringLiteral("anything"), QString());
        QCOMPARE(score, 0);
    }

    void testScanFindsFiles()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("foo.cpp"));
        touchFile(tempDir.path(), QStringLiteral("src/bar.cpp"));
        touchFile(tempDir.path(), QStringLiteral("src/baz.h"));

        QAgentCompletionEngine engine;
        engine.scan(tempDir.path());
        QStringList files = engine.allFiles();

        QVERIFY(files.contains(QStringLiteral("foo.cpp")));
        QVERIFY(files.contains(QStringLiteral("src/bar.cpp")));
        QVERIFY(files.contains(QStringLiteral("src/baz.h")));
    }

    void testScanIgnoresBuildAndGit()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("src/foo.cpp"));
        touchFile(tempDir.path(), QStringLiteral("build/stale.o"));
        touchFile(tempDir.path(), QStringLiteral(".git/index"));
        touchFile(tempDir.path(), QStringLiteral(".qsoc/history"));

        QAgentCompletionEngine engine;
        engine.scan(tempDir.path());
        QStringList files = engine.allFiles();

        QVERIFY(files.contains(QStringLiteral("src/foo.cpp")));
        /* Ignored dirs should not appear */
        for (const QString &file : files) {
            QVERIFY(!file.startsWith(QStringLiteral("build/")));
            QVERIFY(!file.startsWith(QStringLiteral(".git/")));
            QVERIFY(!file.startsWith(QStringLiteral(".qsoc/")));
        }
    }

    void testScanIgnoresHiddenDirs()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("src/foo.cpp"));
        touchFile(tempDir.path(), QStringLiteral(".hidden/leak.cpp"));

        QAgentCompletionEngine engine;
        engine.scan(tempDir.path());
        QStringList files = engine.allFiles();

        QVERIFY(files.contains(QStringLiteral("src/foo.cpp")));
        for (const QString &file : files) {
            QVERIFY(!file.startsWith(QStringLiteral(".hidden/")));
        }
    }

    void testCompleteReturnsSortedMatches()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("README.md"));
        touchFile(tempDir.path(), QStringLiteral("src/foo.cpp"));
        touchFile(tempDir.path(), QStringLiteral("src/bar.cpp"));
        touchFile(tempDir.path(), QStringLiteral("docs/guide.md"));

        QAgentCompletionEngine engine;
        QStringList            results = engine.complete(tempDir.path(), QStringLiteral("foo"), 10);
        QVERIFY(!results.isEmpty());
        QCOMPARE(results.first(), QStringLiteral("src/foo.cpp"));
    }

    void testCompleteEmptyQueryReturnsAll()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("a.cpp"));
        touchFile(tempDir.path(), QStringLiteral("b.cpp"));
        touchFile(tempDir.path(), QStringLiteral("c.cpp"));

        QAgentCompletionEngine engine;
        QStringList            results = engine.complete(tempDir.path(), QString(), 10);
        QCOMPARE(static_cast<int>(results.size()), 3);
    }

    void testCompleteRespectsMaxResults()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        for (int i = 0; i < 20; i++) {
            touchFile(tempDir.path(), QStringLiteral("file%1.cpp").arg(i));
        }

        QAgentCompletionEngine engine;
        QStringList            results = engine.complete(tempDir.path(), QStringLiteral("file"), 5);
        QCOMPARE(static_cast<int>(results.size()), 5);
    }

    void testCustomIgnoreDirs()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("src/foo.cpp"));
        touchFile(tempDir.path(), QStringLiteral("vendor/third.cpp"));

        QAgentCompletionEngine engine;
        engine.setIgnoreDirs({QStringLiteral("vendor")});
        engine.scan(tempDir.path());
        QStringList files = engine.allFiles();

        QVERIFY(files.contains(QStringLiteral("src/foo.cpp")));
        for (const QString &file : files) {
            QVERIFY(!file.startsWith(QStringLiteral("vendor/")));
        }
    }

    void testCacheInvalidation()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        touchFile(tempDir.path(), QStringLiteral("first.cpp"));

        QAgentCompletionEngine engine;
        engine.scan(tempDir.path());
        QVERIFY(engine.allFiles().contains(QStringLiteral("first.cpp")));

        touchFile(tempDir.path(), QStringLiteral("second.cpp"));
        /* Without invalidation and within TTL, the new file won't show up */
        QStringList stale = engine.complete(tempDir.path(), QString(), 10);
        QVERIFY(!stale.contains(QStringLiteral("second.cpp")));

        engine.invalidateCache();
        QStringList fresh = engine.complete(tempDir.path(), QString(), 10);
        QVERIFY(fresh.contains(QStringLiteral("second.cpp")));
    }

    void testMissingProjectPathReturnsEmpty()
    {
        QAgentCompletionEngine engine;
        QStringList            results = engine.complete(
            QStringLiteral("/nonexistent/does/not/exist"), QStringLiteral("foo"), 10);
        QVERIFY(results.isEmpty());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentcompletion.moc"
