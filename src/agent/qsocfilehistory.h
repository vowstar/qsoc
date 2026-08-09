// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFILEHISTORY_H
#define QSOCFILEHISTORY_H

#include <cstdint>
#include <functional>
#include <utility>

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

/**
 * @brief Per-session file snapshot store for qsoc rewind and diff.
 * @details
 *   File history is a thin checkpoint layer that sits behind the
 *   `edit_file` and `write_file` tools. Its job is to capture enough
 *   state so that rewinding the conversation to an earlier message can
 *   also restore every file the agent edited, and so that `/diff` can
 *   show what actually changed between any two turns.
 *
 *   Storage layout, rooted at `<projectPath>/.qsoc/file-history/<session-id>/`:
 *
 *     backups/<sha256>.bak   - content-addressed backup blobs (deduped)
 *     snapshots.jsonl        - one line per snapshot, append-only
 *
 *   Each snapshots.jsonl line is a JSON object of the form
 *
 *     {
 *       "turn": <int>,                  // monotonic turn counter
 *       "ts":   "<iso8601>",             // when the snapshot was captured
 *       "gen":  <uint>,                  // transport that captured it, 0 = none
 *       "files": {                       // files tracked in this snapshot
 *         "/abs/path/to/apb.yaml": "<sha256>",
 *         "/abs/path/to/new.v":    null,     // file was absent at this turn
 *         "/abs/path/to/quiet.v":  "unknown" // state could not be established
 *       }
 *     }
 *
 *   A file value is classified by JSON type and shape, not by a version
 *   field: null is absent, a 64-character lowercase hex string is present,
 *   and anything else (including a torn value) is unknown. A line with no
 *   `"gen"` reads as generation 0, which never trips the cross-generation
 *   guard, so sessions written before the field existed still rewind.
 *
 *   **Snapshot indexing**: turn 0 is the "baseline" snapshot that captures
 *   the pre-edit state of each tracked file at its first mutation. Turn N
 *   (for N >= 1) is the state *after* the N-th user turn completes (all
 *   tool calls done). Rewinding to user message K means applying snapshot
 *   (K - 1), which is the state right before K's effects.
 *
 *   **LRU eviction**: snapshots older than MAX_SNAPSHOTS are dropped from
 *   both snapshots.jsonl and any backup blobs they uniquely reference. The
 *   baseline turn 0 is sticky and never evicted while any later snapshot
 *   survives, so rewinding to the very first turn always works.
 *
 *   **Lazy tracking**: only files that the agent has actually written are
 *   ever backed up or snapshotted. Untouched project files are left alone
 *   by every rewind / apply operation.
 */
class QSocFileHistory
{
public:
    /**
     * @brief What is known about one file's existence or content.
     * @details Unknown is first so a default-constructed state refuses to
     *          act. Collapsing it onto Absent is what turned a link failure
     *          into a deletion: a rewind reads Absent as "unlink this" and
     *          removes a file that is still there.
     */
    enum class FileState : std::uint8_t {
        Unknown, /**< The backend could not be asked. Never actionable. */
        Absent,  /**< The backend answered: the file is not there. */
        Present, /**< The backend answered: the file is there. */
    };

    /**
     * @brief One answer from a live-file read.
     * @details Constructed only through the named factories, so no call site
     *          can produce a value meaning "it failed, you decide": every
     *          backend has to pick which of the three answers it is giving.
     */
    class LiveRead
    {
    public:
        /** @brief The backend could not be asked. Content is empty. */
        static LiveRead unknown() { return LiveRead(FileState::Unknown, QString()); }

        /** @brief The backend answered: no such file. Content is empty. */
        static LiveRead absent() { return LiveRead(FileState::Absent, QString()); }

        /** @brief The backend answered with @p content (possibly empty). */
        static LiveRead present(QString content)
        {
            return LiveRead(FileState::Present, std::move(content));
        }

