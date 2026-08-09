// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"

#include <nlohmann/json.hpp>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

using json = nlohmann::json;

namespace {

QString isoNowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

/* A recorded digest is trusted only when it has the exact shape writeBackup
 * produces; anything else names no blob we could read back. */
bool isSha256Hex(const QString &value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const QChar chr : value) {
        const bool digit = chr >= QLatin1Char('0') && chr <= QLatin1Char('9');
        const bool lower = chr >= QLatin1Char('a') && chr <= QLatin1Char('f');
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

/* Three-state existence on local disk. QFileInfo::exists() is also false for
 * a path we were not allowed to stat and for a stale mount handle, so "not
 * there" is a fact only when the parent directory could answer for its
 * children. */
QSocFileHistory::FileState localPresence(const QString &path)
{
    if (QFileInfo::exists(path)) {
        return QSocFileHistory::FileState::Present;
    }
    const QFileInfo parent(QFileInfo(path).absolutePath());
    if (parent.isDir() && parent.isReadable() && parent.isExecutable()) {
        return QSocFileHistory::FileState::Absent;
    }
    return QSocFileHistory::FileState::Unknown;
}

/* The name localAccessor() reports for the local disk. It is also the one tree
 * a record that names none may be acted on: that is where the absolute paths of
 * a session written before the field existed point. */
QString localTreeName()
{
    return QStringLiteral("local");
}

} // namespace

QSocFileHistory::LiveFileAccessor QSocFileHistory::localAccessor()
{
    LiveFileAccessor accessor;
    accessor.exists = [](const QString &path) { return localPresence(path); };
    accessor.read   = [](const QString &path) -> LiveRead {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray utf8 = file.readAll();
            file.close();
            return LiveRead::present(QString::fromUtf8(utf8));
        }
        /* An open that failed says nothing about the content, and the same
         * stat that could not answer for exists() cannot answer here either:
         * deciding "absent" from a failed stat is what turns a permission
         * error into an instruction to unlink. */
        switch (localPresence(path)) {
        case FileState::Absent:
            return LiveRead::absent();
        case FileState::Present:
        case FileState::Unknown:
            break;
        }
        return LiveRead::unknown();
    };
    accessor.write = [](const QString &path, const QString &content) {
        const QFileInfo info(path);
        QDir            parent = info.absoluteDir();
        if (!parent.exists()) {
            parent.mkpath(QStringLiteral("."));
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        file.write(content.toUtf8());
        file.close();
        return true;
    };
    accessor.remove = [](const QString &path) {
        QFile existing(path);
        if (!existing.exists()) {
            return true;
        }
        return existing.remove();
    };
    /* The local disk is one tree that is never replaced underneath a capture,
     * so it names itself and leaves the link unnumbered. Naming it is what
     * fences it from a remote workspace reached over SFTP: without a name both
     * would read as "no discipline" and records would cross between them. */
    accessor.tree = []() { return localTreeName(); };
    return accessor;
}

QSocFileHistory::EpochRelation QSocFileHistory::relate(const Epoch &record, const Epoch &live)
{
    /* A record that names no tree predates the field: nothing on disk shows
     * which namespace it was captured in. Without a proven tree identity there
     * is no permission to overwrite or unlink a colliding path, on the local
     * disk or anywhere else, so such a record is never this tree's. A purely
     * local pre-upgrade session loses automatic rewind of those records; the
     * alternative is silently deleting a local file the record was never shown
     * to describe, and a refusal the user can act on beats an unrecoverable
     * false success. */
    if (record.tree.isEmpty()) {
        return EpochRelation::OtherTree;
    }
    if (record.tree != live.tree) {
        return EpochRelation::OtherTree;
    }
    if (record.link != 0 && live.link != 0 && record.link != live.link) {
        return EpochRelation::OtherLink;
    }
    return EpochRelation::Same;
}

QSocFileHistory::Epoch QSocFileHistory::liveEpoch() const
{
    Epoch epoch;
    if (liveAccessor.tree) {
        epoch.tree = liveAccessor.tree();
    }
    if (epoch.tree.isEmpty()) {
        epoch.tree = QStringLiteral("unnamed");
    }
    if (liveAccessor.generation) {
        epoch.link = liveAccessor.generation();
    }
    return epoch;
}

QSet<QString> QSocFileHistory::trackedPathsFor(const QString &tree) const
{
    /* Only the paths actually tracked on this tree. A path carried over from a
     * snapshot that names no tree is never swept in here: capturing it would
     * stamp the live tree onto it and promote a fenced record into an
     * actionable one, so the next rewind could delete, create or overwrite it.
     * If the session really edits such a path, trackEdit() records it fresh
     * under the live tree, which is a genuine capture, not a promotion. */
    return trackedFiles.value(tree);
}

void QSocFileHistory::setLiveAccessor(LiveFileAccessor accessor)
{
    liveAccessor = std::move(accessor);
}

QSocFileHistory::QSocFileHistory(QString projectPath, QString sessionId)
    : projectPathValue(std::move(projectPath))
    , sessionIdValue(std::move(sessionId))
    , liveAccessor(localAccessor())
{
    /* Seed the trackedFiles set from any pre-existing snapshots so that
     * a resumed session continues to capture the same paths in its next
     * makeSnapshot call even if the current turn didn't touch them. Each path
     * lands under the tree it was observed on. */
    const auto snaps = loadSnapshots();
    for (const Snapshot &snap : snaps) {
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            trackedFiles[snap.epoch.tree].insert(it.key());
        }
    }
}

