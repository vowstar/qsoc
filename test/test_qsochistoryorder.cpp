// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochistoryorder.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testCurrentProjectSurfacesLast()
    {
        /* Other-project entries come first, current-project last, so the tail
         * (what Up-arrow reaches first) belongs to the active project. */
        const QStringList displays{"other-a", "cur-a", "other-b", "cur-b"};
        const QList<bool> current{false, true, false, true};
        const QList<int>  kept = QSocHistoryOrder::orderedDedup(displays, current);
        QCOMPARE(kept, (QList<int>{0, 2, 1, 3}));
        /* Last kept index is a current-project entry. */
        QVERIFY(current[kept.last()]);
    }

    void testDedupKeepsMostRecent()
    {
        /* A command repeated keeps only its most-recent (latest) position. */
        const QStringList displays{"build", "test", "build"};
        const QList<bool> current{true, true, true};
        const QList<int>  kept = QSocHistoryOrder::orderedDedup(displays, current);
        QCOMPARE(kept, (QList<int>{1, 2}));
    }

    void testDedupAcrossProjectsPrefersCurrent()
    {
        /* Same command in another project then the current one: the current
         * occurrence wins (appears once, in the current section). */
        const QStringList displays{"deploy", "deploy"};
        const QList<bool> current{false, true};
        const QList<int>  kept = QSocHistoryOrder::orderedDedup(displays, current);
        QCOMPARE(kept, (QList<int>{1}));
        QVERIFY(current[kept.last()]);
    }

    void testEmpty() { QVERIFY(QSocHistoryOrder::orderedDedup({}, {}).isEmpty()); }
};

QSOC_TEST_MAIN(Test)

#include "test_qsochistoryorder.moc"