        /** @brief Which of the three answers this is. */
        FileState state() const { return stateValue; }

        /** @brief Content read; empty unless @ref state is Present. */
        QString content() const { return contentValue; }

    private:
        LiveRead(FileState state, QString content)
            : stateValue(state)
            , contentValue(std::move(content))
        {}

        FileState stateValue;
        QString   contentValue;
    };

    /**
     * @brief What a snapshot recorded about one file.
     * @details A default-constructed record is Unknown, so a path missing
     *          from a map reads as "no information" rather than "absent".
     *          @ref generation names the transport the record was captured
     *          over; it survives the flattening that merges several
     *          snapshots into one effective state.
     */
    class FileRecord
    {
    public:
        FileRecord() = default;

        /** @brief State could not be established at capture time. */
        static FileRecord unknown(quint64 generation = 0)
        {
            return FileRecord(FileState::Unknown, QString(), generation);
        }

        /** @brief The file was not there at capture time. */
        static FileRecord absent(quint64 generation)
        {
            return FileRecord(FileState::Absent, QString(), generation);
        }

        /** @brief The file held the content behind @p sha256. */
        static FileRecord present(QString sha256, quint64 generation)
        {
            return FileRecord(FileState::Present, std::move(sha256), generation);
        }

        bool isUnknown() const { return stateValue == FileState::Unknown; }
        bool isAbsent() const { return stateValue == FileState::Absent; }
        bool isPresent() const { return stateValue == FileState::Present; }

        /** @brief Backup blob digest; empty unless the record is present. */
        QString sha256() const { return shaValue; }

        /** @brief Transport this record was captured over; 0 means none. */
        quint64 generation() const { return generationValue; }

    private:
        FileRecord(FileState state, QString sha256, quint64 generation)
            : stateValue(state)
            , shaValue(std::move(sha256))
            , generationValue(generation)
        {}

        FileState stateValue = FileState::Unknown;
        QString   shaValue;
        quint64   generationValue = 0;
    };

    /**
     * @brief Per-snapshot metadata (one entry per captured turn).
     * @details generation names the transport that captured the snapshot; 0
     *          means no transport discipline (local disk, or a session
     *          written before the field existed). The baseline turn 0 keeps
     *          the generation it was created under even as later edits add
     *          paths to it, so a record never claims a transport that was
     *          not bound when it was captured.
     */
    struct Snapshot
    {
        int                       turn = 0;
        QDateTime                 timestamp;
        quint64                   generation = 0;
        QMap<QString, FileRecord> files;
    };

    /**
     * @brief Live-file backend for snapshot capture and restore.
     * @details Backup blobs are always stored locally; only the live files
     *          being snapshotted / restored go through this accessor. The
     *          default is local disk; the CLI swaps in an SFTP-backed
     *          accessor while a remote session is active so rewind restores
     *          the remote working tree, not a stale local path.
     *
     *          `write` and `remove` stay boolean while `exists` and `read`
     *          are three-state, because the two non-Ok write outcomes call
     *          for the same action (do not claim it was restored, touch
     *          nothing else, name the path) whereas the two non-Present
     *          existence outcomes call for opposite ones (remove versus
     *          leave alone). A write whose outcome is unknown is therefore
     *          reported as not restored, which over-reports failure but
     *          never deletes.
     *
     *          `generation` names the transport that is bound right now, so
     *          a capture or a restore can tell that the link it started on
     *          is no longer the link it is finishing on. Leave it unset when
     *          the backend has no such notion; 0 disables both generation
     *          checks.
     */
    struct LiveFileAccessor
    {
        std::function<FileState(const QString &path)>                    exists;
        std::function<LiveRead(const QString &path)>                     read;
        std::function<bool(const QString &path, const QString &content)> write;
        std::function<bool(const QString &path)>                         remove;
        std::function<quint64()>                                         generation;
    };

    /** @brief Local-disk accessor (the default backend). */
    static LiveFileAccessor localAccessor();

