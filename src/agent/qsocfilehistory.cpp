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

} // namespace

QSocFileHistory::LiveFileAccessor QSocFileHistory::localAccessor()
{
    LiveFileAccessor accessor;
    accessor.exists = [](const QString &path) {
        if (QFileInfo::exists(path)) {
            return FileState::Present;
        }
        /* QFileInfo::exists() is also false for a path we were not allowed
         * to stat and for a stale mount handle, so "not there" is a fact
         * only when the parent directory could answer for its children. */
        const QFileInfo parent(QFileInfo(path).absolutePath());
        if (parent.isDir() && parent.isReadable() && parent.isExecutable()) {
            return FileState::Absent;
        }
        return FileState::Unknown;
    };
    accessor.read = [](const QString &path) -> LiveRead {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray utf8 = file.readAll();
            file.close();
            return LiveRead::present(QString::fromUtf8(utf8));
        }
        /* An open that failed on a file that is there (EACCES, EIO, ELOOP)
         * says nothing about its content. */
        return QFileInfo::exists(path) ? LiveRead::unknown() : LiveRead::absent();
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
    /* generation stays unset: the local disk is never replaced underneath a
     * capture, so both generation checks are disabled here. */
    return accessor;
}

quint64 QSocFileHistory::liveGeneration() const
{
    return liveAccessor.generation ? liveAccessor.generation() : 0;
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
     * makeSnapshot call even if the current turn didn't touch them. */
    const auto snaps = loadSnapshots();
    for (const Snapshot &snap : snaps) {
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            trackedFiles.insert(it.key());
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
    /* If this file has already been tracked earlier in the session, its
     * baseline (the turn-0 "before the first edit" state) was captured on
     * the first call — nothing to do now. Subsequent edits in the same
     * session will have their post-state captured by makeSnapshot(). */
    if (trackedFiles.contains(filePath)) {
        return;
    }
    trackedFiles.insert(filePath);

    /* The caller already observed the file, so the baseline is never
     * unknown: it is present with a blob, or absent. */
    QString sha;
    if (beforeExists) {
        sha = sha256Hex(beforeContent);
        writeBackup(sha, beforeContent);
    }

    /* Merge the baseline into snapshot turn 0. If the file already has an
     * entry at turn 0 (paranoia — shouldn't happen because we just added
     * it to trackedFiles), leave the existing record alone. */
    QList<Snapshot> snapshots = loadSnapshots();
    Snapshot       *baseline  = nullptr;
    for (Snapshot &snap : snapshots) {
        if (snap.turn == 0) {
            baseline = &snap;
            break;
        }
    }
    /* Records carry the baseline's own generation, not the one bound right
     * now: the on-disk form keeps one generation per snapshot, so stamping a
     * later transport here would claim it for every path already recorded. */
    if (baseline == nullptr) {
        Snapshot fresh;
        fresh.turn       = 0;
        fresh.timestamp  = QDateTime::currentDateTimeUtc();
        fresh.generation = liveGeneration();
        fresh.files.insert(
            filePath,
            beforeExists ? FileRecord::present(sha, fresh.generation)
                         : FileRecord::absent(fresh.generation));
        snapshots.prepend(fresh);
    } else if (!baseline->files.contains(filePath)) {
        baseline->files.insert(
            filePath,
            beforeExists ? FileRecord::present(sha, baseline->generation)
                         : FileRecord::absent(baseline->generation));
    }
    saveSnapshots(snapshots);
}

bool QSocFileHistory::makeSnapshot(int turn)
{
    if (turn <= 0) {
        return false; /* turn 0 is reserved for the lazily-populated baseline */
    }
    if (trackedFiles.isEmpty()) {
        return true; /* nothing to snapshot; not an error */
    }
    const quint64 entryGeneration = liveGeneration();
    Snapshot      snap;
    snap.turn       = turn;
    snap.timestamp  = QDateTime::currentDateTimeUtc();
    snap.generation = entryGeneration;
    bool straddled  = false;
    for (const QString &path : trackedFiles) {
        /* Once the transport has been replaced, the rest of the tree belongs
         * to a tree this snapshot is not describing, so claim nothing about
         * it rather than reading it on the new link. */
        if (straddled) {
            snap.files.insert(path, FileRecord::unknown(entryGeneration));
            continue;
        }
        const LiveRead live = liveAccessor.read ? liveAccessor.read(path) : LiveRead::unknown();
        /* This read crossed a transport swap, so it describes neither tree.
         * The turn entry is still written: effectiveStateAt takes the latest
         * record at or below a turn, so omitting the entry would make a
         * rewind here inherit the previous turn's records and unlink a file
         * that this turn created. */
        if (liveGeneration() != entryGeneration) {
            straddled = true;
            snap.files.insert(path, FileRecord::unknown(entryGeneration));
            continue;
        }
        switch (live.state()) {
        case FileState::Unknown:
            /* Recorded as unknown, never as absent: absent is an instruction
             * to unlink, and we did not establish that there is nothing
             * there to unlink. */
            snap.files.insert(path, FileRecord::unknown(entryGeneration));
            break;
        case FileState::Absent:
            snap.files.insert(path, FileRecord::absent(entryGeneration));
            break;
        case FileState::Present: {
            const QString sha = sha256Hex(live.content());
            writeBackup(sha, live.content());
            snap.files.insert(path, FileRecord::present(sha, entryGeneration));
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

QMap<QString, QSocFileHistory::FileRecord> QSocFileHistory::effectiveStateAt(int turn) const
{
    QMap<QString, FileRecord> state;
    const auto                snapshots = loadSnapshots();
    for (const Snapshot &snap : snapshots) {
        if (snap.turn > turn) {
            continue;
        }
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            /* Later snapshots overwrite earlier ones for the same path. */
            state.insert(it.key(), it.value());
        }
    }
    return state;
}

QSocFileHistory::RestoreReport QSocFileHistory::applySnapshot(int turn, AcrossGeneration across)
{
    RestoreReport report;
    const auto    state = effectiveStateAt(turn);
    if (state.isEmpty()) {
        return report;
    }
    const quint64 entryGeneration = liveGeneration();
    bool          stopped         = false;
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
        const quint64 live = liveGeneration();
        if (live != entryGeneration) {
            /* The link this restore started on is gone; every path from here
             * on is unattempted and the tree is half-way between two turns. */
            report.transportChanged = true;
            stopped                 = true;
            report.failed.append(path);
            continue;
        }
        const bool crossed = live != 0 && rec.generation() != 0 && rec.generation() != live;
        if (crossed && across == AcrossGeneration::Refuse) {
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
    const auto state = effectiveStateAt(turn);
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
            /* A line written before "gen" existed reads as 0, which never
             * trips the cross-generation guard. */
            if (doc.contains("gen") && doc["gen"].is_number_unsigned()) {
                snap.generation = doc["gen"].get<quint64>();
            }
            const auto &files = doc["files"];
            if (files.is_object()) {
                for (auto it = files.begin(); it != files.end(); ++it) {
                    const QString path = QString::fromStdString(it.key());
                    /* Classified by JSON type plus shape: null is absent, a
                     * well-formed digest is present, and everything else
                     * (including a torn value) is unknown. */
                    FileRecord rec = FileRecord::unknown(snap.generation);
                    if (it.value().is_null()) {
                        rec = FileRecord::absent(snap.generation);
                    } else if (it.value().is_string()) {
                        const QString sha = QString::fromStdString(it.value().get<std::string>());
                        if (isSha256Hex(sha)) {
                            rec = FileRecord::present(sha, snap.generation);
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
    std::sort(result.begin(), result.end(), [](const Snapshot &lhs, const Snapshot &rhs) {
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
        doc["gen"]  = snap.generation;
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
