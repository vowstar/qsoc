// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"
#include "agent/qsocsession.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using json = nlohmann::json;

/**
 * @brief In-memory stand-in for a working tree behind a replaceable link.
 * @details Holds the files, the tree identity and transport generation the
 *          accessor reports, and a switch that makes reads and stats fail the
 *          way a link that went quiet does. Two instances with different names
 *          stand in for two sides of a /ssh or /local swap. Nothing here
 *          touches the network or the disk.
 */
class FakeTree
{
public:
    QHash<QString, QString> files;
    QString                 name        = QStringLiteral("tree-one");
    quint64                 gen         = 1;
    bool                    unreadable  = false;
    int                     removeCalls = 0;
    int                     writeCalls  = 0;
    int                     readCalls   = 0;
    /* Bump the generation from inside the Nth write / read, so a capture or
     * a restore can be made to straddle two transports. 0 disables. */
    int bumpOnWrite = 0;
    int bumpOnRead  = 0;
    /* Whether the accessor advertises an identity at all. */
    bool reportGeneration = true;
    bool reportTree       = true;

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
        if (reportTree) {
            acc.tree = [this]() { return name; };
        }
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
    /* One snapshots.jsonl line. An empty tree writes no "tree" key, which is
     * exactly what a session recorded before the field existed holds. */
    static QString jsonlLine(
        int            turn,
        const QString &path,
        const QString &value,
        bool           quoted,
        const QString &tree = QString(),
        quint64        gen  = 0)
    {
        const QString encoded = quoted ? QStringLiteral("\"%1\"").arg(value) : value;
        const QString epoch   = tree.isEmpty()
                                    ? QString()
                                    : QStringLiteral(R"("tree":"%1","gen":%2,)").arg(tree).arg(gen);
        return QStringLiteral(R"({"turn":%1,"ts":"2026-01-01T00:00:00.000Z",%2"files":{"%3":%4}})")
            .arg(turn)
            .arg(epoch, path, encoded);
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

    /* A record captured over a link to this same tree that has since been
     * replaced describes a tree nobody has observed since. It is left alone
     * unless the caller says otherwise, and an unknown record is left alone
     * either way. */
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

    /* A backend that numbers no connection still names one tree, so its own
     * records stay restorable: an unnumbered link must not read as a boundary.
     * This replaces the expectation that generation 0 disables the check
     * globally, which is what let a record captured on one tree be acted on
     * over another whenever either side reported 0. */
    void testBackendWithoutGenerationRestoresItsOwnRecords()
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
        QCOMPARE(recordAt(history, 1, path).epoch().link, quint64{0});
        QCOMPARE(recordAt(history, 1, path).epoch().tree, tree.name);

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, QStringList{path});
        QVERIFY(report.unknown.isEmpty());
        QCOMPARE(tree.files.value(path), QStringLiteral("v0"));
    }

    /* A backend that names no tree at all is one unnamed tree, not a licence
     * to act on anything: its records are still restorable through it. */
    void testBackendWithoutTreeRestoresItsOwnRecords()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree tree;
        tree.reportTree       = false;
        tree.reportGeneration = false;
        QSocFileHistory history(dir.path(), QStringLiteral("fence-nameless"));
        history.setLiveAccessor(tree.accessor());

        const QString path = QStringLiteral("/vfs/nameless.v");
        tree.files.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        tree.files.insert(path, QStringLiteral("v1"));
        QVERIFY(history.makeSnapshot(1));
        QVERIFY(!recordAt(history, 1, path).epoch().tree.isEmpty());

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, QStringList{path});
        QCOMPARE(tree.files.value(path), QStringLiteral("v0"));
    }

    /* A session written before the "gen" field existed reads as generation 0,
     * and on the tree those paths point at that must not fence it: stamping
     * such lines with 1, or judging them by the live link count, would make
     * every pre-existing local rewind refuse. Bound to the local disk because
     * that is the only tree a tree-less line may be acted on; the refusal
     * elsewhere is testLegacyRecordNeverActsOnAnotherTree. */
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
        tree.name = QStringLiteral("local");
        tree.gen  = 7; /* a live link, several reconnects along */
        tree.files.insert(path, QStringLiteral("changed"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-legacy"));
        history.setLiveAccessor(tree.accessor());

        QVERIFY(recordAt(history, 0, path).isPresent());

        /* The line loads, but a tree-less record acts on no tree: its overwrite
         * is refused and the path is reported as a boundary, not restored. */
        const auto report = history.applySnapshot(0);
        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.unknown.isEmpty());
        QCOMPARE(tree.files.value(path), QStringLiteral("changed"));
        QCOMPARE(history.previewBoundary(0).otherTree, QStringList{path});
        QCOMPARE(recordAt(history, 0, path).epoch().link, quint64{0});
        QVERIFY(recordAt(history, 0, path).epoch().tree.isEmpty());
    }

    /* COUNTEREXAMPLE, promotion on capture. A resumed local session must not
     * launder a tree-less record into an actionable one by re-capturing its
     * path under the live tree. If it did, a rewind to that new turn would act
     * in all three directions the record can name: overwrite a changed file,
     * create an absent one, delete a present one. The snapshot must skip these
     * paths, so the later turn holds no local record for them and touches
     * nothing. A path the session genuinely edits is captured as normal. */
    void testTreelessPathsAreNotPromotedOnSnapshot()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString overwrite = QStringLiteral("/vfs/a-overwrite.v");
        const QString create    = QStringLiteral("/vfs/b-create.v");
        const QString remove    = QStringLiteral("/vfs/c-remove.v");
        const QString edited    = QStringLiteral("/vfs/d-edited.v");
        const QString body      = QStringLiteral("legacy body");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-promote"),
            {jsonlLine(0, overwrite, QSocFileHistory::sha256Hex(body), true),
             jsonlLine(0, create, QSocFileHistory::sha256Hex(body), true),
             jsonlLine(0, remove, QStringLiteral("null"), false)},
            {body});

        FakeTree tree;
        tree.name             = QStringLiteral("local");
        tree.reportGeneration = false;
        tree.files.insert(overwrite, QStringLiteral("changed"));
        /* create is absent on the live tree; remove is present. */
        tree.files.insert(remove, QStringLiteral("still here"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-promote"));
        history.setLiveAccessor(tree.accessor());

        /* A genuine edit this session: this path is tracked under the live tree
         * and must still be captured, proving the fix fences promotion without
         * stopping real checkpointing. */
        tree.files.insert(edited, QStringLiteral("fresh"));
        history.trackEdit(edited, false, QString());

        QVERIFY(history.makeSnapshot(1));
        /* No tree-less path was promoted: turn 1 holds no record for them. */
        QVERIFY(!recordAt(history, 1, overwrite).isPresent());
        QVERIFY(!recordAt(history, 1, create).isPresent());
        QVERIFY(recordAt(history, 1, remove).isUnknown());
        /* The genuinely edited path is captured, under the live tree. */
        QVERIFY(recordAt(history, 1, edited).isPresent());
        QCOMPARE(recordAt(history, 1, edited).epoch().tree, QStringLiteral("local"));

        /* Applying the promoted turn acts on nothing: all three live files as
         * they stood, no unlink, no create, no overwrite. */
        const auto report = history.applySnapshot(1);
        QCOMPARE(tree.files.value(overwrite), QStringLiteral("changed"));
        QVERIFY(!tree.files.contains(create));
        QCOMPARE(tree.files.value(remove), QStringLiteral("still here"));
        QCOMPARE(report.restored, QStringList{edited});
        QCOMPARE(tree.removeCalls, 0);

        /* The turn-0 line on disk still names no tree: fenced, never rewritten
         * into a claim it cannot back up. */
        QVERIFY(recordAt(history, 0, overwrite).epoch().tree.isEmpty());
    }

    /* COUNTEREXAMPLE. A record written before the tree field existed names no
     * namespace, so nothing on disk says it was captured where it is being
     * applied. Acting on it over a remote workspace unlinks a file whose only
     * connection to the record is a shared absolute path. */
    void testLegacyRecordNeverActsOnAnotherTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QStringLiteral("/proj/keep.v");
        /* A pre-upgrade session created this path, so its baseline records "not
         * there", which is an instruction to unlink. */
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-legacy-cross"),
            {jsonlLine(0, path, QStringLiteral("null"), false)},
            {});

        FakeTree remote;
        remote.name = QStringLiteral("ssh:host") + QChar(0x1F) + QStringLiteral("workspace");
        remote.gen  = 1;
        remote.files.insert(path, QStringLiteral("remote body"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-legacy-cross"));
        history.setLiveAccessor(remote.accessor());

        /* The line still loads; only where it may be acted on changes. */
        QVERIFY(recordAt(history, 0, path).isAbsent());
        QVERIFY(recordAt(history, 0, path).epoch().tree.isEmpty());

        const auto refused = history.applySnapshot(0);
        /* Damage first: the file is what a rewind may not destroy. */
        QVERIFY2(
            remote.files.contains(path),
            "a rewind deleted a remote file from a record that names no tree");
        QCOMPARE(remote.files.value(path), QStringLiteral("remote body"));
        QCOMPARE(remote.removeCalls, 0);

        /* Not a boundary anyone can waive on the user's behalf: the path may
         * denote an unrelated file here. */
        const auto forced = history.applySnapshot(0, QSocFileHistory::AcrossGeneration::Allow);
        QVERIFY2(remote.files.contains(path), "Allow deleted a remote file from a tree-less record");
        QCOMPARE(remote.removeCalls, 0);

        QVERIFY(refused.restored.isEmpty());
        QVERIFY(refused.failed.isEmpty());
        QVERIFY(forced.restored.isEmpty());
        QCOMPARE(history.previewBoundary(0).otherTree, QStringList{path});
        QCOMPARE(
            QSocFileHistory::relate(QSocFileHistory::Epoch{}, QSocFileHistory::Epoch{remote.name, 1}),
            QSocFileHistory::EpochRelation::OtherTree);
    }

    /* COUNTEREXAMPLE. A record that names no tree has no proven origin, so it
     * acts on no tree at all, the local disk included. The unlink of an absent
     * record and the overwrite of a present one both stop at a colliding local
     * path, Allow or not, and every left path is reported as a boundary rather
     * than restored. No session state is consulted: the fence is the record's
     * own emptiness, so compaction, restart, or an /ssh then /local cannot
     * erase it. The cost is that a purely-local pre-upgrade session loses
     * automatic rewind of these records, an explicit refusal in place of a
     * silent, unrecoverable deletion. */
    void testLegacyRecordNeverActsOnLocalTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString kept    = QStringLiteral("/proj/a-restore.v");
        const QString created = QStringLiteral("/proj/b-created.v");
        const QString content = QStringLiteral("pre-upgrade body");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-legacy-local"),
            {jsonlLine(0, kept, QSocFileHistory::sha256Hex(content), true),
             jsonlLine(0, created, QStringLiteral("null"), false)},
            {content});

        FakeTree local;
        local.name             = QStringLiteral("local");
        local.reportGeneration = false; /* the local disk numbers no link */
        local.files.insert(kept, QStringLiteral("changed"));
        local.files.insert(created, QStringLiteral("created later"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-legacy-local"));
        history.setLiveAccessor(local.accessor());
        /* No session-state setup at all: nothing latches the fence on. */

        const auto report = history.applySnapshot(0);

        /* Damage first: neither the absent record's unlink nor the present
         * record's overwrite may reach a colliding local path. */
        QVERIFY2(
            local.files.contains(created),
            "a rewind deleted a local file from a record that names no tree");
        QCOMPARE(local.files.value(created), QStringLiteral("created later"));
        QVERIFY2(
            local.files.contains(kept),
            "a rewind overwrote a local file from a record that names no tree");
        QCOMPARE(local.files.value(kept), QStringLiteral("changed"));
        QCOMPARE(local.removeCalls, 0);
        QCOMPARE(local.writeCalls, 0);

        /* Allow waives a replaced link, never an unproven tree. */
        const auto forced = history.applySnapshot(0, QSocFileHistory::AcrossGeneration::Allow);
        QCOMPARE(local.removeCalls, 0);
        QCOMPARE(local.writeCalls, 0);
        QVERIFY(forced.restored.isEmpty());

        QVERIFY(report.restored.isEmpty());
        QVERIFY(report.failed.isEmpty());
        /* Not silently swallowed: the user is told these paths are a boundary. */
        QCOMPARE(history.previewBoundary(0).otherTree, (QStringList{kept, created}));
        QVERIFY(history.previewBoundary(0).otherLink.isEmpty());
        QCOMPARE(
            QSocFileHistory::relate(
                QSocFileHistory::Epoch{}, QSocFileHistory::Epoch{QStringLiteral("local"), 0}),
            QSocFileHistory::EpochRelation::OtherTree);
    }

    /* The fence is only for records that name no tree. A record that does name
     * the local tree has a proven origin, so a purely-local session written
     * under this binary still restores and deletes its own files as before. */
    void testLocalTreeRecordStillRewinds()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString kept    = QStringLiteral("/proj/a-restore.v");
        const QString created = QStringLiteral("/proj/b-created.v");
        const QString content = QStringLiteral("local body");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-local-named"),
            {jsonlLine(0, kept, QSocFileHistory::sha256Hex(content), true, QStringLiteral("local")),
             jsonlLine(0, created, QStringLiteral("null"), false, QStringLiteral("local"))},
            {content});

        FakeTree local;
        local.name             = QStringLiteral("local");
        local.reportGeneration = false;
        local.files.insert(kept, QStringLiteral("changed"));
        local.files.insert(created, QStringLiteral("created later"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-local-named"));
        history.setLiveAccessor(local.accessor());

        const auto report = history.applySnapshot(0);
        QCOMPARE(report.restored, (QStringList{kept, created}));
        QVERIFY(report.unknown.isEmpty());
        QVERIFY(report.failed.isEmpty());
        QCOMPARE(local.files.value(kept), content);
        QVERIFY(!local.files.contains(created));
        QCOMPARE(local.removeCalls, 1);
        QVERIFY(history.previewBoundary(0).isEmpty());
        QCOMPARE(
            QSocFileHistory::relate(
                QSocFileHistory::Epoch{QStringLiteral("local"), 0},
                QSocFileHistory::Epoch{QStringLiteral("local"), 0}),
            QSocFileHistory::EpochRelation::Same);
    }

    /* COUNTEREXAMPLE, capture side. A path carried over from a tree-less
     * snapshot must not be read on a remote workspace either: that read records
     * a state for a path the agent never edited there, and a rewind to that
     * turn then unlinks whatever the user keeps at it. */
    void testLegacyPathIsNotCapturedOnAnotherTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path    = QStringLiteral("/proj/only-local.v");
        const QString content = QStringLiteral("pre-upgrade body");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-legacy-capture"),
            {jsonlLine(0, path, QSocFileHistory::sha256Hex(content), true)},
            {content});

        FakeTree remote;
        remote.name = QStringLiteral("ssh:host") + QChar(0x1F) + QStringLiteral("workspace");
        QSocFileHistory history(dir.path(), QStringLiteral("fence-legacy-capture"));
        history.setLiveAccessor(remote.accessor());

        QVERIFY(history.makeSnapshot(1));

        /* The user's own file turns up at that path on the workspace. */
        remote.files.insert(path, QStringLiteral("theirs"));
        const auto report = history.applySnapshot(1);

        /* Damage first. */
        QVERIFY2(
            remote.files.contains(path),
            "a rewind deleted a remote file the agent never edited on that tree");
        QCOMPARE(remote.files.value(path), QStringLiteral("theirs"));
        QCOMPARE(remote.removeCalls, 0);
        QVERIFY(report.restored.isEmpty());
        /* No record was invented for that tree in the first place. */
        QCOMPARE(remote.readCalls, 0);
        QVERIFY(!hasTurn(history, 1));

        /* Once the agent does edit that path here, it is this tree's path: it
         * gets its own baseline from the state observed here, and the tree-less
         * record stays the local disk's business. */
        history.trackEdit(path, true, QStringLiteral("theirs"));
        bool baselineHere = false;
        bool legacyIntact = false;
        for (const auto &snap : history.listSnapshots()) {
            if (snap.turn != 0 || !snap.files.contains(path)) {
                continue;
            }
            baselineHere = baselineHere || snap.epoch.tree == remote.name;
            legacyIntact = legacyIntact || snap.epoch.tree.isEmpty();
        }
        QVERIFY(baselineHere);
        QVERIFY(legacyIntact);
        remote.files.insert(path, QStringLiteral("edited"));
        const auto second = history.applySnapshot(0);
        QCOMPARE(second.restored, QStringList{path});
        QCOMPARE(remote.files.value(path), QStringLiteral("theirs"));
    }

    /* A digest that is not one classifies as unknown, not as a blob we could
     * have read: a torn line must not be actionable in either direction. The
     * records name the live tree so they are in scope and their classification,
     * not the tree fence, is what decides the outcome. */
    void testGarbageShaLoadsAsUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QStringLiteral("/vfs/torn.v");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-torn"),
            {jsonlLine(0, path, QStringLiteral("not-a-sha"), true, QStringLiteral("local")),
             jsonlLine(1, path, QStringLiteral("unknown"), true, QStringLiteral("local")),
             jsonlLine(2, path, QStringLiteral("42"), false, QStringLiteral("local"))},
            {});

        FakeTree tree;
        tree.name             = QStringLiteral("local");
        tree.reportGeneration = false;
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

    /* COUNTEREXAMPLE. A checkpoint taken through one accessor must never
     * delete a file reached through another. The shipped code read a local
     * absolute path over SFTP, got a confident "not there", recorded it as an
     * instruction to unlink, and the numeric guard waived itself because the
     * local side reports no generation. */
    void testRecordFromAnotherTreeNeverDeletesThisTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path    = QStringLiteral("/local/proj/keep.v");
        const QString content = QStringLiteral("local v0");
        /* Turn 0 observed on the local disk, turn 1 on a remote workspace that
         * cannot see the path. Written by hand because this is the shape the
         * shipped code produced. */
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-crossing"),
            {jsonlLine(0, path, QSocFileHistory::sha256Hex(content), true, QStringLiteral("local"), 0),
             jsonlLine(1, path, QStringLiteral("null"), false, QStringLiteral("ssh:workspace"), 1)},
            {content});

        FakeTree local;
        local.name             = QStringLiteral("local");
        local.reportGeneration = false; /* the local disk numbers no link */
        local.files.insert(path, QStringLiteral("local v1"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-crossing"));
        history.setLiveAccessor(local.accessor());

        const auto report = history.applySnapshot(1);

        /* Damage first: the file is what a rewind may not destroy. */
        QVERIFY2(
            local.files.contains(path),
            "a rewind deleted a local file because another tree could not see it");
        QCOMPARE(local.removeCalls, 0);
        /* The remote record is dropped, so the path falls back to the last
         * state anyone observed on this tree. */
        QCOMPARE(local.files.value(path), content);
        QCOMPARE(report.restored, QStringList{path});
        QVERIFY(report.failed.isEmpty());
    }

    /* COUNTEREXAMPLE, capture side. A turn that runs on one tree must not read
     * paths belonging to another: that read is where the bogus "not there"
     * came from, and no guard is needed for a record that never exists. */
    void testTurnOnAnotherTreeReadsNothingFromThisOne()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeTree local;
        local.name             = QStringLiteral("local");
        local.reportGeneration = false;
        FakeTree remote;
        remote.name = QStringLiteral("ssh:workspace");

        QSocFileHistory history(dir.path(), QStringLiteral("fence-capture-crossing"));
        history.setLiveAccessor(local.accessor());

        const QString path = QStringLiteral("/local/proj/only-local.v");
        local.files.insert(path, QStringLiteral("v0"));
        history.trackEdit(path, true, QStringLiteral("v0"));
        local.files.insert(path, QStringLiteral("v1"));
        QVERIFY(history.makeSnapshot(1));

        /* /ssh, then a turn ends on the other side. */
        history.setLiveAccessor(remote.accessor());
        QVERIFY(history.makeSnapshot(2));

        QCOMPARE(remote.readCalls, 0);
        QVERIFY(!hasTurn(history, 2));

        /* /local, then a rewind to that turn. */
        history.setLiveAccessor(local.accessor());
        const auto report = history.applySnapshot(2);

        QVERIFY2(local.files.contains(path), "a rewind deleted a file the other tree never saw");
        QCOMPARE(local.removeCalls, 0);
        QCOMPARE(local.files.value(path), QStringLiteral("v1"));
        QCOMPARE(report.restored, QStringList{path});
    }

    /* A record from another tree is not a boundary the user can waive: unlike
     * a replaced link to the same tree, the paths may denote different files. */
    void testAllowCannotReachAnotherTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QStringLiteral("/w/shared-name.v");
        seedLegacyHistory(
            dir.path(),
            QStringLiteral("fence-allow-tree"),
            {jsonlLine(0, path, QStringLiteral("null"), false, QStringLiteral("ssh:workspace"), 1)},
            {});

        FakeTree local;
        local.name             = QStringLiteral("local");
        local.reportGeneration = false;
        local.files.insert(path, QStringLiteral("mine"));
        QSocFileHistory history(dir.path(), QStringLiteral("fence-allow-tree"));
        history.setLiveAccessor(local.accessor());

        const auto forced = history.applySnapshot(0, QSocFileHistory::AcrossGeneration::Allow);

        QVERIFY2(local.files.contains(path), "Allow deleted a file recorded on another tree");
        QCOMPARE(local.removeCalls, 0);
        QVERIFY(forced.restored.isEmpty());
        QVERIFY(forced.failed.isEmpty());
        /* Nothing on this tree was ever recorded for the path, so there is no
         * state to report; the notice read before the restore is what names it. */
        QVERIFY(forced.unknown.isEmpty());
        QCOMPARE(history.previewBoundary(0).otherTree, QStringList{path});
        QVERIFY(history.previewBoundary(0).otherLink.isEmpty());
        QCOMPARE(
            QSocFileHistory::relate(
                QSocFileHistory::Epoch{QStringLiteral("ssh:workspace"), 1},
                QSocFileHistory::Epoch{QStringLiteral("local"), 0}),
            QSocFileHistory::EpochRelation::OtherTree);
    }

    /* A failed stat says nothing about existence, and the local read path must
     * reach the same conclusion as the local existence check: deciding
     * "absent" from a stat that could not answer records an unlink. */
    void testLocalReadWithoutStatPermissionIsUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto accessor = QSocFileHistory::localAccessor();

        /* The answerable cases first, on every platform. */
        const QString present = QDir(dir.path()).filePath(QStringLiteral("there.v"));
        QFile         file(present);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("body");
        file.close();
        QCOMPARE(accessor.exists(present), QSocFileHistory::FileState::Present);
        QCOMPARE(accessor.read(present).state(), QSocFileHistory::FileState::Present);
        const QString missing = QDir(dir.path()).filePath(QStringLiteral("gone.v"));
        QCOMPARE(accessor.exists(missing), QSocFileHistory::FileState::Absent);
        QCOMPARE(accessor.read(missing).state(), QSocFileHistory::FileState::Absent);

#ifdef Q_OS_UNIX
        if (geteuid() == 0) {
            QSKIP("running as root: permission bits do not stop a stat");
        }
        const QString locked = QDir(dir.path()).filePath(QStringLiteral("locked"));
        QVERIFY(QDir().mkpath(locked));
        const QString hidden = QDir(locked).filePath(QStringLiteral("hidden.v"));
        QFile         inner(hidden);
        QVERIFY(inner.open(QIODevice::WriteOnly | QIODevice::Truncate));
        inner.write("still here");
        inner.close();
        QVERIFY(QFile::setPermissions(locked, QFileDevice::Permissions()));

        const auto blind = accessor.read(hidden);
        /* Put the directory back before asserting, or a failure leaks a
         * directory QTemporaryDir cannot remove. */
        QVERIFY(
            QFile::setPermissions(
                locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QCOMPARE(blind.state(), QSocFileHistory::FileState::Unknown);
        QVERIFY(QFile::exists(hidden));
#endif
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