    /**
     * @brief Outcome of putting a snapshot back on the working tree.
     * @details `failed` exists because a partial restore used to be
     *          indistinguishable from a small one: a write the accessor
     *          refused, a removal that did not happen, or a missing backup
     *          blob all dropped out silently and the caller reported "N
     *          files restored" with a smaller N.
     *
     *          `unknown` is not a flavour of `failed`: "I did not try"
     *          leaves the working tree exactly as the user left it, while
     *          "I tried and it did not happen" may have left it half
     *          written. isEmpty() deliberately ignores transportChanged so
     *          callers that test it keep their existing meaning.
     */
    struct RestoreReport
    {
        QStringList restored;                 /**< Paths whose content was put back. */
        QStringList failed;                   /**< Paths that could not be put back. */
        QStringList unknown;                  /**< Paths deliberately left untouched. */
        bool        transportChanged = false; /**< The link changed mid-restore. */

        bool isEmpty() const { return restored.isEmpty() && failed.isEmpty() && unknown.isEmpty(); }
    };

    /** @brief Whether a restore may act on a record from another transport. */
    enum class AcrossGeneration : std::uint8_t {
        Refuse, /**< Leave such records alone and report them as unknown. */
        Allow,  /**< Act on them anyway; the user accepted the risk. */
    };

    /**
     * @brief Construct a history bound to a session directory.
     * @param projectPath Absolute path to the project root (used to derive
     *                    the file-history directory).
     * @param sessionId   Session UUID — one history store per session.
     */
    QSocFileHistory(QString projectPath, QString sessionId);

    /**
     * @brief Swap the live-file backend (e.g. local <-> SFTP on /ssh).
     * @details A no-op accessor field falls back to the local default, so a
     *          partially-populated accessor is safe.
     */
    void setLiveAccessor(LiveFileAccessor accessor);

    /**
     * @brief Snapshot cap. Older turns are evicted LRU-style.
     */
    static constexpr int MAX_SNAPSHOTS = 100;

    /**
     * @brief Record the pre-mutation state of a file before a tool edits it.
     * @details Called by edit_file / write_file immediately before their
     *          write() happens. The first call for a given file within a
     *          session captures the baseline version of that file (turn 0).
     *          Subsequent calls are no-ops once the file is already tracked
     *          — each turn's post-state is captured in makeSnapshot(), not
     *          here.
     * @param filePath     Absolute path to the file being edited.
     * @param beforeExists true if the file existed on disk before the edit.
     * @param beforeContent Raw content at the time of the call. Ignored when
     *                     beforeExists is false.
     */
    void trackEdit(const QString &filePath, bool beforeExists, const QString &beforeContent);

    /**
     * @brief Capture the post-turn state of every tracked file.
     * @details Called from runAgentLoop after a turn completes (runStream
     *          returns and persistSessionDelta has flushed). Reads the
     *          current content of every tracked file, hashes it, saves a
     *          backup blob if new, and appends one line to snapshots.jsonl
     *          indexed by turn.
     *          A file the accessor could not read is recorded as unknown,
     *          never as absent: an absent record makes a later rewind unlink
     *          a file that is still there.
     * @param turn Monotonic turn index (1 for the first user turn, 2 for the
     *             next, ...). Must strictly increase across calls.
     * @return true if the snapshot was written, false on I/O errors or when
     *         the transport changed while the capture was running.
     */
    bool makeSnapshot(int turn);

