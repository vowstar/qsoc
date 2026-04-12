// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFILEHISTORY_H
#define QSOCFILEHISTORY_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

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
 *       "files": {                       // files tracked in this snapshot
 *         "/abs/path/to/apb.yaml": "<sha256>",
 *         "/abs/path/to/new.v":    null   // file was absent at this turn
 *       }
 *     }
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
     * @brief Per-snapshot metadata (one entry per captured turn).
     * @details files maps absolute file path to either a sha256 hex digest
     *          (content present) or an empty string (file was absent at
     *          this turn).
     */
    struct Snapshot
    {
        int                    turn = 0;
        QDateTime              timestamp;
        QMap<QString, QString> files; /* path -> sha256 hex, or "" for absent */
    };

    /**
     * @brief Construct a history bound to a session directory.
     * @param projectPath Absolute path to the project root (used to derive
     *                    the file-history directory).
     * @param sessionId   Session UUID — one history store per session.
     */
    QSocFileHistory(QString projectPath, QString sessionId);

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
     * @param turn Monotonic turn index (1 for the first user turn, 2 for the
     *             next, ...). Must strictly increase across calls.
     * @return true if the snapshot was written, false on I/O errors.
     */
    bool makeSnapshot(int turn);

    /**
     * @brief Restore every tracked file to the state captured in snapshot N.
     * @details For each file in the target snapshot:
     *            - if the recorded sha256 is empty, unlink() the file if it
     *              exists on disk (it was absent at that turn);
     *            - otherwise, read the backup blob and overwrite the file.
     *          Files that appear in LATER snapshots but not in the target
     *          are also restored: we look back through history to find the
     *          most recent prior state for them and apply it. Files that
     *          the agent never touched are not modified.
     * @param turn Target snapshot index.
     * @return List of absolute file paths that were touched by the restore.
     *         Empty if the snapshot doesn't exist or on I/O errors.
     */
    QStringList applySnapshot(int turn);

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
    QString projectPathValue;
    QString sessionIdValue;
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
    void            appendSnapshot(const Snapshot &snapshot) const;
    void            evictOldest();
    /* Walk every surviving snapshot, collect referenced sha256 set, and
     * delete any .bak blob that is no longer referenced. */
    void gcOrphanedBackups() const;
    /* Build the effective file->sha256 map at the given turn by scanning
     * all snapshots with turn <= N and keeping the latest record per path. */
    QMap<QString, QString> effectiveStateAt(int turn) const;
};

#endif // QSOCFILEHISTORY_H