QString QSocFileHistory::historyDir(const QString &projectPath, const QString &sessionId)
{
    QString base = projectPath;
    if (base.isEmpty()) {
        base = QDir::currentPath();
    }
    return QDir(base).filePath(QStringLiteral(".qsoc/file-history/") + sessionId);
}

QString QSocFileHistory::sha256Hex(const QString &content)
{
    const QByteArray utf8   = content.toUtf8();
    const QByteArray digest = QCryptographicHash::hash(utf8, QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(digest);
}

QString QSocFileHistory::backupPathFor(const QString &sha256) const
{
    return QDir(historyDir(projectPathValue, sessionIdValue))
        .filePath(QStringLiteral("backups/") + sha256 + QStringLiteral(".bak"));
}

QString QSocFileHistory::snapshotsPath() const
{
    return QDir(historyDir(projectPathValue, sessionIdValue))
        .filePath(QStringLiteral("snapshots.jsonl"));
}

void QSocFileHistory::ensureDirs() const
{
    const QString root    = historyDir(projectPathValue, sessionIdValue);
    const QString backups = QDir(root).filePath(QStringLiteral("backups"));
    QDir().mkpath(root);
    QDir().mkpath(backups);
}

void QSocFileHistory::writeBackup(const QString &sha256, const QString &content) const
{
    ensureDirs();
    const QString path = backupPathFor(sha256);
    if (QFile::exists(path)) {
        return; /* already deduped by hash */
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    const QByteArray utf8 = content.toUtf8();
    file.write(utf8);
    file.close();
}

QString QSocFileHistory::readBackup(const QString &sha256) const
{
    const QString path = backupPathFor(sha256);
    QFile         file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray utf8 = file.readAll();
    file.close();
    return QString::fromUtf8(utf8);
}

void QSocFileHistory::trackEdit(
    const QString &filePath, bool beforeExists, const QString &beforeContent)
{
    /* If this file has already been tracked on this tree earlier in the
     * session, its baseline (the turn-0 "before the first edit" state) was
     * captured on the first call, so nothing to do now. Subsequent edits in the
     * same session will have their post-state captured by makeSnapshot(). A
     * path tracked only on another tree gets its own baseline below: the same
     * absolute path is a different file there. */
    const Epoch epoch = liveEpoch();
    if (trackedPathsFor(epoch.tree).contains(filePath)) {
        return;
    }
    trackedFiles[epoch.tree].insert(filePath);

    /* The caller already observed the file, so the baseline is never
     * unknown: it is present with a blob, or absent. */
    QString sha;
    if (beforeExists) {
        sha = sha256Hex(beforeContent);
        writeBackup(sha, beforeContent);
    }

    /* Merge the baseline into snapshot turn 0 for this tree. If the file
     * already has an entry there (paranoia: shouldn't happen because we just
     * added it to trackedFiles), leave the existing record alone. */
    QList<Snapshot> snapshots = loadSnapshots();
    Snapshot       *baseline  = nullptr;
    for (Snapshot &snap : snapshots) {
        /* Matched on the tree, not on the turn alone: the on-disk form keeps
         * one epoch per snapshot, so merging a second tree's baseline into this
         * line would claim this tree for every path already in it. */
        if (snap.turn == 0 && snap.epoch.tree == epoch.tree) {
            baseline = &snap;
            break;
        }
    }
    if (baseline == nullptr) {
        Snapshot fresh;
        fresh.turn      = 0;
        fresh.timestamp = QDateTime::currentDateTimeUtc();
        fresh.epoch     = epoch;
        fresh.files.insert(
            filePath,
            beforeExists ? FileRecord::present(sha, fresh.epoch) : FileRecord::absent(fresh.epoch));
        /* Appended, not prepended: snapshots are flattened in file order for
         * equal turns, so a baseline written later must win over one written
         * before it. */
        snapshots.append(fresh);
    } else if (!baseline->files.contains(filePath)) {
        baseline->files.insert(
            filePath,
            beforeExists ? FileRecord::present(sha, baseline->epoch)
                         : FileRecord::absent(baseline->epoch));
    }
    saveSnapshots(snapshots);
}

bool QSocFileHistory::makeSnapshot(int turn)
{
    if (turn <= 0) {
        return false; /* turn 0 is reserved for the lazily-populated baseline */
    }
    const Epoch         entryEpoch = liveEpoch();
    const QSet<QString> paths      = trackedPathsFor(entryEpoch.tree);
    if (paths.isEmpty()) {
        /* Nothing tracked on the bound tree; not an error. Paths tracked on
         * another tree are deliberately not read here: over this link the same
         * absolute path names a different file or nothing at all, and "nothing
         * at all" would be recorded as an instruction to unlink. */
        return true;
    }
    Snapshot snap;
    snap.turn      = turn;
    snap.timestamp = QDateTime::currentDateTimeUtc();
    snap.epoch     = entryEpoch;
    bool straddled = false;
    for (const QString &path : paths) {
        /* Once the transport has been replaced, the rest of the tree belongs
         * to a tree this snapshot is not describing, so claim nothing about
         * it rather than reading it on the new link. */
        if (straddled) {
            snap.files.insert(path, FileRecord::unknown(entryEpoch));
            continue;
        }
        const LiveRead live = liveAccessor.read ? liveAccessor.read(path) : LiveRead::unknown();
        /* This read crossed a transport swap, so it describes neither tree.
         * The turn entry is still written: effectiveStateAt takes the latest
         * record at or below a turn, so omitting the entry would make a
         * rewind here inherit the previous turn's records and unlink a file
         * that this turn created. */
        if (liveEpoch() != entryEpoch) {
            straddled = true;
            snap.files.insert(path, FileRecord::unknown(entryEpoch));
            continue;
        }
        switch (live.state()) {
        case FileState::Unknown:
            /* Recorded as unknown, never as absent: absent is an instruction
             * to unlink, and we did not establish that there is nothing
             * there to unlink. */
            snap.files.insert(path, FileRecord::unknown(entryEpoch));
            break;
        case FileState::Absent:
            snap.files.insert(path, FileRecord::absent(entryEpoch));
            break;
        case FileState::Present: {
            const QString sha = sha256Hex(live.content());
            writeBackup(sha, live.content());
            snap.files.insert(path, FileRecord::present(sha, entryEpoch));
            break;
        }
        }
    }

    /* Append in-memory, trim to MAX_SNAPSHOTS (baseline is sticky), then
     * rewrite snapshots.jsonl atomically via saveSnapshots. */
    QList<Snapshot> snapshots = loadSnapshots();
    snapshots.append(snap);
    /* Keep baseline plus the newest (MAX_SNAPSHOTS - 1) regular turns. */
    while (snapshots.size() > MAX_SNAPSHOTS) {
        int dropIndex = -1;
        for (int i = 0; i < snapshots.size(); i++) {
            if (snapshots[i].turn != 0) {
                dropIndex = i;
                break;
            }
        }
        if (dropIndex < 0) {
            break;
        }
        snapshots.removeAt(dropIndex);
    }
    saveSnapshots(snapshots);
    gcOrphanedBackups();
    return true;
}

QMap<QString, QSocFileHistory::FileRecord> QSocFileHistory::effectiveStateAt(
    int turn, const Epoch &live) const
{
    const bool                scoped = !live.tree.isEmpty();
    QMap<QString, FileRecord> state;
    const auto                snapshots = loadSnapshots();
    for (const Snapshot &snap : snapshots) {
        if (snap.turn > turn) {
            continue;
        }
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            /* A turn that ran on another tree cannot have changed this one, so
             * its records are not a later truth about these paths: dropping
             * them leaves each path at its most recent record on the bound
             * tree, which is what that file still holds. A tree-less record is
             * OtherTree by relate() and dropped here too: its origin cannot be
             * proven, so it may not act on a colliding path on any tree. */
            if (scoped && relate(it.value().epoch(), live) == EpochRelation::OtherTree) {
                continue;
            }
            /* Later snapshots overwrite earlier ones for the same path. */
            state.insert(it.key(), it.value());
        }
    }
    return state;
}

QSocFileHistory::BoundaryPreview QSocFileHistory::previewBoundary(int turn) const
{
    BoundaryPreview preview;
    const Epoch     live  = liveEpoch();
    const auto      state = effectiveStateAt(turn, live);
    QSet<QString>   elsewhere;
    for (const Snapshot &snap : loadSnapshots()) {
        if (snap.turn > turn) {
            continue;
        }
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            /* Recorded at or below the target turn, yet absent from the scoped
             * state: every record it has belongs to another tree. */
            if (!it.value().isUnknown() && !state.contains(it.key())) {
                elsewhere.insert(it.key());
            }
        }
    }
    preview.otherTree = QStringList(elsewhere.begin(), elsewhere.end());
    for (auto it = state.begin(); it != state.end(); ++it) {
        if (!it.value().isUnknown()
            && relate(it.value().epoch(), live) == EpochRelation::OtherLink) {
            preview.otherLink.append(it.key());
        }
    }
    std::sort(preview.otherTree.begin(), preview.otherTree.end());
    std::sort(preview.otherLink.begin(), preview.otherLink.end());
    return preview;
}

