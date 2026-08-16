// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"
#include "agent/qsocfilereadstate.h"
#include "agent/tool/qsoctoolfile.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

#include <thread>

using json = nlohmann::json;

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    static QString writeFile(const QString &path, const QString &content)
    {
        QFileInfo info(path);
        QDir      dir = info.absoluteDir();
        if (!dir.exists()) {
            (void) dir.mkpath(QStringLiteral("."));
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return path;
        }
        QTextStream stream(&file);
        stream << content;
        file.close();
        return path;
    }

    static QString readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        QTextStream stream(&file);
        return stream.readAll();
    }

private slots:
    void testReadStateTracksContent()
    {
        /* Shared read-before-edit state used by both local and remote file
         * tools: a path is "read" only after recordRead, and counts as
         * changed when the content hash differs from the recorded one. */
        QSocFileReadState state;
        const QString     path = QStringLiteral("/ws/file.txt");

        QVERIFY(!state.wasRead(path));
        /* Unrecorded paths never report a change. */
        QVERIFY(!state.changedSinceRead(path, QStringLiteral("anything")));

        state.recordRead(path, QStringLiteral("one two three"));
        QVERIFY(state.wasRead(path));
        QVERIFY(!state.changedSinceRead(path, QStringLiteral("one two three")));
        QVERIFY(state.changedSinceRead(path, QStringLiteral("one two THREE")));

        /* Recording the new content (as a write would) clears staleness. */
        state.recordRead(path, QStringLiteral("one two THREE"));
        QVERIFY(!state.changedSinceRead(path, QStringLiteral("one two THREE")));

        /* Keys are independent per path. */
        QVERIFY(!state.wasRead(QStringLiteral("/ws/other.txt")));
    }

    void testSha256HexIsStable()
    {
        const QString hex1 = QSocFileHistory::sha256Hex(QStringLiteral("hello world"));
        const QString hex2 = QSocFileHistory::sha256Hex(QStringLiteral("hello world"));
        QCOMPARE(hex1, hex2);
        QCOMPARE(hex1.size(), 64);
        QVERIFY(hex1 != QSocFileHistory::sha256Hex(QStringLiteral("hello WORLD")));
    }

    void testHistoryDirIsUnderProject()
    {
        const QString dir
            = QSocFileHistory::historyDir(QStringLiteral("/tmp/proj"), QStringLiteral("abc"));
        QCOMPARE(dir, QStringLiteral("/tmp/proj/.qsoc/file-history/abc"));
    }

    void testReadOnlyHistoryLeavesProjectPristine()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path     = writeFile(tempDir.filePath("existing.txt"), "content");
        const QString metadata = tempDir.filePath(QStringLiteral(".qsoc"));

        QSocFileHistory history(tempDir.path(), QStringLiteral("read-only"));

        QVERIFY(history.storageIsBound());
        QVERIFY(history.isPathInScope(path));
        QVERIFY(history.coversPath(path));
        QVERIFY(history.listSnapshots().isEmpty());
        QCOMPARE(history.latestTurn(), 0);
        QVERIFY(history.isEmpty());
        QVERIFY(history.contentAt(path, 0).isNull());
        QVERIFY(history.previewBoundary(0).isEmpty());
        QVERIFY(!history.restoreRefusal(0).isEmpty());
        QVERIFY(!QFileInfo::exists(metadata));
    }

    void testFirstTrackedEditCreatesStorage()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString   path     = writeFile(tempDir.filePath("existing.txt"), "content");
        const QString   metadata = tempDir.filePath(QStringLiteral(".qsoc"));
        QSocFileHistory history(tempDir.path(), QStringLiteral("lazy-write"));

        QVERIFY(!QFileInfo::exists(metadata));
        QVERIFY(history.trackEdit(path, true, QStringLiteral("content")));
        QVERIFY(QFileInfo::exists(tempDir.filePath(QStringLiteral(".qsoc/tree-id"))));
        QVERIFY(QFileInfo::exists(history.snapshotsPath()));
    }

    void testConcurrentFirstWritesConvergeOnOneTreeIdentity()
    {
        for (int iteration = 0; iteration < 8; ++iteration) {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString   firstPath  = writeFile(tempDir.filePath("first.txt"), "first");
            const QString   secondPath = writeFile(tempDir.filePath("second.txt"), "second");
            QSocFileHistory first(tempDir.path(), QStringLiteral("lazy-first"));
            QSocFileHistory second(tempDir.path(), QStringLiteral("lazy-second"));
            QSemaphore      ready;
            QSemaphore      start;
            bool            firstOk  = false;
            bool            secondOk = false;

            auto persist = [&ready, &start](
                               QSocFileHistory *history,
                               const QString   &path,
                               const QString   &content,
                               bool            *result) {
                ready.release();
                start.acquire();
                *result = history->trackEdit(path, true, content);
            };
            std::thread firstThread(persist, &first, firstPath, QStringLiteral("first"), &firstOk);
            std::thread
                secondThread(persist, &second, secondPath, QStringLiteral("second"), &secondOk);
            ready.acquire(2);
            start.release(2);
            firstThread.join();
            secondThread.join();

            QVERIFY(firstOk);
            QVERIFY(secondOk);
            const auto firstSnapshots  = first.listSnapshots();
            const auto secondSnapshots = second.listSnapshots();
            QCOMPARE(firstSnapshots.size(), 1);
            QCOMPARE(secondSnapshots.size(), 1);
            QCOMPARE(firstSnapshots.constFirst().epoch.tree, secondSnapshots.constFirst().epoch.tree);
        }
    }

    void testDelayedTreeIdentityPublicationCanRetry()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = writeFile(tempDir.filePath("existing.txt"), "content");
        QVERIFY(QDir().mkpath(tempDir.filePath(QStringLiteral(".qsoc"))));
        const QString treeIdPath = tempDir.filePath(QStringLiteral(".qsoc/tree-id"));
        QFile         emptyTreeId(treeIdPath);
        QVERIFY(emptyTreeId.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        emptyTreeId.close();
        QSocFileHistory history(tempDir.path(), QStringLiteral("delayed-tree-id"));
        const bool      firstAttempt = history.trackEdit(path, true, QStringLiteral("content"));
        QVERIFY(!firstAttempt);

        QFile publishedTreeId(treeIdPath);
        QVERIFY(publishedTreeId.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray bytes = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1()
                                 + '\n';
        QCOMPARE(publishedTreeId.write(bytes), bytes.size());
        QVERIFY(publishedTreeId.flush());
        publishedTreeId.close();
        QVERIFY(history.trackEdit(path, true, QStringLiteral("content")));
    }

    void testTrackEditCreatesBaselineSnapshot()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "original");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s1"));
        history.trackEdit(fpath, true, QStringLiteral("original"));

        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 1);
        QCOMPARE(snaps[0].turn, 0);
        QVERIFY(snaps[0].files.contains(fpath));
        QVERIFY(snaps[0].files.value(fpath).isPresent());
        QCOMPARE(
            snaps[0].files.value(fpath).sha256(),
            QSocFileHistory::sha256Hex(QStringLiteral("original")));
    }

    void testTrackEditAbsentFileStoresAbsentRecord()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = QDir(tempDir.path()).filePath(QStringLiteral("new.txt"));

        QSocFileHistory history(tempDir.path(), QStringLiteral("s2"));
        history.trackEdit(fpath, false, QString());

        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 1);
        QCOMPARE(snaps[0].turn, 0);
        QVERIFY(snaps[0].files.value(fpath).isAbsent());
    }

    void testMakeSnapshotCapturesPostTurnState()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v1");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s3"));
        history.trackEdit(fpath, true, QStringLiteral("v1"));
        writeFile(fpath, QStringLiteral("v2"));
        QVERIFY(history.makeSnapshot(1));

        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 2);
        QCOMPARE(snaps[0].turn, 0);
        QCOMPARE(snaps[1].turn, 1);
        QCOMPARE(
            snaps[0].files.value(fpath).sha256(), QSocFileHistory::sha256Hex(QStringLiteral("v1")));
        QCOMPARE(
            snaps[1].files.value(fpath).sha256(), QSocFileHistory::sha256Hex(QStringLiteral("v2")));
    }

    void testApplySnapshotRestoresBaseline()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "before");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s4"));
        history.trackEdit(fpath, true, QStringLiteral("before"));
        writeFile(fpath, QStringLiteral("after turn 1"));
        QVERIFY(history.makeSnapshot(1));
        writeFile(fpath, QStringLiteral("after turn 2"));
        QVERIFY(history.makeSnapshot(2));

        const auto report = history.applySnapshot(0);
        QCOMPARE(static_cast<int>(report.restored.size()), 1);
        QVERIFY(report.failed.isEmpty());
        QCOMPARE(readFile(fpath), QStringLiteral("before"));
    }

    /* A blob name is a digest, not permission to trust arbitrary bytes under
     * that name. A damaged backup must never overwrite the live file. */
    void testApplySnapshotRejectsCorruptBackup()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "before");

        QSocFileHistory history(tempDir.path(), QStringLiteral("corrupt-backup"));
        history.trackEdit(fpath, true, QStringLiteral("before"));
        const QString sha = QSocFileHistory::sha256Hex(QStringLiteral("before"));
        writeFile(history.backupPathFor(sha), QStringLiteral("CORRUPT"));
        writeFile(fpath, QStringLiteral("after"));

        const auto report = history.applySnapshot(0);
        QVERIFY(report.restored.isEmpty());
        QCOMPARE(report.failed, QStringList{fpath});
        QCOMPARE(readFile(fpath), QStringLiteral("after"));
    }

    /* An older record is not proof of a newer checkpoint. If the requested
     * line was never published, rewind leaves the live file alone. */
    void testApplySnapshotRejectsMissingTargetTurn()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath     = writeFile(tempDir.filePath("a.txt"), "before");
        const QString sessionId = QStringLiteral("missing-target");

        {
            QSocFileHistory history(tempDir.path(), sessionId);
            history.trackEdit(fpath, true, QStringLiteral("before"));
            writeFile(fpath, QStringLiteral("turn one"));
            QVERIFY(history.makeSnapshot(1));

            QFile index(history.snapshotsPath());
            QVERIFY(index.open(QIODevice::ReadOnly | QIODevice::Text));
            const QByteArray baseline = index.readLine();
            index.close();
            QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(index.write(baseline), static_cast<qint64>(baseline.size()));
            index.close();
        }

        writeFile(fpath, QStringLiteral("later"));
        QSocFileHistory resumed(tempDir.path(), sessionId);
        const auto      report = resumed.applySnapshot(1);
        QVERIFY(report.targetMissing);
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QCOMPARE(report.unknown, QStringList{fpath});
        QCOMPARE(readFile(fpath), QStringLiteral("later"));
    }

    void testEmptyCheckpointIsStillACompleteTurn()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QSocFileHistory history(tempDir.path(), QStringLiteral("empty-checkpoint"));

        QVERIFY(!QFileInfo::exists(tempDir.filePath(QStringLiteral(".qsoc"))));
        QVERIFY(history.makeSnapshot(1));
        QVERIFY(QFileInfo::exists(tempDir.filePath(QStringLiteral(".qsoc/tree-id"))));
        const auto report = history.applySnapshot(1);
        QVERIFY(!report.targetMissing);
        QVERIFY(report.isEmpty());
        QCOMPARE(history.latestTurn(), 1);
    }

    /* A restore that could not write reports the file as not restored.
     * Silently dropping it made a partial restore read as a smaller one:
     * the caller printed "1 file restored" and the user believed the tree
     * was back where it started. */
    void testApplySnapshotReportsUnwritableFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSocFileHistory history(dir.path(), QStringLiteral("sess-report"));

        QHash<QString, QString>           store;
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [&store](const QString &path) {
            return store.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [&store](const QString &path) {
            if (!store.contains(path)) {
                return QSocFileHistory::LiveRead::absent();
            }
            return QSocFileHistory::LiveRead::present(store.value(path));
        };
        bool allowWrites = true;
        acc.write        = [&store, &allowWrites](const QString &path, const QString &content) {
            if (!allowWrites) {
                return false;
            }
            store.insert(path, content);
            return true;
        };
        acc.remove = [&store](const QString &path) {
            store.remove(path);
            return true;
        };
        acc.tree       = []() { return QStringLiteral("memory-tree"); };
        acc.generation = []() { return QStringLiteral("memory-link"); };
        history.setLiveAccessor(acc);

        const QString path = QStringLiteral("/vfs/remote/keep.txt");
        store.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        store.insert(path, QStringLiteral("v1"));
        QVERIFY(history.makeSnapshot(1));

        allowWrites       = false;
        const auto report = history.applySnapshot(0);
        QVERIFY(report.restored.isEmpty());
        QCOMPARE(static_cast<int>(report.failed.size()), 1);
        QCOMPARE(report.failed.first(), path);
        /* And the file really is still the newer content. */
        QCOMPARE(store.value(path), QStringLiteral("v1"));
    }

    /* Same contract for a removal the accessor refuses. */
    void testApplySnapshotReportsUnremovableFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSocFileHistory history(dir.path(), QStringLiteral("sess-rm"));

        QHash<QString, QString>           store;
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [&store](const QString &path) {
            return store.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [&store](const QString &path) {
            if (!store.contains(path)) {
                return QSocFileHistory::LiveRead::absent();
            }
            return QSocFileHistory::LiveRead::present(store.value(path));
        };
        acc.write = [&store](const QString &path, const QString &content) {
            store.insert(path, content);
            return true;
        };
        acc.remove     = [](const QString &) { return false; };
        acc.tree       = []() { return QStringLiteral("memory-tree"); };
        acc.generation = []() { return QStringLiteral("memory-link"); };
        history.setLiveAccessor(acc);

        const QString path = QStringLiteral("/vfs/remote/created.txt");
        /* Absent before the turn, created by it. */
        history.trackEdit(path, false, QString());
        store.insert(path, QStringLiteral("new"));
        QVERIFY(history.makeSnapshot(1));

        const auto report = history.applySnapshot(0);
        QVERIFY(report.restored.isEmpty());
        QCOMPARE(static_cast<int>(report.failed.size()), 1);
        QCOMPARE(report.failed.first(), path);
        QVERIFY(store.contains(path));
    }

    void testApplySnapshotRestoresMidwayTurn()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v0");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s5"));
        history.trackEdit(fpath, true, QStringLiteral("v0"));
        writeFile(fpath, QStringLiteral("v1"));
        history.makeSnapshot(1);
        writeFile(fpath, QStringLiteral("v2"));
        history.makeSnapshot(2);
        writeFile(fpath, QStringLiteral("v3"));
        history.makeSnapshot(3);

        history.applySnapshot(2);
        QCOMPARE(readFile(fpath), QStringLiteral("v2"));
    }

    void testApplySnapshotUnlinksAbsentFile()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = QDir(tempDir.path()).filePath(QStringLiteral("new.txt"));

        QSocFileHistory history(tempDir.path(), QStringLiteral("s6"));
        /* Baseline: file did not exist. */
        history.trackEdit(fpath, false, QString());
        /* Simulate write_file creating the file, then snapshot captures it. */
        writeFile(fpath, QStringLiteral("created"));
        history.makeSnapshot(1);

        /* Rewind to baseline should delete the file. */
        history.applySnapshot(0);
        QVERIFY(!QFile::exists(fpath));
    }

    void testHashDedupShareBackupBlob()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString f1 = writeFile(tempDir.filePath("a.txt"), "same");
        const QString f2 = writeFile(tempDir.filePath("b.txt"), "same");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s7"));
        history.trackEdit(f1, true, QStringLiteral("same"));
        history.trackEdit(f2, true, QStringLiteral("same"));

        const QString sha        = QSocFileHistory::sha256Hex(QStringLiteral("same"));
        const QString backupPath = history.backupPathFor(sha);
        QVERIFY(QFile::exists(backupPath));

        /* Only ONE backup file on disk even though two source files share it. */
        QDir       backupsDir(QFileInfo(backupPath).absolutePath());
        const auto entries = backupsDir.entryInfoList({QStringLiteral("*.bak")}, QDir::Files);
        QCOMPARE(static_cast<int>(entries.size()), 1);
    }

    void testTruncateAfterDropsFutureSnapshots()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v0");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s8"));
        history.trackEdit(fpath, true, QStringLiteral("v0"));
        writeFile(fpath, QStringLiteral("v1"));
        history.makeSnapshot(1);
        writeFile(fpath, QStringLiteral("v2"));
        history.makeSnapshot(2);
        writeFile(fpath, QStringLiteral("v3"));
        history.makeSnapshot(3);

        history.truncateAfter(1);
        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 2); /* baseline + turn 1 */
        QCOMPARE(snaps[0].turn, 0);
        QCOMPARE(snaps[1].turn, 1);
    }

    void testTruncateAllMakesTheNextEditCreateANewBaseline()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString   path = writeFile(tempDir.filePath("again.txt"), "old");
        QSocFileHistory history(tempDir.path(), QStringLiteral("truncate-all"));
        QVERIFY(history.trackEdit(path, true, QStringLiteral("old")));
        writeFile(path, QStringLiteral("new baseline"));
        QVERIFY(history.makeSnapshot(1));

        QVERIFY(history.truncateAfter(-1));
        QVERIFY(history.listSnapshots().isEmpty());
        QVERIFY(history.trackEdit(path, true, QStringLiteral("new baseline")));

        const auto snapshots = history.listSnapshots();
        QCOMPARE(snapshots.size(), 1);
        QCOMPARE(snapshots.first().turn, 0);
        QCOMPARE(
            snapshots.first().files.value(path).sha256(),
            QSocFileHistory::sha256Hex(QStringLiteral("new baseline")));
    }

    void testLaterTrackedPathStartsAtItsIntroductionCheckpoint()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString pathA = writeFile(tempDir.filePath("a.txt"), "a0");
        const QString pathB = writeFile(tempDir.filePath("b.txt"), "b0");

        QSocFileHistory history(tempDir.path(), QStringLiteral("late-introduction"));
        QVERIFY(history.trackEdit(pathA, true, QStringLiteral("a0")));
        writeFile(pathA, QStringLiteral("a1"));
        QVERIFY(history.makeSnapshot(1));

        QVERIFY(history.trackEdit(pathB, true, QStringLiteral("b0")));
        writeFile(pathB, QStringLiteral("b2"));
        QVERIFY(history.makeSnapshot(2));

        writeFile(pathB, QStringLiteral("live"));
        const auto atIntroduction = history.applySnapshot(1);
        QCOMPARE(readFile(pathB), QStringLiteral("b0"));
        QVERIFY(atIntroduction.restored.contains(pathB));

        writeFile(pathB, QStringLiteral("unrelated-before-introduction"));
        const auto beforeIntroduction = history.applySnapshot(0);
        QCOMPARE(readFile(pathB), QStringLiteral("unrelated-before-introduction"));
        QVERIFY(!beforeIntroduction.restored.contains(pathB));
        QVERIFY(!beforeIntroduction.failed.contains(pathB));
        QVERIFY(!beforeIntroduction.unknown.contains(pathB));
    }

    void testMissingIntroductionCheckpointInvalidatesHistory()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = writeFile(tempDir.filePath("tracked.txt"), "base");
        const QString id   = QStringLiteral("missing-introduction");
        {
            QSocFileHistory first(tempDir.path(), id);
            QVERIFY(first.trackEdit(path, true, QStringLiteral("base")));
            writeFile(path, QStringLiteral("one"));
            QVERIFY(first.makeSnapshot(1));
        }

        QFile index(QSocFileHistory(tempDir.path(), id).snapshotsPath());
        QVERIFY(index.open(QIODevice::ReadOnly));
        QList<QByteArray> lines = index.readAll().split('\n');
        index.close();
        QVERIFY(!lines.isEmpty());
        lines.removeFirst();
        QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
        for (const QByteArray &line : std::as_const(lines)) {
            if (!line.isEmpty()) {
                QCOMPARE(index.write(line), line.size());
                QCOMPARE(index.write("\n", 1), qint64(1));
            }
        }
        index.close();

        QSocFileHistory resumed(tempDir.path(), id);
        QVERIFY(!resumed.trackEdit(path, true, QStringLiteral("one")));
        QVERIFY(!resumed.makeSnapshot(2));
        QVERIFY(!resumed.restoreRefusal(1).isEmpty());
        QCOMPARE(readFile(path), QStringLiteral("one"));
    }

    void testOneNewPathPerTurnCannotDefeatSnapshotCap()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString id        = QStringLiteral("bounded-introductions");
        const int     finalTurn = QSocFileHistory::MAX_SNAPSHOTS + 8;
        QString       firstPath;
        {
            QSocFileHistory history(tempDir.path(), id);
            for (int turn = 1; turn <= finalTurn; ++turn) {
                const QString path = tempDir.filePath(QStringLiteral("file-%1.txt").arg(turn));
                if (firstPath.isEmpty()) {
                    firstPath = path;
                }
                QVERIFY(history.trackEdit(path, false, QString()));
                writeFile(path, QStringLiteral("turn %1").arg(turn));
                QVERIFY(history.makeSnapshot(turn));
                QVERIFY(history.listSnapshots().size() <= QSocFileHistory::MAX_SNAPSHOTS);
            }
        }

        QSocFileHistory resumed(tempDir.path(), id);
        const auto      snapshots = resumed.listSnapshots();
        QCOMPARE(snapshots.size(), QSocFileHistory::MAX_SNAPSHOTS);
        QVERIFY(snapshots.first().turn > 0);
        QVERIFY(snapshots.first().files.contains(firstPath));
        QCOMPARE(snapshots.first().files.value(firstPath).introducedTurn(), snapshots.first().turn);
        QVERIFY(resumed.restoreRefusal(finalTurn).isEmpty());
        QVERIFY(resumed.makeSnapshot(finalTurn + 1));
        QCOMPARE(resumed.listSnapshots().size(), QSocFileHistory::MAX_SNAPSHOTS);
    }

    void testTruncateFailureKeepsForwardSnapshots()
    {
#ifdef Q_OS_WIN
        QSKIP("directory write permissions are not deterministic on Windows");
#else
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString   path = writeFile(tempDir.filePath("future.txt"), "base");
        const QString   id   = QStringLiteral("truncate-failure");
        QSocFileHistory history(tempDir.path(), id);
        QVERIFY(history.trackEdit(path, true, QStringLiteral("base")));
        writeFile(path, QStringLiteral("one"));
        QVERIFY(history.makeSnapshot(1));
        writeFile(path, QStringLiteral("two"));
        QVERIFY(history.makeSnapshot(2));

        const QString root           = QSocFileHistory::historyDir(tempDir.path(), id);
        const auto    oldPermissions = QFileInfo(root).permissions();
        QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto restorePermissions = qScopeGuard(
            [&] { (void) QFile::setPermissions(root, oldPermissions); });

        QVERIFY(!history.truncateAfter(1));
        QCOMPARE(history.latestTurn(), 2);
#endif
    }

    void testReadableButIncompleteCheckpointNeverFallsBack()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString   path = tempDir.filePath("only-copy.txt");
        const QString   id   = QStringLiteral("incomplete-exact");
        QSocFileHistory history(tempDir.path(), id);
        QVERIFY(history.trackEdit(path, false, QString()));
        writeFile(path, QStringLiteral("only copy"));
        QVERIFY(history.makeSnapshot(1));

        QFile index(history.snapshotsPath());
        QVERIFY(index.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = index.readAll().split('\n');
        index.close();
        QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
        for (const QByteArray &line : lines) {
            if (line.isEmpty()) {
                continue;
            }
            json record = json::parse(line.constData(), line.constData() + line.size());
            if (record.value("turn", -1) == 1) {
                record["files"] = json::object();
            }
            const std::string encoded = record.dump();
            QCOMPARE(index.write(encoded.data(), encoded.size()), qint64(encoded.size()));
            QCOMPARE(index.write("\n", 1), qint64(1));
        }
        index.close();

        QSocFileHistory resumed(tempDir.path(), id);
        const auto      report = resumed.applySnapshot(1);
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.targetMissing);
        QVERIFY(report.unknown.isEmpty());
        QCOMPARE(readFile(path), QStringLiteral("only copy"));
    }

    void testUnreadableIndexCannotBeReplacedAsEmptyHistory()
    {
#ifdef Q_OS_WIN
        QSKIP("file read permissions are not deterministic on Windows");
#else
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString   path = writeFile(tempDir.filePath("kept.txt"), "base");
        const QString   id   = QStringLiteral("unreadable-index");
        QSocFileHistory first(tempDir.path(), id);
        QVERIFY(first.trackEdit(path, true, QStringLiteral("base")));
        QFile index(first.snapshotsPath());
        QVERIFY(index.open(QIODevice::ReadOnly));
        const QByteArray before = index.readAll();
        index.close();
        const auto oldPermissions = QFileInfo(first.snapshotsPath()).permissions();
        QVERIFY(QFile::setPermissions(first.snapshotsPath(), QFileDevice::WriteOwner));
        const auto restorePermissions = qScopeGuard(
            [&] { (void) QFile::setPermissions(first.snapshotsPath(), oldPermissions); });

        QSocFileHistory resumed(tempDir.path(), id);
        QVERIFY(!resumed.trackEdit(path, true, QStringLiteral("base")));
        QVERIFY(!resumed.makeSnapshot(1));
        QVERIFY(!resumed.truncateAfter(-1));

        QVERIFY(QFile::setPermissions(first.snapshotsPath(), oldPermissions));
        QFile afterFile(first.snapshotsPath());
        QVERIFY(afterFile.open(QIODevice::ReadOnly));
        QCOMPARE(afterFile.readAll(), before);
#endif
    }

    void testTruncateAfterGcOrphanedBlobs()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "base");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s9"));
        history.trackEdit(fpath, true, QStringLiteral("base"));
        writeFile(fpath, QStringLiteral("turn1"));
        history.makeSnapshot(1);
        writeFile(fpath, QStringLiteral("turn2"));
        history.makeSnapshot(2);

        const QString turn2Sha = QSocFileHistory::sha256Hex(QStringLiteral("turn2"));
        QVERIFY(QFile::exists(history.backupPathFor(turn2Sha)));

        history.truncateAfter(1);

        /* turn2 backup should be gc'd since no surviving snapshot references
         * it, while base and turn1 blobs remain. */
        QVERIFY(!QFile::exists(history.backupPathFor(turn2Sha)));
        QVERIFY(
            QFile::exists(
                history.backupPathFor(QSocFileHistory::sha256Hex(QStringLiteral("base")))));
        QVERIFY(
            QFile::exists(
                history.backupPathFor(QSocFileHistory::sha256Hex(QStringLiteral("turn1")))));
    }

    void testContentAtReturnsHistoricalVersion()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v0");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s10"));
        history.trackEdit(fpath, true, QStringLiteral("v0"));
        writeFile(fpath, QStringLiteral("v1"));
        history.makeSnapshot(1);
        writeFile(fpath, QStringLiteral("v2"));
        history.makeSnapshot(2);

        QCOMPARE(history.contentAt(fpath, 0), QStringLiteral("v0"));
        QCOMPARE(history.contentAt(fpath, 1), QStringLiteral("v1"));
        QCOMPARE(history.contentAt(fpath, 2), QStringLiteral("v2"));
    }

    void testLatestTurnTracksHighest()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v0");

        QSocFileHistory history(tempDir.path(), QStringLiteral("s11"));
        QCOMPARE(history.latestTurn(), 0);
        history.trackEdit(fpath, true, QStringLiteral("v0"));
        QCOMPARE(history.latestTurn(), 0);
        writeFile(fpath, QStringLiteral("v1"));
        history.makeSnapshot(1);
        QCOMPARE(history.latestTurn(), 1);
        writeFile(fpath, QStringLiteral("v2"));
        history.makeSnapshot(3);
        QCOMPARE(history.latestTurn(), 3);
    }

    void testResumedSessionSeedsTrackedFiles()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "v0");

        {
            QSocFileHistory first(tempDir.path(), QStringLiteral("resume"));
            first.trackEdit(fpath, true, QStringLiteral("v0"));
            writeFile(fpath, QStringLiteral("v1"));
            first.makeSnapshot(1);
        }

        /* Write a new version on disk WITHOUT going through trackEdit — the
         * resumed history should still pick it up in its next snapshot
         * because trackedFiles was seeded from the on-disk history. */
        writeFile(fpath, QStringLiteral("v2"));
        QSocFileHistory resumed(tempDir.path(), QStringLiteral("resume"));
        resumed.makeSnapshot(2);

        QCOMPARE(resumed.contentAt(fpath, 2), QStringLiteral("v2"));
    }

    void testEditFileToolCapturesPreEditBackup()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "hello world");

        QSocFileHistory history(tempDir.path(), QStringLiteral("tool_edit"));

        QSocToolFileEdit editTool;
        editTool.setFileHistory(&history);

        json args;
        args["file_path"]    = fpath.toStdString();
        args["old_string"]   = "hello";
        args["new_string"]   = "goodbye";
        const QString result = editTool.execute(args);
        QVERIFY(result.startsWith("Successfully edited"));
        QCOMPARE(readFile(fpath), QStringLiteral("goodbye world"));

        /* Baseline should now hold the pre-edit content. */
        const QString baselineSha = QSocFileHistory::sha256Hex(QStringLiteral("hello world"));
        QVERIFY(QFile::exists(history.backupPathFor(baselineSha)));

        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 1);
        QCOMPARE(snaps[0].turn, 0);
        QCOMPARE(snaps[0].files.value(fpath).sha256(), baselineSha);
    }

    void testWriteFileToolCapturesAbsentBaseline()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = QDir(tempDir.path()).filePath(QStringLiteral("new.txt"));
        QVERIFY(!QFile::exists(fpath));

        QSocFileHistory history(tempDir.path(), QStringLiteral("tool_write"));

        QSocToolFileWrite writeTool;
        writeTool.setFileHistory(&history);

        json args;
        args["file_path"]    = fpath.toStdString();
        args["content"]      = "fresh content";
        const QString result = writeTool.execute(args);
        QVERIFY(result.startsWith("Successfully wrote"));
        QCOMPARE(readFile(fpath), QStringLiteral("fresh content"));

        /* Baseline should record the file as absent (empty sha). */
        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 1);
        QCOMPARE(snaps[0].turn, 0);
        QVERIFY(snaps[0].files.contains(fpath));
        QVERIFY(snaps[0].files.value(fpath).isAbsent());

        /* makeSnapshot after the "turn" should capture the new content. */
        QVERIFY(history.makeSnapshot(1));
        history.applySnapshot(0);
        QVERIFY(!QFile::exists(fpath));
    }

    void testLocalToolsWriteOutsideProjectWithoutCheckpointing()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString project = tempDir.filePath(QStringLiteral("project"));
        const QString outside = tempDir.filePath(QStringLiteral("outside"));
        QVERIFY(QDir().mkpath(project));
        QVERIFY(QDir().mkpath(outside));

        QSocFileHistory history(project, QStringLiteral("project-only"));
        const QString   edited = writeFile(QDir(outside).filePath("edit.txt"), "before");
        QVERIFY(!history.isPathInScope(edited));
        QVERIFY(!history.coversPath(edited));
        QVERIFY(!history.trackEdit(edited, true, QStringLiteral("before")));

        QSocToolFileEdit editTool;
        editTool.setFileHistory(&history);
        const QString editResult = editTool.execute(
            json{
                {"file_path", edited.toStdString()},
                {"old_string", "before"},
                {"new_string", "after"}});
        QVERIFY2(editResult.startsWith(QStringLiteral("Successfully edited")), qPrintable(editResult));
        QCOMPARE(readFile(edited), QStringLiteral("after"));

        const QString     writePath = QDir(outside).filePath(QStringLiteral("write.txt"));
        QSocToolFileWrite writeTool;
        writeTool.setFileHistory(&history);
        const QString writeResult = writeTool.execute(
            json{{"file_path", writePath.toStdString()}, {"content", "created"}});
        QVERIFY2(
            writeResult.startsWith(QStringLiteral("Successfully wrote")), qPrintable(writeResult));
        QCOMPARE(readFile(writePath), QStringLiteral("created"));
        QVERIFY(history.listSnapshots().isEmpty());
    }

    void testLocalToolRefusesARecreatedProjectRoot()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString project = tempDir.filePath(QStringLiteral("project"));
        const QString saved   = tempDir.filePath(QStringLiteral("saved"));
        QVERIFY(QDir().mkpath(project));
        QSocFileHistory history(project, QStringLiteral("recreated-tool-root"));
        QVERIFY(QDir().rename(project, saved));
        QVERIFY(QDir().mkpath(project));

        const QString target = QDir(project).filePath(
            QStringLiteral("new-parent/must-not-exist.txt"));
        QVERIFY(history.isPathInScope(target));
        QVERIFY(!history.coversPath(target));
        QSocToolFileWrite tool;
        tool.setFileHistory(&history);
        const QString result = tool.execute(
            json{{"file_path", target.toStdString()}, {"content", "blocked"}});
        QVERIFY2(result.startsWith(QStringLiteral("Error:")), qPrintable(result));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(QDir(project).filePath(QStringLiteral("new-parent"))));
    }

    void testWriteFileToolRefusesAnUnpersistedBaseline()
    {
#ifdef Q_OS_WIN
        QSKIP("directory write permissions are not deterministic on Windows");
#else
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QSocFileHistory history(tempDir.path(), QStringLiteral("tool-history-failure"));
        const QString   existing = writeFile(tempDir.filePath("existing.txt"), "baseline");
        QVERIFY(history.trackEdit(existing, true, QStringLiteral("baseline")));

        const QString historyRoot
            = QSocFileHistory::historyDir(tempDir.path(), QStringLiteral("tool-history-failure"));
        const QFile::Permissions originalPermissions = QFileInfo(historyRoot).permissions();
        QVERIFY(QFile::setPermissions(historyRoot, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto restorePermissions = qScopeGuard(
            [&] { (void) QFile::setPermissions(historyRoot, originalPermissions); });

        const QString     target = tempDir.filePath("must-not-exist.txt");
        QSocToolFileWrite tool;
        tool.setFileHistory(&history);
        json args;
        args["file_path"] = target.toStdString();
        args["content"]   = "new content";

        const QString result = tool.execute(args);
        QVERIFY(result.startsWith(QStringLiteral("Error:")));
        QVERIFY(!QFile::exists(target));
#endif
    }

    void testWriteFileToolRefusesACorruptExistingHistory()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath     = writeFile(tempDir.filePath("a.txt"), "before");
        const QString sessionId = QStringLiteral("tool-corrupt-history");
        const QString sha       = QSocFileHistory::sha256Hex(QStringLiteral("before"));

        {
            QSocFileHistory first(tempDir.path(), sessionId);
            QVERIFY(first.trackEdit(fpath, true, QStringLiteral("before")));
            writeFile(first.backupPathFor(sha), QStringLiteral("CORRUPT"));
        }

        QSocFileHistory   resumed(tempDir.path(), sessionId);
        QSocToolFileWrite tool;
        tool.setFileHistory(&resumed);
        json args;
        args["file_path"] = fpath.toStdString();
        args["content"]   = "after";

        const QString result = tool.execute(args);
        QVERIFY(result.startsWith(QStringLiteral("Error:")));
        QCOMPARE(readFile(fpath), QStringLiteral("before"));
    }

    void testRoundtripEditSnapshotRestore()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = writeFile(tempDir.filePath("a.txt"), "initial");

        QSocFileHistory history(tempDir.path(), QStringLiteral("roundtrip"));

        history.trackEdit(fpath, true, QStringLiteral("initial"));
        writeFile(fpath, QStringLiteral("edit1"));
        history.makeSnapshot(1);

        history.trackEdit(fpath, true, QStringLiteral("edit1"));
        writeFile(fpath, QStringLiteral("edit2"));
        history.makeSnapshot(2);

        history.applySnapshot(1);
        QCOMPARE(readFile(fpath), QStringLiteral("edit1"));

        history.applySnapshot(0);
        QCOMPARE(readFile(fpath), QStringLiteral("initial"));

        history.applySnapshot(2);
        QCOMPARE(readFile(fpath), QStringLiteral("edit2"));
    }

    /* A custom live accessor (an in-memory map) stands in for a remote SFTP
     * transport: snapshot capture reads through it and restore writes /
     * deletes through it, never touching the local disk path. */
    void testCustomAccessorSnapshotAndRestore()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QSocFileHistory history(tempDir.path(), QStringLiteral("remote-sim"));

        QHash<QString, QString>           store;
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [&store](const QString &path) {
            return store.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [&store](const QString &path) {
            if (!store.contains(path)) {
                return QSocFileHistory::LiveRead::absent();
            }
            return QSocFileHistory::LiveRead::present(store.value(path));
        };
        acc.write = [&store](const QString &path, const QString &content) {
            store.insert(path, content);
            return true;
        };
        acc.remove = [&store](const QString &path) {
            store.remove(path);
            return true;
        };
        acc.tree       = []() { return QStringLiteral("memory-tree"); };
        acc.generation = []() { return QStringLiteral("memory-link"); };
        history.setLiveAccessor(acc);

        const QString rpath = QStringLiteral("/vfs/remote/a.txt");
        store.insert(rpath, QStringLiteral("v0"));

        history.trackEdit(rpath, true, QStringLiteral("v0"));
        store.insert(rpath, QStringLiteral("v1")); /* the "remote" edit */
        history.makeSnapshot(1);

        /* Local disk must be untouched: the accessor is the only backend. */
        QVERIFY(!QFileInfo::exists(rpath));

        history.applySnapshot(0);
        QCOMPARE(store.value(rpath), QStringLiteral("v0"));

        history.applySnapshot(1);
        QCOMPARE(store.value(rpath), QStringLiteral("v1"));
    }

    /* Restoring to an absent baseline deletes the file through the accessor. */
    void testCustomAccessorDeletesCreatedFile()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QSocFileHistory history(tempDir.path(), QStringLiteral("remote-del"));

        QHash<QString, QString>           store;
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [&store](const QString &path) {
            return store.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [&store](const QString &path) {
            if (!store.contains(path)) {
                return QSocFileHistory::LiveRead::absent();
            }
            return QSocFileHistory::LiveRead::present(store.value(path));
        };
        acc.write = [&store](const QString &path, const QString &content) {
            store.insert(path, content);
            return true;
        };
        acc.remove = [&store](const QString &path) {
            store.remove(path);
            return true;
        };
        acc.tree       = []() { return QStringLiteral("memory-tree"); };
        acc.generation = []() { return QStringLiteral("memory-link"); };
        history.setLiveAccessor(acc);

        const QString rpath = QStringLiteral("/vfs/remote/new.txt");
        history.trackEdit(rpath, false, QString());     /* absent baseline */
        store.insert(rpath, QStringLiteral("created")); /* the "remote" create */
        history.makeSnapshot(1);

        history.applySnapshot(0);
        QVERIFY(!store.contains(rpath)); /* deleted via accessor.remove */
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocfilehistory.moc"
