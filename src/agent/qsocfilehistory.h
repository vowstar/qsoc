// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFILEHISTORY_H
#define QSOCFILEHISTORY_H

#include <cstdint>
#include <functional>
#include <optional>
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
 *   also restore every covered file the agent edited, and so that `/diff` can
 *   show what actually changed between any two turns.
 *
 *   Storage layout, rooted at `<projectPath>/.qsoc/file-history/<session-id>/`:
 *
 *     backups/<sha256>.bak   - content-addressed backup blobs (deduped)
 *     snapshots.jsonl        - one sealed line per retained snapshot
 *
 *   Each snapshots.jsonl line is a JSON object of the form
 *
 *     {
 *       "turn": <int>,                  // monotonic turn counter
 *       "ts":   "<iso8601>",             // when the snapshot was captured
 *       "tree": "<id>",                  // working tree it describes
 *       "link": "<opaque-id>",           // transport reaching that tree
 *       "files": {                       // files tracked in this snapshot
 *         "/abs/path/to/apb.yaml": {"since": 0, "state": "<sha256>"},
 *         "/abs/path/to/new.v":    {"since": 1, "state": null},
 *         "/abs/path/to/quiet.v":  {"since": 0, "state": "unknown"}
 *       },
 *       "seal": "<sha256-of-the-other-fields>"
 *     }
 *
 *   `since` is the last completed turn before the file's first mutation. The
 *   record at that turn is its durable baseline. Missing or inconsistent
 *   provenance invalidates the index. Unsealed legacy lines load only as
 *   untrusted unknown records and can never mutate a working tree.
 *
 *   **Snapshot indexing**: turn N is the state after the N-th user turn
 *   completes. A path first edited after turn N stores its pre-edit baseline
 *   in that checkpoint; turn 0 therefore contains only paths edited before the
 *   first turn completes. Rewinding to user message K applies snapshot K - 1,
 *   the state immediately before that message's effects.
 *
 *   **Oldest-turn eviction**: at most MAX_SNAPSHOTS checkpoints survive. When an
 *   introduction checkpoint is evicted, the earliest retained known state
 *   becomes that path's new baseline; a path with no retained known state is
 *   forgotten. Backup blobs referenced only by evicted state are removed.
 *
 *   **Lazy tracking**: only files that the agent has actually written inside
 *   the bound project or remote workspace are backed up or snapshotted.
 *   Writes to other allowed local roots continue without a file checkpoint.
 *   Untouched and uncovered files are left alone by every rewind / apply
 *   operation. The tracked set is per tree: a path edited on one tree is not
 *   read on another, where the same absolute path names a different file or
 *   nothing at all.
 *
 *   **Epochs**: every record carries the tree and opaque transport identity
 *   under which it was captured. Restore uses an exact checkpoint from the
 *   live epoch when one exists. Missing identities and omitted paths are never
 *   inferred from older records.
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
     * @brief Which working tree a record belongs to, and over which link.
     * @details @ref tree names a file namespace: the local disk, or one
     *          workspace on one host. Absolute paths only mean the same thing
     *          inside one of them. @ref link names the connection reaching
     *          that tree, so a reconnect to the same workspace keeps the tree
     *          and changes the link. Empty identities are what snapshots
     *          written before the fields existed read as. They are not
     *          wildcards and are never actionable.
     */
    struct Epoch
    {
        QString tree; /**< Working-tree identity; empty when unproven. */
        QString link; /**< Transport identity; empty when unproven. */

        friend bool operator==(const Epoch &lhs, const Epoch &rhs)
        {
            return lhs.tree == rhs.tree && lhs.link == rhs.link;
        }
        friend bool operator!=(const Epoch &lhs, const Epoch &rhs) { return !(lhs == rhs); }
    };

    /**
     * @brief How a record's epoch relates to the one bound right now.
     * @details The two mismatches are not one risk with two names. A
     *          different link is the same files seen over a connection that
     *          has since been replaced, which a user may knowingly accept. A
     *          tree that is not this one, or cannot be shown to be, is another
     *          file namespace, where the same absolute path may denote an
     *          unrelated file, so acting on it is not a risk anyone can accept
     *          on the user's behalf.
     */
    enum class EpochRelation : std::uint8_t {
        Same,      /**< The record is this tree's, over a link still bound. */
        OtherLink, /**< Same tree, a connection that has been replaced. */
        OtherTree, /**< Not shown to be this namespace; paths may differ. */
    };

    /**
     * @brief Classify @p record against @p live. Exposed for tests.
     * @details A record naming no tree is OtherTree against every live epoch,
     *          the local disk included: its origin was never proven, so nothing
     *          acts on it and no caller can waive it. Only a record whose named
     *          tree matches the live one is Same.
     */
    static EpochRelation relate(const Epoch &record, const Epoch &live);

    /**
     * @brief What a snapshot recorded about one file.
     * @details A default-constructed record is Unknown, so a path missing
     *          from a map reads as "no information" rather than "absent".
     *          @ref epoch names the tree and link the record was captured
     *          under; it survives the flattening that merges several
     *          snapshots into one effective state.
     */
    class FileRecord
    {
    public:
        FileRecord() = default;

        /** @brief State could not be established at capture time. */
        static FileRecord unknown(Epoch epoch, int introducedTurn = 0)
        {
            return FileRecord(FileState::Unknown, QString(), std::move(epoch), introducedTurn);
        }

        /** @brief The file was not there at capture time. */
        static FileRecord absent(Epoch epoch, int introducedTurn = 0)
        {
            return FileRecord(FileState::Absent, QString(), std::move(epoch), introducedTurn);
        }

        /** @brief The file held the content behind @p sha256. */
        static FileRecord present(QString sha256, Epoch epoch, int introducedTurn = 0)
        {
            return FileRecord(FileState::Present, std::move(sha256), std::move(epoch), introducedTurn);
        }

        bool isUnknown() const { return stateValue == FileState::Unknown; }
        bool isAbsent() const { return stateValue == FileState::Absent; }
        bool isPresent() const { return stateValue == FileState::Present; }

        /** @brief Backup blob digest; empty unless the record is present. */
        QString sha256() const { return shaValue; }

        /** @brief Tree and link this record was captured under. */
        const Epoch &epoch() const { return epochValue; }

        /** @brief Last completed turn before this path was first tracked. */
        int introducedTurn() const { return introducedTurnValue; }

    private:
        FileRecord(FileState state, QString sha256, Epoch epoch, int introducedTurn)
            : stateValue(state)
            , shaValue(std::move(sha256))
            , epochValue(std::move(epoch))
            , introducedTurnValue(introducedTurn)
        {}

        FileState stateValue = FileState::Unknown;
        QString   shaValue;
        Epoch     epochValue;
        int       introducedTurnValue = 0;
    };

    /**
     * @brief Per-snapshot metadata (one entry per captured turn and tree).
     * @details The epoch is stored once per snapshot, so every record in one
     *          snapshot shares it. That is why the baseline keeps the epoch it
     *          was created under: stamping a later one would claim it for
     *          every path already recorded. A session that edits on two trees
     *          therefore gets one baseline per tree, not one shared baseline.
     */
    struct Snapshot
    {
        int                       turn = 0;
        QDateTime                 timestamp;
        Epoch                     epoch;
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
     *          `tree` names the file namespace these paths live in and
     *          `generation` the opaque connection reaching it, so a capture or a
     *          restore can tell that the tree it started on is not the tree it
     *          is finishing on. A backend that names no tree is unproven and
     *          never actionable. Every backend
     *          must provide both identities; local disk uses a stable identity
     *          derived from the canonical project root.
     *
     *          `inScope` is the stable checkpoint policy. `coversPath` is the
     *          live safety fact: a replaced root remains in scope but is no
     *          longer covered, so the operation must fail instead of silently
     *          proceeding without history. `ensureIdentity`, when present,
     *          establishes a lazy identity immediately before checkpoint
     *          persistence begins.
     */
    struct LiveFileAccessor
    {
        std::function<FileState(const QString &path)>                    exists;
        std::function<LiveRead(const QString &path)>                     read;
        std::function<bool(const QString &path, const QString &content)> write;
        std::function<bool(const QString &path)>                         remove;
        std::function<bool(const QString &path)>                         inScope;
        std::function<bool(const QString &path)>                         coversPath;
        std::function<QString()>                                         tree;
        std::function<QString()>                                         generation;
        std::function<bool()>                                            ensureIdentity;
    };

    using WritableEntryResolver = std::function<bool(const QString &, QString *)>;

    /** @brief Local-disk accessor (the default backend). */
    static LiveFileAccessor localAccessor(
        const QString &projectRoot = QString(), WritableEntryResolver resolver = {});

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
     *          written. A missing target snapshot is also explicit: an older
     *          state must not masquerade as the requested turn.
     */
    struct RestoreReport
    {
        QStringList restored;                      /**< Paths whose content was put back. */
        QStringList failed;                        /**< Paths that could not be put back. */
        QStringList unknown;                       /**< Paths deliberately left untouched. */
        bool        transportChanged      = false; /**< The link changed mid-restore. */
        bool        targetMissing         = false; /**< The requested checkpoint is absent. */
        bool        historyTruncateFailed = false; /**< Forward checkpoints remain. */

        bool isEmpty() const
        {
            return restored.isEmpty() && failed.isEmpty() && unknown.isEmpty() && !targetMissing
                   && !historyTruncateFailed;
        }
    };

    /**
     * @brief Whether a restore may act on a record from a replaced link.
     * @details Scoped to @ref EpochRelation::OtherLink, the risk it was
     *          written for: the same tree, seen over a connection that is
     *          gone. It cannot waive @ref EpochRelation::OtherTree, because
     *          those records never reach the decision: they are dropped when
     *          the effective state is flattened.
     */
    enum class AcrossGeneration : std::uint8_t {
        Refuse, /**< Leave such records alone and report them as unknown. */
        Allow,  /**< Act on them anyway; the user accepted the risk. */
    };

    /**
     * @brief What a restore to a given turn cannot put back, known up front.
     * @details Read before the restore so the user is told why a path will be
     *          left alone instead of inferring it from a shorter list
     *          afterwards. Sampled through the same live epoch the restore
     *          itself uses, so the two can never disagree.
     */
    struct BoundaryPreview
    {
        QStringList otherTree; /**< Recorded on another tree; never acted on. */
        QStringList otherLink; /**< Same tree, replaced link; needs Allow. */

        bool isEmpty() const { return otherTree.isEmpty() && otherLink.isEmpty(); }
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

    /** @brief Whether policy assigns @p filePath to this history store. */
    bool isPathInScope(const QString &filePath) const;

    /** @brief Whether the live binding can safely reach @p filePath now. */
    bool coversPath(const QString &filePath) const;

    /** @brief Snapshot cap. Oldest checkpoints are evicted first. */
    static constexpr int MAX_SNAPSHOTS = 100;

    /**
     * @brief Record the pre-mutation state of a file before a tool edits it.
     * @details Called by edit_file / write_file immediately before their
     *          write() happens. The first call for a given file captures its
     *          state at the latest completed turn. Subsequent calls are no-ops
     *          once the file is already tracked; each turn's post-state is
     *          captured in makeSnapshot(), not here. Uncovered paths return
     *          false and must be skipped by callers that intentionally permit
     *          writes outside the checkpoint scope.
     * @param filePath     Absolute path to the file being edited.
     * @param beforeExists true if the file existed on disk before the edit.
     * @param beforeContent Raw content at the time of the call. Ignored when
     *                     beforeExists is false.
     * @return true only after the baseline index and any blob are durable.
     */
    bool trackEdit(const QString &filePath, bool beforeExists, const QString &beforeContent);

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
     *          The target turn must contain an exact checkpoint for the live
     *          tree. Without one the restore is a no-op reported as missing;
     *          an older state cannot stand in for a turn recorded elsewhere.
     *          A tracked path omitted by the exact checkpoint is unknown and
     *          is never filled from an older turn. Files the agent never
     *          touched are not modified.
     * @param turn Target snapshot index.
     * @param across Whether to act on records captured over a link that has
     *               since been replaced. An unknown record is never
     *               actionable, Allow or not: a replaced link is a heuristic
     *               the user may knowingly override, an unknown record is a
     *               genuine absence of information.
     * @return What was put back, what could not be, and what was skipped.
     */
    RestoreReport applySnapshot(int turn, AcrossGeneration across = AcrossGeneration::Refuse);

    /**
     * @brief Prove a target checkpoint can be read before any rewind mutation.
     * @return Empty when usable, otherwise a user-facing refusal reason.
     */
    QString restoreRefusal(int turn) const;

    /**
     * @brief What @ref applySnapshot would refuse to act on at @p turn.
     * @details Unknown records are left out: they are reported by the restore
     *          itself and would fire the notice for something no boundary
     *          caused.
     */
    BoundaryPreview previewBoundary(int turn) const;

    /**
     * @brief Drop every snapshot with turn > cutoffTurn.
     * @details Used after a rewind picks turn K with files mode: the
     *          snapshots for K+1, K+2, ... become orphaned because the
     *          future they describe no longer exists. Backup blobs that
     *          are no longer referenced by any surviving snapshot are
     *          garbage-collected.
     * @param cutoffTurn Keep snapshots with turn <= cutoffTurn.
     */
    bool truncateAfter(int cutoffTurn);

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

    /** @brief Whether the local checkpoint store is bound or can still bind safely. */
    bool storageIsBound() const;

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
    QString storageCanonicalRootValue;
    QString storageRootIdentityValue;
    QString storageTreeIdValue;
    QString storageLexicalRootValue;

    LiveFileAccessor liveAccessor;
    /* Snapshots loaded lazily on first access; mutations to disk keep this
     * in sync so callers don't pay for repeated reads. */
    mutable QList<Snapshot> cachedSnapshots;
    mutable bool            cacheValid = false;
    enum class IndexState : std::uint8_t {
        Unknown,
        Valid,
        Invalid,
    };
    mutable IndexState indexState = IndexState::Unknown;

    bool                   ensureStorageBinding();
    bool                   ensureWritableBindings();
    bool                   ensureDirs() const;
    bool                   writeBackup(const QString &sha256, const QString &content) const;
    std::optional<QString> readBackup(const QString &sha256) const;
    QList<Snapshot>        loadSnapshots() const;
    bool                   saveSnapshots(const QList<Snapshot> &snapshots) const;
    /* Walk every surviving snapshot, collect referenced sha256 set, and
     * delete any .bak blob that is no longer referenced. */
    void gcOrphanedBackups() const;
    /* Build the effective path->record map at the given turn by scanning all
     * snapshots with turn <= N and keeping the latest record per path. A
     * non-empty live.tree drops every record from another tree first. */
    QMap<QString, FileRecord> effectiveStateAt(int turn, const Epoch &live) const;
    /* Exact checkpoint records plus fail-closed unknowns for tracked paths
     * the checkpoint does not name. */
    QMap<QString, FileRecord> restoreStateAt(
        int turn, const Epoch &live, bool *targetExists, bool *targetComplete = nullptr) const;
    /* Paths captured in the exact epoch. Empty identities never promote old
     * records into the live namespace. */
    QSet<QString> trackedPathsFor(const Epoch &epoch, int atTurn) const;
    int           introducedTurnFor(const Epoch &epoch, const QString &path) const;
    /* Single sampling point for every epoch check, so the straddle guard, the
     * scoping and the per-record boundary guard can never disagree about what
     * is bound. Empty identities remain empty and fail closed. */
    Epoch liveEpoch() const;
};

#endif // QSOCFILEHISTORY_H
