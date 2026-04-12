// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"
#include "agent/tool/qsoctoolfile.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
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
    void initTestCase() { TestApp::instance(); }

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
        QCOMPARE(snaps[0].files.value(fpath), QSocFileHistory::sha256Hex(QStringLiteral("original")));
    }

    void testTrackEditAbsentFileStoresEmptySha()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString fpath = QDir(tempDir.path()).filePath(QStringLiteral("new.txt"));

        QSocFileHistory history(tempDir.path(), QStringLiteral("s2"));
        history.trackEdit(fpath, false, QString());

        const auto snaps = history.listSnapshots();
        QCOMPARE(static_cast<int>(snaps.size()), 1);
        QCOMPARE(snaps[0].turn, 0);
        QVERIFY(snaps[0].files.value(fpath).isEmpty());
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
        QCOMPARE(snaps[0].files.value(fpath), QSocFileHistory::sha256Hex(QStringLiteral("v1")));
        QCOMPARE(snaps[1].files.value(fpath), QSocFileHistory::sha256Hex(QStringLiteral("v2")));
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

        const QStringList touched = history.applySnapshot(0);
        QCOMPARE(static_cast<int>(touched.size()), 1);
        QCOMPARE(readFile(fpath), QStringLiteral("before"));
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
        QCOMPARE(snaps[0].files.value(fpath), baselineSha);
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
        QVERIFY(snaps[0].files.value(fpath).isEmpty());

        /* makeSnapshot after the "turn" should capture the new content. */
        QVERIFY(history.makeSnapshot(1));
        history.applySnapshot(0);
        QVERIFY(!QFile::exists(fpath));
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
};

QTEST_GUILESS_MAIN(Test)
#include "test_qsocfilehistory.moc"