QSocFileHistory::RestoreReport QSocFileHistory::applySnapshot(int turn, AcrossGeneration across)
{
    RestoreReport report;
    const Epoch   entryEpoch = liveEpoch();
    const auto    state      = effectiveStateAt(turn, entryEpoch);
    if (state.isEmpty()) {
        return report;
    }
    bool stopped = false;
    for (auto it = state.begin(); it != state.end(); ++it) {
        const QString    &path = it.key();
        const FileRecord &rec  = it.value();
        if (stopped) {
            report.failed.append(path);
            continue;
        }
        if (rec.isUnknown()) {
            report.unknown.append(path);
            continue;
        }
        const Epoch live = liveEpoch();
        if (live != entryEpoch) {
            /* The link this restore started on is gone; every path from here
             * on is unattempted and the tree is half-way between two turns. */
            report.transportChanged = true;
            stopped                 = true;
            report.failed.append(path);
            continue;
        }
        /* effectiveStateAt dropped every record from another tree, so the only
         * boundary left here is a replaced connection to this one. */
        if (relate(rec.epoch(), live) == EpochRelation::OtherLink
            && across == AcrossGeneration::Refuse) {
            report.unknown.append(path);
            continue;
        }
        if (rec.isAbsent()) {
            /* Absent at the target turn: unlink, but only once the accessor
             * says there is something there. */
            const FileState liveState = liveAccessor.exists ? liveAccessor.exists(path)
                                                            : FileState::Unknown;
            switch (liveState) {
            case FileState::Unknown:
                report.unknown.append(path);
                break;
            case FileState::Absent:
                break; /* already matches the target turn */
            case FileState::Present:
                if (liveAccessor.remove && liveAccessor.remove(path)) {
                    report.restored.append(path);
                } else {
                    report.failed.append(path);
                }
                break;
            }
            continue;
        }
        const QString content = readBackup(rec.sha256());
        if (content.isNull()) {
            /* Leaving the file alone beats corrupting it, but the caller
             * must not be told the tree is back where it was. */
            report.failed.append(path);
            continue;
        }
        if (liveAccessor.write && liveAccessor.write(path, content)) {
            report.restored.append(path);
        } else {
            report.failed.append(path);
        }
    }
    std::sort(report.restored.begin(), report.restored.end());
    std::sort(report.failed.begin(), report.failed.end());
    std::sort(report.unknown.begin(), report.unknown.end());
    return report;
}

