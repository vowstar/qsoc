// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuipathpicker.h"

#include <QMap>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    /* In-memory directory tree built at runtime: no committed fixtures.
     * Maps an absolute dir to the names of its immediate subdirectories. */
    static QTuiPathPicker::ListDirsFn fakeTree()
    {
        QMap<QString, QStringList> tree;
        tree[QStringLiteral("/")]                   = {"home", "tmp"};
        tree[QStringLiteral("/home")]               = {"user"};
        tree[QStringLiteral("/home/user")]          = {"proj", "docs"};
        tree[QStringLiteral("/home/user/proj")]     = {"src"};
        tree[QStringLiteral("/home/user/proj/src")] = {};
        tree[QStringLiteral("/home/user/docs")]     = {};
        tree[QStringLiteral("/tmp")]                = {};
        return [tree](const QString &path) -> QStringList { return tree.value(path); };
    }

private slots:
    void fullValidAbsolutePath();
    void typoLastSegmentFallsToParent();
    void typoMidSegmentFallsToDeepestValid();
    void relativeInputAnchorsAtCurrent();
    void dotDotClimbsThenDescends();
    void tildeExpandsToHome();
    void emptyInputKeepsCurrent();
    void rootAndDotSegments();
};

void Test::fullValidAbsolutePath()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("/home/user/proj/src"), fakeTree()),
        QStringLiteral("/home/user/proj/src"));
}

void Test::typoLastSegmentFallsToParent()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("/home/user/projX"), fakeTree()),
        QStringLiteral("/home/user"));
}

void Test::typoMidSegmentFallsToDeepestValid()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("/home/zzz/proj"), fakeTree()),
        QStringLiteral("/home"));
}

void Test::relativeInputAnchorsAtCurrent()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/home/user"), QStringLiteral("proj/src"), fakeTree()),
        QStringLiteral("/home/user/proj/src"));
}

void Test::dotDotClimbsThenDescends()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/home/user/proj"), QStringLiteral("../docs"), fakeTree()),
        QStringLiteral("/home/user/docs"));
}

void Test::tildeExpandsToHome()
{
    const QString home = QStringLiteral("/home/user");
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("~/proj"), fakeTree(), home),
        QStringLiteral("/home/user/proj"));
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/tmp"), QStringLiteral("~"), fakeTree(), home),
        QStringLiteral("/home/user"));
    /* No home configured: "~" is treated literally and matches nothing. */
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(QStringLiteral("/"), QStringLiteral("~/proj"), fakeTree()),
        QStringLiteral("/"));
}

void Test::emptyInputKeepsCurrent()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(QStringLiteral("/home/user"), QString(), fakeTree()),
        QStringLiteral("/home/user"));
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/home/user"), QStringLiteral("   "), fakeTree()),
        QStringLiteral("/home/user"));
}

void Test::rootAndDotSegments()
{
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(QStringLiteral("/home"), QStringLiteral("/"), fakeTree()),
        QStringLiteral("/"));
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("/home/./user/."), fakeTree()),
        QStringLiteral("/home/user"));
    QCOMPARE(
        QTuiPathPicker::resolveJumpTarget(
            QStringLiteral("/"), QStringLiteral("/home/user/"), fakeTree()),
        QStringLiteral("/home/user"));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuipathpicker.moc"
