// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclinediff.h"
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

    /* Helper: extract the kinds in order, ignoring text content. Lets
     * tests express expected structure compactly. */
    static QString kindString(const QList<QSocLineDiff::DiffLine> &diff)
    {
        QString out;
        for (const auto &line : diff) {
            switch (line.kind) {
            case QSocLineDiff::Kind::Context:
                out += QLatin1Char('=');
                break;
            case QSocLineDiff::Kind::Add:
                out += QLatin1Char('+');
                break;
            case QSocLineDiff::Kind::Del:
                out += QLatin1Char('-');
                break;
            case QSocLineDiff::Kind::Hunk:
                out += QLatin1Char('@');
                break;
            }
        }
        return out;
    }

    void testIdenticalProducesNothing()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("alpha\nbeta\ngamma"), QStringLiteral("alpha\nbeta\ngamma"));
        QVERIFY(diff.isEmpty());
    }

    void testEmptyOldYieldsAllAdds()
    {
        auto diff = QSocLineDiff::computeLineDiff(QString(), QStringLiteral("a\nb\nc"));
        QCOMPARE(kindString(diff), QStringLiteral("@+++"));
        QCOMPARE(diff[1].text, QStringLiteral("+a"));
        QCOMPARE(diff[2].text, QStringLiteral("+b"));
        QCOMPARE(diff[3].text, QStringLiteral("+c"));
    }

    void testEmptyNewYieldsAllDeletes()
    {
        auto diff = QSocLineDiff::computeLineDiff(QStringLiteral("a\nb"), QString());
        QCOMPARE(kindString(diff), QStringLiteral("@--"));
        QCOMPARE(diff[1].text, QStringLiteral("-a"));
        QCOMPARE(diff[2].text, QStringLiteral("-b"));
    }

    void testSingleLineModifyShowsBothLines()
    {
        auto diff
            = QSocLineDiff::computeLineDiff(QStringLiteral("width: 32"), QStringLiteral("width: 64"));
        /* No surrounding lines exist, so just hunk + del + add */
        QCOMPARE(kindString(diff), QStringLiteral("@-+"));
        QCOMPARE(diff[1].text, QStringLiteral("-width: 32"));
        QCOMPARE(diff[2].text, QStringLiteral("+width: 64"));
    }

    void testContextLinesAroundChange()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("a\nb\nc\nd\ne"), QStringLiteral("a\nb\nX\nd\ne"));
        /* All 5 source lines fit in the default 3-line context window. */
        QCOMPARE(kindString(diff), QStringLiteral("@==-+=="));
        QCOMPARE(diff[3].text, QStringLiteral("-c"));
        QCOMPARE(diff[4].text, QStringLiteral("+X"));
    }

    void testInsertOnlyKeepsContext()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("a\nb\nc"), QStringLiteral("a\nb\nNEW\nc"));
        QCOMPARE(kindString(diff), QStringLiteral("@==+="));
        QCOMPARE(diff[3].text, QStringLiteral("+NEW"));
    }

    void testDeleteOnlyKeepsContext()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("a\nb\nGONE\nc"), QStringLiteral("a\nb\nc"));
        QCOMPARE(kindString(diff), QStringLiteral("@==-="));
        QCOMPARE(diff[3].text, QStringLiteral("-GONE"));
    }

    void testHunkHeaderHasCorrectCounts()
    {
        auto diff
            = QSocLineDiff::computeLineDiff(QStringLiteral("a\nb\nc"), QStringLiteral("a\nB\nc"));
        QCOMPARE(diff.first().kind, QSocLineDiff::Kind::Hunk);
        /* Hunk covers all 3 source lines (= - + =) so old=3 new=3. */
        QCOMPARE(diff.first().text, QStringLiteral("@@ -1,3 +1,3 @@"));
    }

    void testCjkContent()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("行一\n行二\n行三"), QStringLiteral("行一\n新二\n行三"));
        QCOMPARE(kindString(diff), QStringLiteral("@=-+="));
        QCOMPARE(diff[2].text, QStringLiteral("-行二"));
        QCOMPARE(diff[3].text, QStringLiteral("+新二"));
    }

    void testTrailingNewlinePreserved()
    {
        /* "a" vs "a\n" — adding a trailing newline introduces an empty
         * second line, which the diff should record as an Add. */
        auto diff = QSocLineDiff::computeLineDiff(QStringLiteral("a"), QStringLiteral("a\n"));
        QCOMPARE(kindString(diff), QStringLiteral("@=+"));
    }

    void testZeroContextHidesUnchangedLines()
    {
        auto diff = QSocLineDiff::computeLineDiff(
            QStringLiteral("a\nb\nc\nd\ne"),
            QStringLiteral("a\nb\nX\nd\ne"),
            /*contextLines=*/0);
        /* With zero context, only the change itself remains. */
        QCOMPARE(kindString(diff), QStringLiteral("@-+"));
    }
};

QTEST_GUILESS_MAIN(Test)
#include "test_qsoclinediff.moc"
