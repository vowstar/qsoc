// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"
#include "agent/qsocrewind.h"
#include "agent/qsocsession.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

#include <memory>

using json = nlohmann::json;

namespace {

/**
 * @brief In-memory working tree standing in for the live file backend.
 * @details Nothing here touches the network or the real filesystem, so the
 *          paths are free to look like a remote workspace. `failWrite` makes
 *          one path refuse its write the way an SFTP write to a full or
 *          read-only tree does; `bumpOnWrite` replaces the transport from
 *          inside the Nth write, which is how a restore is made to straddle
 *          two links.
 */
class FakeTree
{
public:
    QHash<QString, QString> files;
    QSet<QString>           failWrite;
    QSet<QString>           unreadable;
    quint64                 gen         = 1;
    int                     bumpOnWrite = 0;
    int                     writeCalls  = 0;
    bool                    covered     = true;

    QSocFileHistory::LiveFileAccessor accessor()
    {
        QSocFileHistory::LiveFileAccessor acc;
        acc.exists = [this](const QString &path) {
            if (unreadable.contains(path)) {
                return QSocFileHistory::FileState::Unknown;
            }
            return files.contains(path) ? QSocFileHistory::FileState::Present
                                        : QSocFileHistory::FileState::Absent;
        };
        acc.read = [this](const QString &path) {
            if (unreadable.contains(path)) {
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
            if (failWrite.contains(path)) {
                return false;
            }
            files.insert(path, content);
            return true;
        };
        acc.remove = [this](const QString &path) {
            files.remove(path);
            return true;
        };
        acc.inScope    = [](const QString &) { return true; };
        acc.coversPath = [this](const QString &) { return covered; };
        acc.tree       = []() { return QStringLiteral("fake-tree"); };
        acc.generation = [this]() { return QString::number(gen); };
        return acc;
    }
};

const QString kPathA = QStringLiteral("/w/a.sv");
const QString kPathB = QStringLiteral("/w/b.sv");
const QString kPathC = QStringLiteral("/w/c.sv");

class Test : public QObject
{
    Q_OBJECT

private:
    /* Real temp project, real session file, real snapshots.jsonl. */
    std::unique_ptr<QTemporaryDir>   dir;
    std::unique_ptr<QSocSession>     session;
    std::unique_ptr<QSocFileHistory> history;
    FakeTree                         tree;
    QDateTime                        createdAt;