void QSocFileHistory::truncateAfter(int cutoffTurn)
{
    QList<Snapshot> snapshots = loadSnapshots();
    QList<Snapshot> kept;
    kept.reserve(snapshots.size());
    for (const Snapshot &snap : snapshots) {
        if (snap.turn <= cutoffTurn) {
            kept.append(snap);
        }
    }
    if (kept.size() != snapshots.size()) {
        saveSnapshots(kept);
        gcOrphanedBackups();
    }
}

QList<QSocFileHistory::Snapshot> QSocFileHistory::listSnapshots() const
{
    return loadSnapshots();
}

QString QSocFileHistory::contentAt(const QString &filePath, int turn) const
{
    /* Unscoped on purpose: this reads a backup blob and writes nothing, and
     * /diff asks about paths from every tree in the session. */
    const auto state = effectiveStateAt(turn, Epoch());
    if (!state.contains(filePath)) {
        return QString();
    }
    const FileRecord rec = state.value(filePath);
    if (!rec.isPresent()) {
        return QString();
    }
    return readBackup(rec.sha256());
}

int QSocFileHistory::latestTurn() const
{
    const auto snapshots = loadSnapshots();
    int        latest    = 0;
    for (const Snapshot &snap : snapshots) {
        if (snap.turn > latest) {
            latest = snap.turn;
        }
    }
    return latest;
}

