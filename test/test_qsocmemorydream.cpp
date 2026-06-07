// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemorydream.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

    static QSocSession::Info sessionInfo(const QString &id, qint64 mtimeMs)
    {
        QSocSession::Info info;
        info.id           = id;
        info.lastModified = QDateTime::fromMSecsSinceEpoch(mtimeMs);
        return info;
    }

private slots:
    /* Session gate: counts sessions newer than lastMs, excluding current. */
    void testSessionGate()
    {
        const qint64             base     = 1000000;
        QList<QSocSession::Info> sessions = {
            sessionInfo("a", base + 100),
            sessionInfo("b", base + 200),
            sessionInfo("cur", base + 300), /* current, excluded */
            sessionInfo("c", base - 50),    /* older than lastMs */
        };

        /* lastMs = base: a and b qualify, cur excluded, c too old. */
        QVERIFY(QSocMemoryDream::sessionGatePasses(sessions, base, "cur", 2));
        QVERIFY(!QSocMemoryDream::sessionGatePasses(sessions, base, "cur", 3));
        /* Without excluding current, three qualify. */
        QVERIFY(QSocMemoryDream::sessionGatePasses(sessions, base, QString(), 3));
    }

    /* Fresh lock blocks; stale lock is stolen with prior mtime captured. */
    void testLockLifecycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = QDir(dir.path()).filePath(".consolidate.lock");

        /* No lock yet: absent -> last consolidated is 0. */
        QCOMPARE(QSocMemoryDream::readLastConsolidatedMs(lockPath), qint64(0));

        /* Acquire on empty path: succeeds, no prior mtime. */
        auto first = QSocMemoryDream::tryAcquireLock(lockPath, 1);
        QVERIFY(first.acquired);
        QCOMPARE(first.priorMtimeMs, qint64(0));
        QVERIFY(QFile::exists(lockPath));

        /* A fresh lock blocks a second acquirer. */
        auto blocked = QSocMemoryDream::tryAcquireLock(lockPath, 1);
        QVERIFY(!blocked.acquired);

        /* Age the lock past the stale window: it gets stolen. */
        const qint64 twoHoursAgo = QDateTime::currentMSecsSinceEpoch() - 2 * 3600 * 1000;
        {
            QFile file(lockPath);
            QVERIFY(file.open(QIODevice::ReadWrite));
            file.setFileTime(
                QDateTime::fromMSecsSinceEpoch(twoHoursAgo), QFileDevice::FileModificationTime);
            file.close();
        }
        auto stolen = QSocMemoryDream::tryAcquireLock(lockPath, 1);
        QVERIFY(stolen.acquired);
        QVERIFY(stolen.priorMtimeMs > 0);
    }

    /* Rollback with no prior removes the lock; with prior rewinds mtime. */
    void testRollback()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = QDir(dir.path()).filePath(".consolidate.lock");

        QSocMemoryDream::tryAcquireLock(lockPath, 1);
        QVERIFY(QFile::exists(lockPath));
        QSocMemoryDream::rollbackLock(lockPath, 0);
        QVERIFY(!QFile::exists(lockPath)); /* removed when no prior */

        QSocMemoryDream::tryAcquireLock(lockPath, 1);
        const qint64 priorMs = QDateTime::currentMSecsSinceEpoch() - 5 * 3600 * 1000;
        QSocMemoryDream::rollbackLock(lockPath, priorMs);
        QVERIFY(QFile::exists(lockPath)); /* kept, mtime rewound */
        const qint64 actual = QFileInfo(lockPath).lastModified().toMSecsSinceEpoch();
        QVERIFY(qAbs(actual - priorMs) < 2000);
    }

    /* Consolidation prompt names the allowed tools and is scope-aware. */
    void testConsolidationPrompt()
    {
        const QString prompt = QSocMemoryDream::consolidationPrompt();
        QVERIFY(prompt.contains("memory_read"));
        QVERIFY(prompt.contains("memory_write"));
        QVERIFY(prompt.contains("memory_delete"));
        QVERIFY(prompt.contains("scope"));
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocmemorydream.moc"
