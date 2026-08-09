// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

/**
 * @brief In-memory stand-in for a working tree behind a replaceable link.
 * @details Holds the files, the transport generation the accessor reports,
 *          and a switch that makes reads and stats fail the way a link that
 *          went quiet does. Nothing here touches the network or the disk.
 */
class FakeTree
{
public:
    QHash<QString, QString> files;
    quint64                 gen         = 1;
    bool                    unreadable  = false;
    int                     removeCalls = 0;
    int                     writeCalls  = 0;
    int                     readCalls   = 0;
    /* Bump the generation from inside the Nth write / read, so a capture or
     * a restore can be made to straddle two transports. 0 disables. */
    int bumpOnWrite = 0;
    int bumpOnRead  = 0;
    /* Whether the accessor advertises a generation at all. */
    bool reportGeneration = true;

    QSocFileHistory::LiveFileAccessor accessor()
    {
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [this](const QString &path) {
            if (unreadable) {
                return QSocFileHistory::FileState::Unknown;
            }
            return files.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [this](const QString &path) {
            readCalls++;
            if (bumpOnRead != 0 && readCalls == bumpOnRead) {
                gen++;
            }
            if (unreadable) {
                return QSocFileHistory::LiveRead::unknown();
            }
            if (!files.contains(path)) {
                return QSocFileHistory::LiveRead::absent();
            }
            return QSocFileHistory::LiveRead::present(files.value(path));
        };
        acc.write = [this](const QString &path, const QString &content) {
            writeCalls++;
            if (bumpOnWrite != 0 && writeCalls == bumpOnWrite) {
                gen++;
            }
            files.insert(path, content);
            return true;
        };
        acc.remove = [this](const QString &path) {
            removeCalls++;
            files.remove(path);
            return true;
        };
        if (reportGeneration) {
            acc.generation = [this]() { return gen; };
        }
        return acc;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QString jsonlLine(int turn, const QString &path, const QString &value, bool quoted)
    {
        const QString encoded = quoted ? QStringLiteral("\"%1\"").arg(value) : value;
        return QStringLiteral(R"({"turn":%1,"ts":"2026-01-01T00:00:00.000Z","files":{"%2":%3}})")
            .arg(turn)
            .arg(path, encoded);
    }

    /* Hand-write a pre-"gen" history plus the blobs its records name. */
    static void seedLegacyHistory(
        const QString     &projectPath,
        const QString     &sessionId,
        const QStringList &lines,
        const QStringList &blobContents)
    {
        const QString root = QSocFileHistory::historyDir(projectPath, sessionId);
        QDir().mkpath(QDir(root).filePath(QStringLiteral("backups")));
        for (const QString &content : blobContents) {
            const QString sha = QSocFileHistory::sha256Hex(content);
            QFile         blob(
                QDir(root).filePath(QStringLiteral("backups/") + sha + QStringLiteral(".bak")));
            QVERIFY(blob.open(QIODevice::WriteOnly | QIODevice::Truncate));
            blob.write(content.toUtf8());
            blob.close();
        }
        QFile jsonl(QDir(root).filePath(QStringLiteral("snapshots.jsonl")));
        QVERIFY(jsonl.open(QIODevice::WriteOnly | QIODevice::Truncate));
        for (const QString &line : lines) {
            jsonl.write(line.toUtf8());
            jsonl.write("\n", 1);
        }
        jsonl.close();
    }

    static bool hasTurn(const QSocFileHistory &history, int turn)
    {
        for (const auto &snap : history.listSnapshots()) {
            if (snap.turn == turn) {
                return true;
            }
        }
        return false;
    }

    static QSocFileHistory::FileRecord recordAt(
        const QSocFileHistory &history, int turn, const QString &path)
    {
        for (const auto &snap : history.listSnapshots()) {
            if (snap.turn == turn) {
                return snap.files.value(path);
            }
        }
        return QSocFileHistory::FileRecord();
    }

private slots:
    /* A capture that could not read a file must record "I do not know", not
     * "it is not there": the absent reading turns a link failure into an
     * instruction to unlink a file that is still on the host. */
    void testSnapshotUnknownStateNeverDeletesLiveFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree        tree;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-headline"));
        history.setLiveAccessor(tree.accessor());

        const QString path = QStringLiteral("/vfs/keep.v");
        tree.files.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        tree.files.insert(path, QStringLiteral("v1")); /* the turn's edit */

        tree.unreadable = true;
        QVERIFY(history.makeSnapshot(1));

        tree.unreadable   = false; /* the link comes back */
        const auto report = history.applySnapshot(1);

        QVERIFY(tree.files.contains(path));
        QCOMPARE(tree.files.value(path), QStringLiteral("v1")); /* not rewritten */
        QCOMPARE(tree.removeCalls, 0);
        QCOMPARE(report.unknown, QStringList{path});
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QVERIFY(recordAt(history, 1, path).isUnknown());
    }

    /* Recording an unknown state must not degrade into "an unreadable file
     * breaks every rewind from now on": an earlier turn still has a real
     * record for that path and a rewind to it still applies it. */
    void testSnapshotUnknownDoesNotShadowEarlierTurns()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree        tree;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-shadow"));
        history.setLiveAccessor(tree.accessor());

        const QString path = QStringLiteral("/vfs/shadow.v");
        tree.files.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        tree.files.insert(path, QStringLiteral("v1"));
        QVERIFY(history.makeSnapshot(1));

        tree.files.insert(path, QStringLiteral("v2"));
        tree.unreadable = true;
        QVERIFY(history.makeSnapshot(2));
        tree.unreadable = false;

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, QStringList{path});
        QVERIFY(report.unknown.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QCOMPARE(tree.files.value(path), QStringLiteral("v0"));
    }