bool QSocFileHistory::isEmpty() const
{
    return loadSnapshots().isEmpty();
}

QList<QSocFileHistory::Snapshot> QSocFileHistory::loadSnapshots() const
{
    if (cacheValid) {
        return cachedSnapshots;
    }
    QList<Snapshot> result;
    QFile           file(snapshotsPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        cachedSnapshots = result;
        cacheValid      = true;
        return result;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.isEmpty()) {
            continue;
        }
        try {
            const json doc = json::parse(line.toStdString());
            if (!doc.is_object() || !doc.contains("turn") || !doc.contains("files")) {
                continue;
            }
            Snapshot snap;
            snap.turn = doc["turn"].get<int>();
            if (doc.contains("ts") && doc["ts"].is_string()) {
                snap.timestamp = QDateTime::fromString(
                    QString::fromStdString(doc["ts"].get<std::string>()), Qt::ISODateWithMs);
            }
            /* A line written before these fields existed names no tree and
             * reads as link 0. It still loads: what it may be acted on is
             * relate()'s decision, not the parser's. */
            if (doc.contains("tree") && doc["tree"].is_string()) {
                snap.epoch.tree = QString::fromStdString(doc["tree"].get<std::string>());
            }
            if (doc.contains("gen") && doc["gen"].is_number_unsigned()) {
                snap.epoch.link = doc["gen"].get<quint64>();
            }
            const auto &files = doc["files"];
            if (files.is_object()) {
                for (auto it = files.begin(); it != files.end(); ++it) {
                    const QString path = QString::fromStdString(it.key());
                    /* Classified by JSON type plus shape: null is absent, a
                     * well-formed digest is present, and everything else
                     * (including a torn value) is unknown. */
                    FileRecord rec = FileRecord::unknown(snap.epoch);
                    if (it.value().is_null()) {
                        rec = FileRecord::absent(snap.epoch);
                    } else if (it.value().is_string()) {
                        const QString sha = QString::fromStdString(it.value().get<std::string>());
                        if (isSha256Hex(sha)) {
                            rec = FileRecord::present(sha, snap.epoch);
                        }
                    }
                    snap.files.insert(path, rec);
                }
            }
            result.append(snap);
        } catch (const std::exception &) {
            /* Skip malformed line — probably a torn write from a crash. */
            continue;
        }
    }
    file.close();
    /* Stable: two snapshots can share a turn (one baseline per tree, plus a
     * legacy baseline that names none), and flattening must take the one
     * written later, not whichever the sort happened to leave last. */
    std::stable_sort(result.begin(), result.end(), [](const Snapshot &lhs, const Snapshot &rhs) {
        return lhs.turn < rhs.turn;
    });
    cachedSnapshots = result;
    cacheValid      = true;
    return result;
}

