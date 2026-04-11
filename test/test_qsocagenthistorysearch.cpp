// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagenthistorysearch.h"
#include "qsoc_test.h"

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

    void testEmptyHistoryNoMatch()
    {
        QStringList         history;
        QAgentHistorySearch search(history);
        auto                result = search.findNext(QStringLiteral("foo"));
        QCOMPARE(result.index, -1);
        QVERIFY(result.text.isEmpty());
    }

    void testEmptyQueryReturnsNoMatch()
    {
        QStringList         history{"hello", "world"};
        QAgentHistorySearch search(history);
        auto                result = search.findNext(QString());
        QCOMPARE(result.index, -1);
    }

    void testSingleMatchReturnsNewest()
    {
        QStringList         history{"abc", "foo bar", "xyz"};
        QAgentHistorySearch search(history);
        auto                result = search.findNext(QStringLiteral("foo"));
        QCOMPARE(result.index, 1);
        QCOMPARE(result.text, QStringLiteral("foo bar"));
    }

    void testMultipleMatchesReturnedNewestFirst()
    {
        QStringList         history{"foo one", "bar", "foo two", "baz", "foo three"};
        QAgentHistorySearch search(history);

        auto first = search.findNext(QStringLiteral("foo"));
        QCOMPARE(first.text, QStringLiteral("foo three"));

        auto second = search.findNext(QStringLiteral("foo"));
        QCOMPARE(second.text, QStringLiteral("foo two"));

        auto third = search.findNext(QStringLiteral("foo"));
        QCOMPARE(third.text, QStringLiteral("foo one"));

        auto fourth = search.findNext(QStringLiteral("foo"));
        QCOMPARE(fourth.index, -1);
    }

    void testDuplicateEntriesDeduped()
    {
        QStringList         history{"hello world", "hello world", "hello universe"};
        QAgentHistorySearch search(history);

        auto first = search.findNext(QStringLiteral("hello"));
        QCOMPARE(first.text, QStringLiteral("hello universe"));

        auto second = search.findNext(QStringLiteral("hello"));
        QCOMPARE(second.text, QStringLiteral("hello world"));

        auto third = search.findNext(QStringLiteral("hello"));
        QCOMPARE(third.index, -1);
    }

    void testQueryChangeResetsScan()
    {
        QStringList         history{"alpha one", "beta one", "alpha two"};
        QAgentHistorySearch search(history);

        auto first = search.findNext(QStringLiteral("alpha"));
        QCOMPARE(first.text, QStringLiteral("alpha two"));

        /* Switch query — scan should restart from the newest entry. */
        auto second = search.findNext(QStringLiteral("beta"));
        QCOMPARE(second.text, QStringLiteral("beta one"));

        /* Switch back — scan restarts with the alpha query from newest. */
        auto third = search.findNext(QStringLiteral("alpha"));
        QCOMPARE(third.text, QStringLiteral("alpha two"));
    }

    void testRewindResetsEverything()
    {
        QStringList         history{"foo", "foobar"};
        QAgentHistorySearch search(history);

        search.findNext(QStringLiteral("foo"));
        search.findNext(QStringLiteral("foo"));
        auto exhausted = search.findNext(QStringLiteral("foo"));
        QCOMPARE(exhausted.index, -1);

        search.rewind();
        auto revived = search.findNext(QStringLiteral("foo"));
        QCOMPARE(revived.text, QStringLiteral("foobar"));
    }

    void testCaseSensitive()
    {
        QStringList         history{"Hello", "hello"};
        QAgentHistorySearch search(history);

        auto result = search.findNext(QStringLiteral("hell"));
        QCOMPARE(result.text, QStringLiteral("hello"));

        auto second = search.findNext(QStringLiteral("hell"));
        QCOMPARE(second.index, -1); /* "Hello" is not a match with lowercase query */
    }

    void testSubstringMatch()
    {
        QStringList         history{"run the whole build first"};
        QAgentHistorySearch search(history);

        auto result = search.findNext(QStringLiteral("whole"));
        QCOMPARE(result.index, 0);

        auto none = search.findNext(QStringLiteral("whole"));
        QCOMPARE(none.index, -1);
    }
};

QTEST_GUILESS_MAIN(Test)
#include "test_qsocagenthistorysearch.moc"