    static QString readAll(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    static json userMessage(const QString &text)
    {
        json msg;
        msg["role"]    = "user";
        msg["content"] = text.toStdString();
        return msg;
    }

    static json assistantMessage(const QString &text)
    {
        json msg;
        msg["role"]    = "assistant";
        msg["content"] = text.toStdString();
        return msg;
    }

    /* The two messages that survive a rewind to the second user turn. */
    json keptTwo() const
    {
        json kept = json::array();
        kept.push_back(userMessage(QStringLiteral("first ask")));
        kept.push_back(assistantMessage(QStringLiteral("first answer")));
        return kept;
    }

    /* A full rewind to the second user message: turn 2, so snapshot 1. */
    QSocRewindRequest fullRewind() const
    {
        QSocRewindRequest request;
        request.restoreConversation = true;
        request.restoreFiles        = true;
        request.targetSnapshot      = 1;
        request.keptMessages        = keptTwo();
        request.originalCreatedAt   = createdAt;
        return request;
    }

    int snapshotHighWater() const
    {
        int highest = 0;
        for (const auto &snap : history->listSnapshots()) {
            highest = qMax(highest, snap.turn);
        }
        return highest;
    }

private slots:
    void init()
    {
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        const QString projectPath = dir->path();
        const QString sessionId   = QStringLiteral("rewind-fixture");
        createdAt
            = QDateTime::fromString(QStringLiteral("2026-01-02T03:04:05.000Z"), Qt::ISODateWithMs);
        QVERIFY(createdAt.isValid());

        QDir().mkpath(QSocSession::sessionsDir(projectPath));
        session = std::make_unique<QSocSession>(
            sessionId,
            QDir(QSocSession::sessionsDir(projectPath))
                .filePath(sessionId + QStringLiteral(".jsonl")));
        QVERIFY(
            session->appendMeta(QStringLiteral("created"), createdAt.toString(Qt::ISODateWithMs)));
        QVERIFY(session->appendMessage(userMessage(QStringLiteral("first ask"))));
        QVERIFY(session->appendMessage(assistantMessage(QStringLiteral("first answer"))));
        QVERIFY(session->appendMessage(userMessage(QStringLiteral("second ask"))));
        QVERIFY(session->appendMessage(assistantMessage(QStringLiteral("second answer"))));
        QCOMPARE(QSocSession::loadMessages(session->filePath()).size(), std::size_t{4});

        tree    = FakeTree();
        history = std::make_unique<QSocFileHistory>(projectPath, sessionId);
        history->setLiveAccessor(tree.accessor());

        /* Baseline (turn 0): a was there, b was not, c was there. */
        tree.files.insert(kPathA, QStringLiteral("base A"));
        tree.files.insert(kPathC, QStringLiteral("base C"));
        history->trackEdit(kPathA, true, QStringLiteral("base A"));
        history->trackEdit(kPathB, false, QString());
        history->trackEdit(kPathC, true, QStringLiteral("base C"));

        /* Turn 1: the state a rewind to the second user message targets. c
         * goes quiet during the capture, so its record is unknown and no
         * restore may act on it. */
        tree.files.insert(kPathA, QStringLiteral("turn1 A"));
        tree.files.insert(kPathB, QStringLiteral("turn1 B"));
        tree.unreadable.insert(kPathC);
        QVERIFY(!history->makeSnapshot(1));
        tree.unreadable.remove(kPathC);

        /* Turn 2: the future a full rewind orphans. */
        tree.files.insert(kPathA, QStringLiteral("turn2 A"));
        tree.files.insert(kPathB, QStringLiteral("turn2 B"));
        QVERIFY(history->makeSnapshot(2));
        QCOMPARE(snapshotHighWater(), 2);
    }

    void cleanup()
    {
        history.reset();
        session.reset();
        dir.reset();
    }

    /* SPECIFICATION test, not a counterexample: qsocApplyRewind does not
     * exist before this change, so this slot cannot be run against the old
     * tree at all. It pins the ordering the shipped handler got wrong.
     *
     * A refusal from the workspace gate must be a genuine no-op. The shipped
     * handler rewrote the session JSONL first and refused afterwards, which
     * destroyed the conversation and restored nothing. */
    void refusedRewindLeavesTheSessionAndTreeUntouched()
    {
        const QString before   = readAll(session->filePath());
        const auto    treeCopy = tree.files;

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), []() {
                  return QStringLiteral("no remote workspace is bound");
              });

        /* Damage first: nothing may have moved. */
        QCOMPARE(readAll(session->filePath()), before);
        QCOMPARE(QSocSession::loadMessages(session->filePath()).size(), std::size_t{4});
        QCOMPARE(tree.files, treeCopy);
        QCOMPARE(tree.writeCalls, 0);
        QCOMPARE(snapshotHighWater(), 2);

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Refused);
        QCOMPARE(result.kept, 0);
        QVERIFY(result.files.isEmpty());
        QCOMPARE(
            qsocRewindReport(fullRewind(), result),
            QStringLiteral(
                "\n(Rewind cancelled: no remote workspace is bound. Nothing was "
                "changed.)\n"));
    }