    /**
     * @brief Restore every tracked file to the state captured in snapshot N.
     * @details For each file in the target snapshot:
     *            - an unknown record is left strictly alone;
     *            - an absent record unlinks the file, but only once the
     *              accessor confirms it is there;
     *            - a present record overwrites the file from its backup blob.
     *          Files that appear in LATER snapshots but not in the target
     *          are also restored: we look back through history to find the
     *          most recent prior state for them and apply it. Files that
     *          the agent never touched are not modified.
     * @param turn Target snapshot index.
     * @param across Whether to act on records captured over a transport
     *               other than the one bound now. An unknown record is never
     *               actionable, Allow or not: a generation boundary is a
     *               heuristic the user may knowingly override, an unknown
     *               record is a genuine absence of information.
     * @return What was put back, what could not be, and what was skipped.
     */
    RestoreReport applySnapshot(int turn, AcrossGeneration across = AcrossGeneration::Refuse);

    /**
     * @brief Drop every snapshot with turn > cutoffTurn.
     * @details Used after a rewind picks turn K with files mode: the
     *          snapshots for K+1, K+2, ... become orphaned because the
     *          future they describe no longer exists. Backup blobs that
     *          are no longer referenced by any surviving snapshot are
     *          garbage-collected.
     * @param cutoffTurn Keep snapshots with turn <= cutoffTurn.
     */
    void truncateAfter(int cutoffTurn);

    /**
     * @brief Load the complete snapshot index from disk.
     * @return Snapshots in turn-ascending order. Empty on I/O errors or
     *         missing file.
     */
    QList<Snapshot> listSnapshots() const;

    /**
     * @brief Read a single file's content as it was at a specific snapshot.
     * @param filePath Absolute file path.
     * @param turn     Snapshot index.
     * @return The content string, or a null QString if the file was absent
     *         at that turn or the snapshot / backup is missing.
     */
    QString contentAt(const QString &filePath, int turn) const;

    /**
     * @brief Compute the "current" turn counter: the highest snapshot turn
     *        on disk, or 0 if none have been written.
     */
    int latestTurn() const;

    /**
     * @brief Check whether any snapshots exist for this session.
     */
    bool isEmpty() const;

    /**
     * @brief Return the content-addressed backup path for a given sha256.
     * @details Public for tests; callers should not rely on the layout.
     */
    QString backupPathFor(const QString &sha256) const;

    /**
     * @brief Return the snapshots.jsonl path for this session.
     * @details Public for tests.
     */
    QString snapshotsPath() const;

    /**
     * @brief Canonical file-history directory for a project + session.
     */
    static QString historyDir(const QString &projectPath, const QString &sessionId);

    /**
     * @brief Compute SHA-256 hex digest of a QString encoded as UTF-8.
     * @details Exposed for tests and for callers that need to compare the
     *          current disk content against a snapshot record.
     */
    static QString sha256Hex(const QString &content);

private:
    QString          projectPathValue;
    QString          sessionIdValue;
    LiveFileAccessor liveAccessor;
    /* Files that have been touched at least once this session, tracked so
     * subsequent snapshots capture their post-turn state even when the
     * file wasn't re-edited in that specific turn. */
    QSet<QString> trackedFiles;
    /* Snapshots loaded lazily on first access; mutations to disk keep this
     * in sync so callers don't pay for repeated reads. */
    mutable QList<Snapshot> cachedSnapshots;
    mutable bool            cacheValid = false;

    void            ensureDirs() const;
    void            writeBackup(const QString &sha256, const QString &content) const;
    QString         readBackup(const QString &sha256) const;
    QList<Snapshot> loadSnapshots() const;
    void            saveSnapshots(const QList<Snapshot> &snapshots) const;
    /* Walk every surviving snapshot, collect referenced sha256 set, and
     * delete any .bak blob that is no longer referenced. */
    void gcOrphanedBackups() const;
    /* Build the effective path->record map at the given turn by scanning
     * all snapshots with turn <= N and keeping the latest record per path. */
    QMap<QString, FileRecord> effectiveStateAt(int turn) const;
    /* Single sampling point for both generation checks, so the straddle
     * guard and the per-record boundary guard can never disagree about
     * which transport is bound. 0 when the backend has no notion of one. */
    quint64 liveGeneration() const;
};

#endif // QSOCFILEHISTORY_H