void QSocFileHistory::saveSnapshots(const QList<Snapshot> &snapshots) const
{
    ensureDirs();
    const QString path = snapshotsPath();
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    for (const Snapshot &snap : snapshots) {
        json doc;
        doc["turn"] = snap.turn;
        doc["ts"]   = (snap.timestamp.isValid() ? snap.timestamp : QDateTime::currentDateTimeUtc())
                          .toString(Qt::ISODateWithMs)
                          .toStdString();
        /* Omitted when unnamed so a legacy line stays a legacy line through a
         * rewrite instead of gaining a tree it was never captured on. */
        if (!snap.epoch.tree.isEmpty()) {
            doc["tree"] = snap.epoch.tree.toStdString();
        }
        doc["gen"]    = snap.epoch.link;
        json filesObj = json::object();
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            if (it.value().isPresent()) {
                filesObj[it.key().toStdString()] = it.value().sha256().toStdString();
            } else if (it.value().isAbsent()) {
                filesObj[it.key().toStdString()] = nullptr;
            } else {
                filesObj[it.key().toStdString()] = "unknown";
            }
        }
        doc["files"]                 = filesObj;
        const std::string serialized = doc.dump();
        file.write(serialized.data(), static_cast<qint64>(serialized.size()));
        file.write("\n", 1);
    }
    file.close();
    cachedSnapshots = snapshots;
    cacheValid      = true;
}

void QSocFileHistory::gcOrphanedBackups() const
{
    /* Collect every sha256 still referenced by a surviving snapshot. */
    const auto    snapshots = loadSnapshots();
    QSet<QString> referenced;
    for (const Snapshot &snap : snapshots) {
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            /* Gated on isPresent(), not on a non-empty digest: an unknown
             * record names no blob and must not keep one alive. */
            if (it.value().isPresent()) {
                referenced.insert(it.value().sha256());
            }
        }
    }

    const QString backupsDir
        = QDir(historyDir(projectPathValue, sessionIdValue)).filePath(QStringLiteral("backups"));
    QDir dir(backupsDir);
    if (!dir.exists()) {
        return;
    }
    const auto entries = dir.entryInfoList({QStringLiteral("*.bak")}, QDir::Files);
    for (const QFileInfo &entry : entries) {
        const QString sha = entry.completeBaseName();
        if (!referenced.contains(sha)) {
            QFile::remove(entry.absoluteFilePath());
        }
    }
}