    /* An absent record says "unlink it", but only once the backend confirms
     * there is something to unlink. An existence check that cannot answer
     * must leave the path alone AND say that it did. */
    void testRestoreRefusesUnknownLiveExistence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree        tree;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-exists"));
        history.setLiveAccessor(tree.accessor());

        const QString path = QStringLiteral("/vfs/created.v");
        history.trackEdit(path, false, QString()); /* absent baseline */
        tree.files.insert(path, QStringLiteral("created"));
        QVERIFY(history.makeSnapshot(1));
        QVERIFY(recordAt(history, 0, path).isAbsent());

        tree.unreadable   = true; /* exists() can no longer answer */
        const auto report = history.applySnapshot(0);

        QCOMPARE(tree.removeCalls, 0);
        QCOMPARE(report.unknown, QStringList{path});
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QVERIFY(tree.files.contains(path));
    }

    /* The unknown state has to survive the round trip through
     * snapshots.jsonl, or a resumed session reads it back as absent and
     * deletes on the next rewind. */
    void testUnknownEntrySurvivesReloadFromDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QStringLiteral("/vfs/reload.v");
        {
            FakeTree        tree;
            QSocFileHistory first(dir.path(), QStringLiteral("fence-reload"));
            first.setLiveAccessor(tree.accessor());
            tree.files.insert(path, QStringLiteral("v0"));
            first.trackEdit(path, true, QStringLiteral("v0"));
            tree.files.insert(path, QStringLiteral("v1"));
            tree.unreadable = true;
            QVERIFY(first.makeSnapshot(1));
        }

        FakeTree        tree;
        QSocFileHistory second(dir.path(), QStringLiteral("fence-reload"));
        second.setLiveAccessor(tree.accessor());
        tree.files.insert(path, QStringLiteral("v1"));

        QVERIFY(hasTurn(second, 1));

        const auto report = second.applySnapshot(1);
        QVERIFY(tree.files.contains(path));
        QCOMPARE(tree.files.value(path), QStringLiteral("v1"));
        QCOMPARE(tree.removeCalls, 0);
        QCOMPARE(report.unknown, QStringList{path});
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QVERIFY(recordAt(second, 1, path).isUnknown());
    }

    /* A record captured over a link that has since been replaced describes a
     * tree nobody has observed since. It is left alone unless the caller
     * says otherwise, and an unknown record is left alone either way. */
    void testGenerationBoundaryLeavesFilesAlone()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree        tree;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-boundary"));
        history.setLiveAccessor(tree.accessor());

        const QString kept  = QStringLiteral("/vfs/a-kept.v");
        const QString quiet = QStringLiteral("/vfs/b-quiet.v");
        tree.files.insert(kept, QStringLiteral("v0"));
        tree.files.insert(quiet, QStringLiteral("q0"));
        history.trackEdit(kept, true, QStringLiteral("v0"));
        history.trackEdit(quiet, true, QStringLiteral("q0"));
        tree.files.insert(kept, QStringLiteral("v1"));
        tree.unreadable = true;
        QVERIFY(history.makeSnapshot(1)); /* both recorded unknown */
        tree.unreadable = false;

        tree.gen = 2; /* the link was replaced */

        const auto refused = history.applySnapshot(0);
        QCOMPARE(refused.unknown, (QStringList{kept, quiet}));
        QVERIFY(refused.restored.isEmpty());
        QVERIFY(refused.failed.isEmpty());
        QCOMPARE(tree.files.value(kept), QStringLiteral("v1"));

        /* Allow overrides the boundary heuristic for the baseline records,
         * which are real observations from generation 1. */
        const auto allowed = history.applySnapshot(0, QSocFileHistory::AcrossGeneration::Allow);
        QCOMPARE(allowed.restored, (QStringList{kept, quiet}));
        QCOMPARE(tree.files.value(kept), QStringLiteral("v0"));

        /* An unknown record stays unknown even with Allow: a boundary is a
         * heuristic, an unknown record is a genuine absence of information. */
        const auto forcedUnknown
            = history.applySnapshot(1, QSocFileHistory::AcrossGeneration::Allow);
        QCOMPARE(forcedUnknown.unknown, (QStringList{kept, quiet}));
        QVERIFY(forcedUnknown.restored.isEmpty());
    }

    /* A backend with no notion of a transport reports generation 0, which
     * must disable the boundary check rather than refuse everything. */
    void testGenerationZeroKeepsLegacyRestoresWorking()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree tree;
        tree.reportGeneration = false;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-genzero"));
        history.setLiveAccessor(tree.accessor());

        const QString path = QStringLiteral("/vfs/plain.v");
        tree.files.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        tree.files.insert(path, QStringLiteral("v1"));
        QVERIFY(history.makeSnapshot(1));
        QCOMPARE(recordAt(history, 1, path).generation(), quint64{0});

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, QStringList{path});
        QVERIFY(report.unknown.isEmpty());
        QCOMPARE(tree.files.value(path), QStringLiteral("v0"));
    }

    /* A session written before the "gen" field existed reads as generation
     * 0. Stamping such lines with 1 would make every rewind in every
     * pre-existing remote session refuse. */
    void testLegacyJsonlWithoutGenerationLoads()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path    = QStringLiteral("/vfs/legacy.v");
        const QString content = QStringLiteral("legacy content");
        const QString sha     = QSocFileHistory::sha256Hex(content);
        seedLegacyHistory(
            dir.path(), QStringLiteral("fence-legacy"), {jsonlLine(0, path, sha, true)}, {content});

        FakeTree tree;
        tree.gen = 7; /* a live link, several reconnects along */
        tree.files.insert(path, QStringLiteral("changed"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-legacy"));
        history.setLiveAccessor(tree.accessor());

        QVERIFY(recordAt(history, 0, path).isPresent());

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, QStringList{path});
        QVERIFY(report.unknown.isEmpty());
        QCOMPARE(tree.files.value(path), content);
        QCOMPARE(recordAt(history, 0, path).generation(), quint64{0});
    }

    /* A digest that is not one classifies as unknown, not as a blob we could
     * have read: a torn line must not be actionable in either direction. */
    void testGarbageShaLoadsAsUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QStringLiteral("/vfs/torn.v");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-torn"),
            {jsonlLine(0, path, QStringLiteral("not-a-sha"), true),
             jsonlLine(1, path, QStringLiteral("unknown"), true),
             jsonlLine(2, path, QStringLiteral("42"), false)},
            {});

        FakeTree tree;
        tree.files.insert(path, QStringLiteral("live"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-torn"));
        history.setLiveAccessor(tree.accessor());

        QVERIFY(recordAt(history, 0, path).isUnknown());
        QVERIFY(recordAt(history, 1, path).isUnknown());
        QVERIFY(recordAt(history, 2, path).isUnknown());

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.unknown, QStringList{path});
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QCOMPARE(tree.removeCalls, 0);
        QCOMPARE(tree.files.value(path), QStringLiteral("live"));
    }

    /* An operation that starts on one link and finishes on another describes
     * neither tree. A restore stops at the change and names every path it
     * did not get to; a capture writes no turn at all. */
    void testTransportChangeMidOperationStopsCleanly()
    {
        const QString first  = QStringLiteral("/vfs/a.v");
        const QString second = QStringLiteral("/vfs/b.v");
        const QString third  = QStringLiteral("/vfs/c.v");

        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            FakeTree        tree;
            QSocFileHistory history(dir.path(), QStringLiteral("fence-straddle-restore"));
            history.setLiveAccessor(tree.accessor());
            for (const QString &path : {first, second, third}) {
                tree.files.insert(path, QStringLiteral("v0"));
                history.trackEdit(path, true, QStringLiteral("v0"));
                tree.files.insert(path, QStringLiteral("v1"));
            }
            QVERIFY(history.makeSnapshot(1));

            tree.bumpOnWrite  = 1; /* the link is replaced during write #1 */
            const auto report = history.applySnapshot(0);

            QCOMPARE(report.restored, QStringList{first});
            QCOMPARE(report.failed, (QStringList{second, third}));
            QVERIFY(report.unknown.isEmpty());
            QVERIFY(report.transportChanged);
            QCOMPARE(tree.writeCalls, 1);
            QCOMPARE(tree.files.value(first), QStringLiteral("v0"));
            QCOMPARE(tree.files.value(second), QStringLiteral("v1"));
            QCOMPARE(tree.files.value(third), QStringLiteral("v1"));
        }

        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            FakeTree        tree;
            QSocFileHistory history(dir.path(), QStringLiteral("fence-straddle-capture"));
            history.setLiveAccessor(tree.accessor());
            for (const QString &path : {first, second, third}) {
                tree.files.insert(path, QStringLiteral("v0"));
                history.trackEdit(path, true, QStringLiteral("v0"));
            }

            /* A file this turn created. Its turn-1 record is absent, which is
             * an instruction to unlink, so it is the file a missing turn-2
             * entry would destroy. */
            const QString born = QStringLiteral("/vfs/born.v");
            history.trackEdit(born, false, QString());
            QVERIFY(history.makeSnapshot(1));
            tree.files.insert(born, QStringLiteral("new"));

            /* readCalls is cumulative and the turn-1 capture already spent
             * some, so aim at the next read rather than at read #1. */
            tree.bumpOnRead = tree.readCalls + 1;
            /* Deliberately unasserted: the damage below is what this slot is
             * for, and a QVERIFY here would abort before reaching it. */
            history.makeSnapshot(2);

            /* Nothing the straddled capture touched may be acted on, and the
             * entry has to exist so a rewind here does not fall back to
             * turn 1 and unlink a file turn 2 created. Allow is what makes
             * this observable: it waives the generation boundary, which
             * otherwise masks the missing entry, and it must NOT waive an
             * unknown record. */
            const auto report = history.applySnapshot(2, QSocFileHistory::AcrossGeneration::Allow);
            QVERIFY2(
                tree.files.contains(born),
                "a rewind to the straddled turn deleted a file that turn created");
            QCOMPARE(tree.files.value(born), QStringLiteral("new"));
            QVERIFY(hasTurn(history, 2));
            QVERIFY(report.restored.isEmpty());
            QVERIFY(report.failed.isEmpty());
            QVERIFY(report.unknown.contains(born));
        }
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocfilehistoryfence.moc"