    void fullRewindWithoutFileHistoryRefusesBeforeMutation()
    {
        const QString beforeSession = readAll(session->filePath());
        const auto    beforeTree    = tree.files;
        const int     beforeHigh    = snapshotHighWater();

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), nullptr, QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Refused);
        QVERIFY(result.refusal.contains(QStringLiteral("file history")));
        QCOMPARE(readAll(session->filePath()), beforeSession);
        QCOMPARE(tree.files, beforeTree);
        QCOMPARE(tree.writeCalls, 0);
        QCOMPARE(snapshotHighWater(), beforeHigh);
    }

    void uncoveredCheckpointRefusesBeforeConversationRewrite()
    {
        const QString beforeSession = readAll(session->filePath());
        const auto    beforeTree    = tree.files;
        tree.covered                = false;

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Refused);
        QVERIFY(result.refusal.contains(QStringLiteral("working tree")));
        QCOMPARE(readAll(session->filePath()), beforeSession);
        QCOMPARE(tree.files, beforeTree);
        QCOMPARE(tree.writeCalls, 0);
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. rewriteMessages is QSaveFile-backed, so a write that fails leaves
     * the session byte-identical, which is what makes it the last step
     * allowed to refuse. */
    void refusedSessionWriteReportsCancelledWithNothingChanged()
    {
        /* A session whose parent path is a regular file: the directory can
         * never be created, so every rewrite fails. */
        const QString blocker = QDir(dir->path()).filePath(QStringLiteral("blocker"));
        QFile         blockerFile(blocker);
        QVERIFY(blockerFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        blockerFile.write("not a directory");
        blockerFile.close();
        QSocSession
            doomed(QStringLiteral("doomed"), QDir(blocker).filePath(QStringLiteral("doomed.jsonl")));

        const auto treeCopy = tree.files;

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), &doomed, history.get(), QSocRewindFileGate());

        /* Damage first: the gate passed, so the restore must not have run. */
        QCOMPARE(tree.files, treeCopy);
        QCOMPARE(tree.writeCalls, 0);
        QCOMPARE(snapshotHighWater(), 2);
        QVERIFY(!QFile::exists(QDir(blocker).filePath(QStringLiteral("doomed.jsonl"))));

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Refused);
        QCOMPARE(
            qsocRewindReport(fullRewind(), result),
            QStringLiteral(
                "\n(Rewind cancelled: the conversation could not be written to the "
                "session file. Nothing was changed.)\n"));
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. A path the accessor refused to write is not a cancellation and
     * not a silently smaller success; it has to be named. `unknown` is
     * reported separately because nothing was attempted on it. */
    void partialRestoreReportsPartialAndNamesTheFilesThatDidNotMove()
    {
        tree.failWrite.insert(kPathB);

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QVERIFY(!qsocRewindMovesTurnCounter(fullRewind(), result));
        QCOMPARE(result.files.restored, QStringList{kPathA});
        QCOMPARE(result.files.failed, QStringList{kPathB});
        QCOMPARE(result.files.unknown, QStringList{kPathC});
        QCOMPARE(tree.files.value(kPathA), QStringLiteral("turn1 A"));
        /* Not written, so still the turn-2 content. */
        QCOMPARE(tree.files.value(kPathB), QStringLiteral("turn2 B"));

        const QString report = qsocRewindReport(fullRewind(), result);
        QVERIFY2(!report.contains(QStringLiteral("cancelled")), qPrintable(report));
        QCOMPARE(
            report,
            QStringLiteral(
                "\n(Rewound partially: kept 2 messages, 1 file restored, 1 NOT "
                "restored, 1 left alone (state unknown), picked text restored for "
                "editing)\n"
                "  could not restore:\n    /w/b.sv\n"
                "  left untouched (state unknown):\n    /w/c.sv\n"));
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. A full rewind commits every step: the conversation shrinks, the
     * original creation timestamp survives the rewrite, the tree goes back,
     * and the target stays readable. A path left alone for unknown state is a
     * partial rewind, so forward checkpoints remain available for diagnosis
     * or retry and the label matches the list underneath it. */
    void partialRewindKeepsForwardSnapshotsAndTheTargetReachable()
    {
        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QCOMPARE(result.kept, 2);
        QCOMPARE(QSocSession::loadMessages(session->filePath()).size(), std::size_t{2});
        QCOMPARE(QSocSession::readInfo(session->filePath()).createdAt, createdAt);
        QCOMPARE(tree.files.value(kPathA), QStringLiteral("turn1 A"));
        QCOMPARE(tree.files.value(kPathB), QStringLiteral("turn1 B"));
        QCOMPARE(snapshotHighWater(), 2);
        QCOMPARE(history->contentAt(kPathA, 1), QStringLiteral("turn1 A"));
        QCOMPARE(
            qsocRewindReport(fullRewind(), result),
            QStringLiteral(
                "\n(Rewound partially: kept 2 messages, 2 files restored, 1 left alone "
                "(state unknown), picked text restored for editing)\n"
                "  left untouched (state unknown):\n    /w/c.sv\n"));
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. Done means every path the rewind was responsible for moved. With
     * nothing left alone and nothing refused there is no qualifier to print. */
    void aRewindThatMovedEveryPathReportsDone()
    {
        /* c stays readable this time, so its turn-1 record is a real one. */
        tree.files.insert(kPathC, QStringLiteral("turn1 C"));
        QVERIFY(history->makeSnapshot(3));
        QSocRewindRequest request = fullRewind();
        request.targetSnapshot    = 3;

        const QSocRewindResult result
            = qsocApplyRewind(request, session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Done);
        QVERIFY(qsocRewindMovesTurnCounter(request, result));
        QVERIFY(result.files.unknown.isEmpty());
        QVERIFY(result.files.failed.isEmpty());
        QCOMPARE(result.files.restored.size(), 3);
        QCOMPARE(
            qsocRewindReport(request, result),
            QStringLiteral(
                "\n(Rewound: kept 2 messages, 3 files restored, picked text restored "
                "for editing)\n"));
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. Code-only mode is preserved by construction: with
     * restoreConversation false there is no rewrite, no meta re-emission and
     * no truncation, so a later full rewind still has its snapshots. */
    void codeOnlyRewindKeepsTheConversationAndTheSnapshotHistory()
    {
        const QString before = readAll(session->filePath());

        QSocRewindRequest request   = fullRewind();
        request.restoreConversation = false;

        const QSocRewindResult result
            = qsocApplyRewind(request, session.get(), history.get(), QSocRewindFileGate());

        /* Damage first: the conversation and the snapshot index are the two
         * things code-only mode promises not to touch. */
        QCOMPARE(readAll(session->filePath()), before);
        QCOMPARE(QSocSession::loadMessages(session->filePath()).size(), std::size_t{4});
        QCOMPARE(snapshotHighWater(), 2);

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QCOMPARE(result.kept, 0);
        QCOMPARE(tree.files.value(kPathA), QStringLiteral("turn1 A"));
        QCOMPARE(
            qsocRewindReport(request, result),
            QStringLiteral(
                "\n(Rewound partially: code only, 2 files restored, 1 left alone "
                "(state unknown), conversation unchanged)\n"
                "  left untouched (state unknown):\n    /w/c.sv\n"));
    }

    /* SPECIFICATION test, not a counterexample: the function under test is
     * new. A restore that straddled two transports left the tree part-way
     * between turns, and the forward snapshots are what a retry restores
     * from, so they must survive. */
    void aTransportChangeMidRestoreKeepsTheSnapshotsARetryNeeds()
    {
        tree.bumpOnWrite = 1;

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), QSocRewindFileGate());

        /* Damage first: garbage-collecting these is what would make the
         * retry impossible. */
        QCOMPARE(snapshotHighWater(), 2);
        QCOMPARE(history->contentAt(kPathA, 2), QStringLiteral("turn2 A"));

        QVERIFY(result.files.transportChanged);
        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QVERIFY(result.files.failed.contains(kPathB));

        const QString report = qsocRewindReport(fullRewind(), result);
        QVERIFY2(!report.contains(QStringLiteral("cancelled")), qPrintable(report));
        QVERIFY2(
            report.contains(QStringLiteral("the checkpoints a retry needs were kept")),
            qPrintable(report));
        QVERIFY2(report.contains(kPathB), qPrintable(report));
    }

    void anExactLinkCheckpointCannotHideAnotherLinksPath()
    {
        const QString splitId = QStringLiteral("rewind-cross-link");
        QSocSession   splitSession(
            splitId,
            QDir(QSocSession::sessionsDir(dir->path())).filePath(splitId + QStringLiteral(".jsonl")));
        QVERIFY(splitSession.appendMessage(userMessage(QStringLiteral("keep"))));

        FakeTree        splitTree;
        QSocFileHistory splitHistory(dir->path(), splitId);
        splitHistory.setLiveAccessor(splitTree.accessor());
        splitTree.files.insert(kPathA, QStringLiteral("a0"));
        QVERIFY(splitHistory.trackEdit(kPathA, true, QStringLiteral("a0")));
        splitTree.files.insert(kPathA, QStringLiteral("a1"));
        QVERIFY(splitHistory.makeSnapshot(1));

        splitTree.gen = 2;
        splitTree.files.insert(kPathB, QStringLiteral("b0"));
        QVERIFY(splitHistory.trackEdit(kPathB, true, QStringLiteral("b0")));
        splitTree.files.insert(kPathB, QStringLiteral("b1"));
        QVERIFY(splitHistory.makeSnapshot(2));
        QVERIFY(splitHistory.trackEdit(kPathA, true, QStringLiteral("a1")));
        splitTree.files.insert(kPathA, QStringLiteral("a2"));
        QVERIFY(splitHistory.makeSnapshot(3));

        QSocRewindRequest request;
        request.restoreConversation = true;
        request.restoreFiles        = true;
        request.targetSnapshot      = 1;
        request.keptMessages        = json::array({userMessage(QStringLiteral("keep"))});
        const QSocRewindResult result
            = qsocApplyRewind(request, &splitSession, &splitHistory, QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QCOMPARE(result.files.unknown, QStringList{kPathA});
        QCOMPARE(splitTree.files.value(kPathA), QStringLiteral("a2"));
        QCOMPARE(splitTree.files.value(kPathB), QStringLiteral("b0"));
        int latest = 0;
        for (const auto &snapshot : splitHistory.listSnapshots()) {
            latest = qMax(latest, snapshot.turn);
        }
        QCOMPARE(latest, 3);
    }

    void missingCheckpointRefusesBeforeConversationRewrite()
    {
        QVERIFY(history->truncateAfter(0));
        const QString beforeSession = readAll(session->filePath());
        const auto    beforeTree    = tree.files;

        const QSocRewindResult result
            = qsocApplyRewind(fullRewind(), session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Refused);
        QCOMPARE(readAll(session->filePath()), beforeSession);
        QCOMPARE(tree.files, beforeTree);
        QVERIFY(result.refusal.contains(QStringLiteral("checkpoint")));
    }

    void failedCheckpointTruncationIsPartialAndKeepsTheFuture()
    {
#ifdef Q_OS_WIN
        QSKIP("directory write permissions are not deterministic on Windows");
#else
        tree.files.insert(kPathA, QStringLiteral("turn3 A"));
        tree.files.insert(kPathB, QStringLiteral("turn3 B"));
        tree.files.insert(kPathC, QStringLiteral("turn3 C"));
        QVERIFY(history->makeSnapshot(3));

        QSocRewindRequest request = fullRewind();
        request.targetSnapshot    = 2;
        const QString root
            = QSocFileHistory::historyDir(dir->path(), QStringLiteral("rewind-fixture"));
        const auto oldPermissions = QFileInfo(root).permissions();
        QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto restorePermissions = qScopeGuard(
            [&] { (void) QFile::setPermissions(root, oldPermissions); });

        const QSocRewindResult result
            = qsocApplyRewind(request, session.get(), history.get(), QSocRewindFileGate());

        QCOMPARE(result.outcome, QSocRewindResult::Outcome::Partial);
        QVERIFY(result.files.historyTruncateFailed);
        QCOMPARE(snapshotHighWater(), 3);
        const QString report = qsocRewindReport(request, result);
        QVERIFY(report.contains(QStringLiteral("forward checkpoints NOT discarded")));
#endif
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocrewind.moc"
