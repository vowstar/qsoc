// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocfilehistory.h"

#include <nlohmann/json.hpp>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

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

QString sha256Bytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool pathWithin(const QString &path, const QString &root)
{
    QString       prefix    = QDir::fromNativeSeparators(QDir::cleanPath(root));
    const QString cleanPath = QDir::fromNativeSeparators(QDir::cleanPath(path));
    if (cleanPath.compare(prefix, pathCaseSensitivity()) == 0) {
        return true;
    }
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
    return cleanPath.startsWith(prefix, pathCaseSensitivity());
}

bool pathInLocalScope(const QString &path, const QString &lexicalRoot, const QString &canonicalRoot)
{
    const QString clean = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    return pathWithin(clean, lexicalRoot) || pathWithin(clean, canonicalRoot);
}

/* Resolve only the parent. History records the actual file reached by the
 * original tool, so following a later leaf symlink would switch objects.
 * Requiring the parent to retain its canonical spelling also rejects an
 * ancestor that was replaced by a symlink after capture. */
QString readLocalTreeId(const QString &canonicalRoot)
{
    const QString   metadata = QDir(canonicalRoot).filePath(QStringLiteral(".qsoc"));
    const QFileInfo metadataInfo(metadata);
    if (!metadataInfo.isDir() || metadataInfo.isSymLink()
        || metadataInfo.canonicalFilePath().compare(QDir::cleanPath(metadata), pathCaseSensitivity())
               != 0) {
        return {};
    }
    const QString   treeIdPath = QDir(metadata).filePath(QStringLiteral("tree-id"));
    const QFileInfo treeIdInfo(treeIdPath);
    if (treeIdInfo.isSymLink()) {
        return {};
    }
    QFile file(treeIdPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll().trimmed();
    if (file.error() != QFileDevice::NoError) {
        return {};
    }
    const QUuid id(QString::fromLatin1(bytes));
    return id.isNull() ? QString() : id.toString(QUuid::WithoutBraces);
}

QString ensureLocalTreeId(const QString &canonicalRoot)
{
    QString id = readLocalTreeId(canonicalRoot);
    if (!id.isEmpty()) {
        return id;
    }
    const QString   metadata = QDir(canonicalRoot).filePath(QStringLiteral(".qsoc"));
    const QFileInfo metadataInfo(metadata);
    if ((metadataInfo.exists() || metadataInfo.isSymLink())
            ? (!metadataInfo.isDir() || metadataInfo.isSymLink()
               || metadataInfo.canonicalFilePath()
                          .compare(QDir::cleanPath(metadata), pathCaseSensitivity())
                      != 0)
            : !QDir(canonicalRoot).mkdir(QStringLiteral(".qsoc"))) {
        return {};
    }
    const QString    path = QDir(metadata).filePath(QStringLiteral("tree-id"));
    QFile            file(path);
    const QString    generated = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray bytes     = generated.toLatin1() + '\n';
    if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const bool written = file.write(bytes) == bytes.size() && file.flush()
                             && file.error() == QFileDevice::NoError;
        file.close();
        if (written) {
            return generated;
        }
    }
    return readLocalTreeId(canonicalRoot);
}

QString localTreeIdentity(const QString &treeId)
{
    if (treeId.isEmpty()) {
        return {};
    }
    return sha256Bytes(treeId.toUtf8());
}

bool localRootIsBound(const QString &lexicalRoot, const QString &canonicalRoot, const QString &treeId)
{
    return !canonicalRoot.isEmpty() && !treeId.isEmpty()
           && QFileInfo(lexicalRoot).canonicalFilePath() == canonicalRoot
           && readLocalTreeId(canonicalRoot) == treeId;
}

bool boundMetadataDirectory(const QString &canonicalRoot, const QString &relativePath)
{
    QString current = canonicalRoot;
    for (const QString &part : relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (!info.exists() && !info.isSymLink()) {
            return true;
        }
        if (!info.isDir() || info.isSymLink()
            || info.canonicalFilePath().compare(QDir::cleanPath(current), pathCaseSensitivity())
                   != 0) {
            return false;
        }
    }
    return true;
}

bool makeBoundMetadataDirectory(const QString &canonicalRoot, const QString &relativePath)
{
    QString current;
    for (const QString &part : relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        current                  = current.isEmpty() ? part : QDir(current).filePath(part);
        const QString   absolute = QDir(canonicalRoot).filePath(current);
        const QFileInfo info(absolute);
        if (!info.exists() && !info.isSymLink()) {
            const QString parentRelative = QFileInfo(current).path();
            QDir          parent(
                parentRelative == QStringLiteral(".")
                    ? canonicalRoot
                    : QDir(canonicalRoot).filePath(parentRelative));
            if (!parent.mkdir(QFileInfo(current).fileName())) {
                return false;
            }
        }
        if (!boundMetadataDirectory(canonicalRoot, current)) {
            return false;
        }
    }
    return true;
}

std::optional<QString> localHistoryEntry(
    const QString                                &path,
    const QString                                &lexicalRoot,
    const QString                                &canonicalRoot,
    const QString                                &treeId,
    const QSocFileHistory::WritableEntryResolver &resolver)
{
    if (!localRootIsBound(lexicalRoot, canonicalRoot, treeId)) {
        return std::nullopt;
    }
    const QString clean = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    const QFileInfo info(clean);
    QString         probe = info.absolutePath();
    QStringList     tail;
    QString         canonicalParent;
    for (;;) {
        const QFileInfo parent(probe);
        if (parent.exists() || parent.isSymLink()) {
            if (!parent.isDir()) {
                return std::nullopt;
            }
            canonicalParent = QDir::fromNativeSeparators(parent.canonicalFilePath());
            break;
        }
        const QString name = parent.fileName();
        const QString next = parent.absolutePath();
        if (name.isEmpty() || next == probe) {
            return std::nullopt;
        }
        tail.prepend(name);
        probe = next;
    }
    if (canonicalParent.isEmpty()) {
        return std::nullopt;
    }
    for (const QString &part : std::as_const(tail)) {
        canonicalParent = QDir(canonicalParent).filePath(part);
    }
    const QString entry = QDir::fromNativeSeparators(
        QDir(canonicalParent).filePath(info.fileName()));
    if (entry.compare(clean, pathCaseSensitivity()) != 0) {
        return std::nullopt;
    }
    if (!pathWithin(entry, canonicalRoot)) {
        return std::nullopt;
    }
    if (resolver) {
        QString allowedEntry;
        if (!resolver(entry, &allowedEntry)
            || QDir::fromNativeSeparators(allowedEntry).compare(entry, pathCaseSensitivity()) != 0) {
            return std::nullopt;
        }
    }
    return clean;
}

QSocFileHistory::FileRecord withIntroducedTurn(
    const QSocFileHistory::FileRecord &record, int introducedTurn)
{
    if (record.isPresent()) {
        return QSocFileHistory::FileRecord::present(record.sha256(), record.epoch(), introducedTurn);
    }
    if (record.isAbsent()) {
        return QSocFileHistory::FileRecord::absent(record.epoch(), introducedTurn);
    }
    return QSocFileHistory::FileRecord::unknown(record.epoch(), introducedTurn);
}

void evictOldestSnapshot(QList<QSocFileHistory::Snapshot> &snapshots)
{
    const QSocFileHistory::Snapshot dropped = snapshots.takeFirst();
    for (auto droppedIt = dropped.files.cbegin(); droppedIt != dropped.files.cend(); ++droppedIt) {
        const QSocFileHistory::FileRecord droppedRecord = droppedIt.value();
        if (droppedRecord.isUnknown() || droppedRecord.introducedTurn() != dropped.turn) {
            continue;
        }

        const QString                path             = droppedIt.key();
        const QSocFileHistory::Epoch epoch            = dropped.epoch;
        const int                    oldSince         = droppedRecord.introducedTurn();
        bool                         baselineSurvives = false;
        int                          newSince         = -1;
        for (const QSocFileHistory::Snapshot &snap : snapshots) {
            if (snap.epoch != epoch || !snap.files.contains(path)) {
                continue;
            }
            const QSocFileHistory::FileRecord record = snap.files.value(path);
            if (record.introducedTurn() != oldSince || record.isUnknown()) {
                continue;
            }
            if (snap.turn == oldSince) {
                baselineSurvives = true;
                break;
            }
            if (newSince < 0 || snap.turn < newSince) {
                newSince = snap.turn;
            }
        }
        if (baselineSurvives) {
            continue;
        }
        for (QSocFileHistory::Snapshot &snap : snapshots) {
            auto recordIt = snap.files.find(path);
            if (snap.epoch != epoch || recordIt == snap.files.end()
                || recordIt.value().introducedTurn() != oldSince) {
                continue;
            }
            if (newSince < 0 || snap.turn < newSince) {
                snap.files.erase(recordIt);
            } else {
                recordIt.value() = withIntroducedTurn(recordIt.value(), newSince);
            }
        }
    }
}

void trimSnapshots(QList<QSocFileHistory::Snapshot> &snapshots)
{
    std::stable_sort(snapshots.begin(), snapshots.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.turn < rhs.turn;
    });
    while (snapshots.size() > QSocFileHistory::MAX_SNAPSHOTS) {
        evictOldestSnapshot(snapshots);
    }
}

} // namespace

QSocFileHistory::LiveFileAccessor QSocFileHistory::localAccessor(
    const QString &projectRoot, WritableEntryResolver resolver)
{
    const QString lexicalRoot
        = QFileInfo(projectRoot.isEmpty() ? QDir::currentPath() : projectRoot).absoluteFilePath();
    const QString canonicalRoot = QFileInfo(lexicalRoot).canonicalFilePath();
    const QString treeId        = ensureLocalTreeId(canonicalRoot);
    const QString treeIdentity  = localTreeIdentity(treeId);
    const auto    resolve = [lexicalRoot, canonicalRoot, treeId, resolver](const QString &path) {
        return localHistoryEntry(path, lexicalRoot, canonicalRoot, treeId, resolver);
    };
    LiveFileAccessor accessor;
    accessor.exists = [resolve](const QString &path) {
        const auto entry = resolve(path);
        if (!entry.has_value()) {
            return FileState::Unknown;
        }
        const QFileInfo info(*entry);
        if (info.exists() || info.isSymLink()) {
            return FileState::Present;
        }
        const QFileInfo parent(info.absolutePath());
        if (parent.isDir() && parent.isReadable() && parent.isExecutable()) {
            return FileState::Absent;
        }
        return FileState::Unknown;
    };
    accessor.read = [resolve](const QString &path) -> LiveRead {
        const auto entry = resolve(path);
        if (!entry.has_value()) {
            return LiveRead::unknown();
        }
        const QFileInfo info(*entry);
        if (info.isSymLink()) {
            return LiveRead::unknown();
        }
        QFile file(*entry);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray utf8     = file.readAll();
            const bool       complete = file.error() == QFileDevice::NoError;
            file.close();
            if (complete) {
                return LiveRead::present(QString::fromUtf8(utf8.constData(), utf8.size()));
            }
            return LiveRead::unknown();
        }
        const QFileInfo parent(info.absolutePath());
        if (!info.exists() && !info.isSymLink() && parent.isDir() && parent.isReadable()
            && parent.isExecutable()) {
            return LiveRead::absent();
        }
        return LiveRead::unknown();
    };
    accessor.write = [resolve](const QString &path, const QString &content) {
        const auto entry = resolve(path);
        if (!entry.has_value()) {
            return false;
        }
        const QByteArray utf8 = content.toUtf8();
        QSaveFile        file(*entry);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(utf8) != utf8.size()) {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    };
    accessor.remove = [resolve](const QString &path) {
        const auto entry = resolve(path);
        if (!entry.has_value()) {
            return false;
        }
        const QFileInfo info(*entry);
        if (!info.exists() && !info.isSymLink()) {
            return true;
        }
        return QFile::remove(*entry);
    };
    accessor.inScope = [lexicalRoot, canonicalRoot](const QString &path) {
        return pathInLocalScope(path, lexicalRoot, canonicalRoot);
    };
    accessor.coversPath = [resolve](const QString &path) { return resolve(path).has_value(); };
    accessor.tree       = [lexicalRoot, canonicalRoot, treeId, treeIdentity]() {
        if (treeIdentity.isEmpty() || !localRootIsBound(lexicalRoot, canonicalRoot, treeId)) {
            return QString();
        }
        return QStringLiteral("local:") + treeIdentity;
    };
    accessor.generation = []() { return QStringLiteral("disk"); };
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
    if (record.tree.isEmpty() || live.tree.isEmpty()) {
        return EpochRelation::OtherTree;
    }
    if (record.tree != live.tree) {
        return EpochRelation::OtherTree;
    }
    if (record.link.isEmpty() || live.link.isEmpty() || record.link != live.link) {
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
    if (liveAccessor.generation) {
        epoch.link = liveAccessor.generation();
    }
    return epoch;
}

QSet<QString> QSocFileHistory::trackedPathsFor(const Epoch &epoch, int atTurn) const
{
    QSet<QString> paths;
    if (epoch.tree.isEmpty() || epoch.link.isEmpty()) {
        return paths;
    }
    for (const Snapshot &snap : loadSnapshots()) {
        if (snap.epoch != epoch) {
            continue;
        }
        for (auto it = snap.files.cbegin(); it != snap.files.cend(); ++it) {
            if (it.value().introducedTurn() <= atTurn) {
                paths.insert(it.key());
            }
        }
    }
    return paths;
}

int QSocFileHistory::introducedTurnFor(const Epoch &epoch, const QString &path) const
{
    int introduced = -1;
    for (const Snapshot &snap : loadSnapshots()) {
        if (snap.epoch != epoch || !snap.files.contains(path)) {
            continue;
        }
        const int candidate = snap.files.value(path).introducedTurn();
        if (introduced < 0) {
            introduced = candidate;
        } else if (introduced != candidate) {
            return -1;
        }
    }
    return introduced;
}

void QSocFileHistory::setLiveAccessor(LiveFileAccessor accessor)
{
    liveAccessor = std::move(accessor);
}

bool QSocFileHistory::isPathInScope(const QString &filePath) const
{
    return !liveAccessor.inScope || liveAccessor.inScope(filePath);
}

bool QSocFileHistory::coversPath(const QString &filePath) const
{
    return isPathInScope(filePath)
           && (!liveAccessor.coversPath || liveAccessor.coversPath(filePath));
}

QSocFileHistory::QSocFileHistory(QString projectPath, QString sessionId)
    : sessionIdValue(std::move(sessionId))
{
    const QString root      = projectPath.isEmpty() ? QDir::currentPath() : std::move(projectPath);
    storageLexicalRootValue = QFileInfo(root).absoluteFilePath();
    storageCanonicalRootValue = QFileInfo(storageLexicalRootValue).canonicalFilePath();
    storageTreeIdValue        = ensureLocalTreeId(storageCanonicalRootValue);
    projectPathValue          = storageLexicalRootValue;
    liveAccessor              = localAccessor(projectPathValue);
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
    return QDir(historyDir(storageCanonicalRootValue, sessionIdValue))
        .filePath(QStringLiteral("backups/") + sha256 + QStringLiteral(".bak"));
}

QString QSocFileHistory::snapshotsPath() const
{
    return QDir(historyDir(storageCanonicalRootValue, sessionIdValue))
        .filePath(QStringLiteral("snapshots.jsonl"));
}

bool QSocFileHistory::ensureDirs() const
{
    if (!storageIsBound()) {
        return false;
    }
    const QString relative = QStringLiteral(".qsoc/file-history/%1/backups").arg(sessionIdValue);
    return makeBoundMetadataDirectory(storageCanonicalRootValue, relative) && storageIsBound();
}

bool QSocFileHistory::writeBackup(const QString &sha256, const QString &content) const
{
    if (!ensureDirs()) {
        return false;
    }
    const QString path = backupPathFor(sha256);
    if (QFileInfo(path).isSymLink()) {
        return false;
    }
    if (readBackup(sha256).has_value()) {
        return true;
    }
    const QByteArray utf8 = content.toUtf8();
    if (QString::fromLatin1(QCryptographicHash::hash(utf8, QCryptographicHash::Sha256).toHex())
        != sha256) {
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(utf8) != utf8.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

std::optional<QString> QSocFileHistory::readBackup(const QString &sha256) const
{
    if (!storageIsBound()) {
        return std::nullopt;
    }
    const QString path = backupPathFor(sha256);
    if (QFileInfo(path).isSymLink()) {
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray utf8 = file.readAll();
    file.close();
    const QString actual = QString::fromLatin1(
        QCryptographicHash::hash(utf8, QCryptographicHash::Sha256).toHex());
    if (actual != sha256) {
        return std::nullopt;
    }
    return QString::fromUtf8(utf8.constData(), utf8.size());
}

bool QSocFileHistory::trackEdit(
    const QString &filePath, bool beforeExists, const QString &beforeContent)
{
    /* A tracked path already has a baseline at its introduction checkpoint, so
     * later edits need no new one. makeSnapshot() captures their post-state. A
     * path tracked only on another tree gets its own baseline below because the
     * same absolute path names a different file there. */
    if (!coversPath(filePath)) {
        return false;
    }
    const Epoch epoch = liveEpoch();
    if (epoch.tree.isEmpty() || epoch.link.isEmpty()) {
        return false;
    }
    (void) loadSnapshots();
    if (indexState == IndexState::Invalid) {
        return false;
    }
    const int existingIntroduction = introducedTurnFor(epoch, filePath);
    if (existingIntroduction >= 0) {
        const int introduced    = existingIntroduction;
        bool      baselineFound = false;
        for (const Snapshot &snap : loadSnapshots()) {
            if (snap.epoch != epoch || !snap.files.contains(filePath)) {
                continue;
            }
            const FileRecord record = snap.files.value(filePath);
            if (snap.turn == introduced && record.introducedTurn() == introduced
                && !record.isUnknown()) {
                baselineFound = true;
            }
            if (record.isPresent() && !readBackup(record.sha256()).has_value()) {
                return false;
            }
        }
        return baselineFound;
    }

    /* The caller already observed the file, so the durable baseline is either
     * present with a blob or absent. A publication failure rejects the edit. */
    QString sha;
    bool    backupStored = true;
    if (beforeExists) {
        sha          = sha256Hex(beforeContent);
        backupStored = writeBackup(sha, beforeContent);
    }
    if (!backupStored) {
        return false;
    }
    const int  baselineTurn   = latestTurn();
    const auto baselineRecord = [&](const Epoch &recordEpoch) {
        if (!beforeExists) {
            return FileRecord::absent(recordEpoch, baselineTurn);
        }
        return FileRecord::present(sha, recordEpoch, baselineTurn);
    };

    /* The pre-mutation state belongs to the latest completed turn. This makes
     * a file first touched later explicit without claiming anything about
     * older turns. */
    QList<Snapshot> snapshots = loadSnapshots();
    Snapshot       *baseline  = nullptr;
    for (Snapshot &snap : snapshots) {
        if (snap.turn == baselineTurn && snap.epoch == epoch) {
            baseline = &snap;
            break;
        }
    }
    if (baseline == nullptr) {
        Snapshot fresh;
        fresh.turn      = baselineTurn;
        fresh.timestamp = QDateTime::currentDateTimeUtc();
        fresh.epoch     = epoch;
        fresh.files.insert(filePath, baselineRecord(fresh.epoch));
        snapshots.append(fresh);
    } else if (!baseline->files.contains(filePath)) {
        baseline->files.insert(filePath, baselineRecord(baseline->epoch));
    }
    trimSnapshots(snapshots);
    if (!saveSnapshots(snapshots)) {
        return false;
    }
    gcOrphanedBackups();
    return true;
}

bool QSocFileHistory::makeSnapshot(int turn)
{
    if (turn <= 0) {
        return false; /* turn 0 is reserved for pre-first-turn state */
    }
    (void) loadSnapshots();
    if (indexState == IndexState::Invalid) {
        return false;
    }
    const Epoch entryEpoch = liveEpoch();
    if (entryEpoch.tree.isEmpty() || entryEpoch.link.isEmpty()) {
        return false;
    }
    const QSet<QString> paths = trackedPathsFor(entryEpoch, turn);
    Snapshot            snap;
    snap.turn      = turn;
    snap.timestamp = QDateTime::currentDateTimeUtc();
    snap.epoch     = entryEpoch;
    bool straddled = false;
    bool complete  = true;
    for (const QString &path : paths) {
        /* Once the transport has been replaced, the rest of the tree belongs
         * to a tree this snapshot is not describing, so claim nothing about
         * it rather than reading it on the new link. */
        if (straddled) {
            snap.files
                .insert(path, FileRecord::unknown(entryEpoch, introducedTurnFor(entryEpoch, path)));
            complete = false;
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
            snap.files
                .insert(path, FileRecord::unknown(entryEpoch, introducedTurnFor(entryEpoch, path)));
            complete = false;
            continue;
        }
        switch (live.state()) {
        case FileState::Unknown:
            /* Recorded as unknown, never as absent: absent is an instruction
             * to unlink, and we did not establish that there is nothing
             * there to unlink. */
            snap.files
                .insert(path, FileRecord::unknown(entryEpoch, introducedTurnFor(entryEpoch, path)));
            complete = false;
            break;
        case FileState::Absent:
            snap.files
                .insert(path, FileRecord::absent(entryEpoch, introducedTurnFor(entryEpoch, path)));
            break;
        case FileState::Present: {
            const QString sha = sha256Hex(live.content());
            if (writeBackup(sha, live.content())) {
                snap.files.insert(
                    path, FileRecord::present(sha, entryEpoch, introducedTurnFor(entryEpoch, path)));
            } else {
                snap.files.insert(
                    path, FileRecord::unknown(entryEpoch, introducedTurnFor(entryEpoch, path)));
                complete = false;
            }
            break;
        }
        }
    }

    /* Append in-memory, trim to MAX_SNAPSHOTS, then rewrite snapshots.jsonl
     * atomically via saveSnapshots. */
    QList<Snapshot> snapshots = loadSnapshots();
    snapshots.append(snap);
    trimSnapshots(snapshots);
    if (!saveSnapshots(snapshots)) {
        return false;
    }
    gcOrphanedBackups();
    return complete;
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

QMap<QString, QSocFileHistory::FileRecord> QSocFileHistory::restoreStateAt(
    int turn, const Epoch &live, bool *targetExists, bool *targetComplete) const
{
    QMap<QString, FileRecord> state;
    QList<Snapshot>           candidates;
    const auto                snapshots = loadSnapshots();
    QSet<QString>             required;
    for (const Snapshot &snap : snapshots) {
        if (live.tree.isEmpty() || snap.epoch.tree != live.tree) {
            continue;
        }
        for (auto it = snap.files.cbegin(); it != snap.files.cend(); ++it) {
            if (it.value().introducedTurn() <= turn) {
                required.insert(it.key());
            }
        }
        if (snap.turn == turn && snap.epoch != live) {
            candidates.append(snap);
        }
    }
    for (const Snapshot &snap : snapshots) {
        if (snap.turn == turn && snap.epoch == live) {
            candidates.append(snap);
        }
    }
    for (const Snapshot &snap : candidates) {
        for (auto it = snap.files.cbegin(); it != snap.files.cend(); ++it) {
            state.insert(it.key(), it.value());
        }
    }
    bool complete = true;
    for (const QString &path : required) {
        if (!state.contains(path)) {
            state.insert(path, FileRecord::unknown(live));
            complete = false;
        }
    }
    if (targetExists != nullptr) {
        *targetExists = !candidates.isEmpty();
    }
    if (targetComplete != nullptr) {
        *targetComplete = complete;
    }
    return state;
}

QSocFileHistory::BoundaryPreview QSocFileHistory::previewBoundary(int turn) const
{
    BoundaryPreview preview;
    const Epoch     live  = liveEpoch();
    const auto      state = restoreStateAt(turn, live, nullptr);
    if (indexState == IndexState::Invalid) {
        return preview;
    }
    QSet<QString> elsewhere;
    for (const Snapshot &snap : loadSnapshots()) {
        if (snap.turn > turn) {
            continue;
        }
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            if (state.contains(it.key())) {
                continue;
            }
            switch (relate(it.value().epoch(), live)) {
            case EpochRelation::OtherTree:
                elsewhere.insert(it.key());
                break;
            case EpochRelation::OtherLink:
                preview.otherLink.append(it.key());
                break;
            case EpochRelation::Same:
                break;
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

QString QSocFileHistory::restoreRefusal(int turn) const
{
    bool targetExists   = false;
    bool targetComplete = false;
    (void) loadSnapshots();
    if (indexState == IndexState::Invalid) {
        return QStringLiteral("the file checkpoint index is unreadable or incomplete");
    }
    const Epoch live = liveEpoch();
    if (live.tree.isEmpty() || live.link.isEmpty()) {
        return QStringLiteral("the working tree identity cannot be established");
    }
    const auto state = restoreStateAt(turn, live, &targetExists, &targetComplete);
    if (!targetExists) {
        return QStringLiteral("the requested file checkpoint is unavailable for this workspace");
    }
    if (!targetComplete) {
        return QStringLiteral("the requested file checkpoint is incomplete");
    }
    for (auto it = state.cbegin(); it != state.cend(); ++it) {
        if (!it.value().isUnknown() && !coversPath(it.key())) {
            return QStringLiteral("the working tree no longer covers a required checkpoint path");
        }
        if (it.value().isPresent() && !readBackup(it.value().sha256()).has_value()) {
            return QStringLiteral("a backup required by the file checkpoint is unavailable");
        }
    }
    return {};
}

QSocFileHistory::RestoreReport QSocFileHistory::applySnapshot(int turn, AcrossGeneration across)
{
    RestoreReport report;
    const Epoch   entryEpoch   = liveEpoch();
    bool          targetExists = false;
    const auto    state        = restoreStateAt(turn, entryEpoch, &targetExists);
    if (indexState == IndexState::Invalid || !targetExists) {
        report.targetMissing = true;
        report.unknown       = state.keys();
        std::sort(report.unknown.begin(), report.unknown.end());
        return report;
    }
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
            if (liveEpoch() != entryEpoch) {
                report.transportChanged = true;
                report.unknown.append(path);
                stopped = true;
                continue;
            }
            switch (liveState) {
            case FileState::Unknown:
                report.unknown.append(path);
                break;
            case FileState::Absent:
                break; /* already matches the target turn */
            case FileState::Present:
                if (liveAccessor.remove) {
                    const bool removed = liveAccessor.remove(path);
                    if (liveEpoch() != entryEpoch) {
                        report.transportChanged = true;
                        report.failed.append(path);
                        stopped = true;
                        break;
                    }
                    if (removed) {
                        report.restored.append(path);
                    } else {
                        report.failed.append(path);
                    }
                } else {
                    report.failed.append(path);
                }
                break;
            }
            continue;
        }
        const auto content = readBackup(rec.sha256());
        if (!content.has_value()) {
            /* Leaving the file alone beats corrupting it, but the caller
             * must not be told the tree is back where it was. */
            report.failed.append(path);
            continue;
        }
        if (liveAccessor.write) {
            const bool written = liveAccessor.write(path, *content);
            if (liveEpoch() != entryEpoch) {
                report.transportChanged = true;
                report.failed.append(path);
                stopped = true;
            } else if (written) {
                report.restored.append(path);
            } else {
                report.failed.append(path);
            }
        } else {
            report.failed.append(path);
        }
    }
    std::sort(report.restored.begin(), report.restored.end());
    std::sort(report.failed.begin(), report.failed.end());
    std::sort(report.unknown.begin(), report.unknown.end());
    return report;
}

bool QSocFileHistory::truncateAfter(int cutoffTurn)
{
    QList<Snapshot> snapshots = loadSnapshots();
    if (indexState == IndexState::Invalid) {
        return false;
    }
    QList<Snapshot> kept;
    kept.reserve(snapshots.size());
    for (const Snapshot &snap : snapshots) {
        if (snap.turn <= cutoffTurn) {
            kept.append(snap);
        }
    }
    if (kept.size() != snapshots.size()) {
        if (!saveSnapshots(kept)) {
            return false;
        }
        gcOrphanedBackups();
    }
    return true;
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
    return readBackup(rec.sha256()).value_or(QString());
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
    const auto snapshots = loadSnapshots();
    return indexState == IndexState::Valid && snapshots.isEmpty();
}

bool QSocFileHistory::storageIsBound() const
{
    if (!localRootIsBound(storageLexicalRootValue, storageCanonicalRootValue, storageTreeIdValue)) {
        return false;
    }
    const QString history = QStringLiteral(".qsoc/file-history/%1/backups").arg(sessionIdValue);
    const QString index   = snapshotsPath();
    return boundMetadataDirectory(storageCanonicalRootValue, QStringLiteral(".qsoc/sessions"))
           && boundMetadataDirectory(storageCanonicalRootValue, history)
           && !QFileInfo(index).isSymLink();
}

QList<QSocFileHistory::Snapshot> QSocFileHistory::loadSnapshots() const
{
    if (!storageIsBound()) {
        cachedSnapshots.clear();
        cacheValid = true;
        indexState = IndexState::Invalid;
        return {};
    }
    if (cacheValid) {
        return cachedSnapshots;
    }
    QList<Snapshot> result;
    QFile           file(snapshotsPath());
    if (!file.exists()) {
        cachedSnapshots = result;
        cacheValid      = true;
        indexState      = IndexState::Valid;
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        cachedSnapshots.clear();
        cacheValid = true;
        indexState = IndexState::Invalid;
        return {};
    }
    const QByteArray contents = file.readAll();
    const bool       readOk   = file.error() == QFileDevice::NoError;
    file.close();
    if (!readOk) {
        cachedSnapshots.clear();
        cacheValid = true;
        indexState = IndexState::Invalid;
        return {};
    }
    for (const QByteArray &rawLine : contents.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        try {
            const json doc = json::parse(line.constData(), line.constData() + line.size());
            if (!doc.is_object() || !doc.contains("turn") || !doc.contains("files")
                || !doc["turn"].is_number_integer() || doc["turn"].get<int>() < 0
                || !doc["files"].is_object()) {
                throw std::runtime_error("invalid snapshot record");
            }
            bool trusted = false;
            if (doc.contains("seal")) {
                if (!doc["seal"].is_string()) {
                    throw std::runtime_error("invalid snapshot seal");
                }
                const QString expected = QString::fromStdString(doc["seal"].get<std::string>());
                json          payload  = doc;
                payload.erase("seal");
                const std::string serialized = payload.dump();
                if (!isSha256Hex(expected)
                    || sha256Bytes(QByteArray::fromStdString(serialized)) != expected) {
                    throw std::runtime_error("snapshot seal mismatch");
                }
                trusted = true;
            }
            Snapshot snap;
            snap.turn = doc["turn"].get<int>();
            if (doc.contains("ts")) {
                if (!doc["ts"].is_string()) {
                    throw std::runtime_error("invalid snapshot timestamp");
                }
                snap.timestamp = QDateTime::fromString(
                    QString::fromStdString(doc["ts"].get<std::string>()), Qt::ISODateWithMs);
                if (!snap.timestamp.isValid()) {
                    throw std::runtime_error("invalid snapshot timestamp");
                }
            }
            if (trusted && doc.contains("tree")) {
                if (!doc["tree"].is_string()) {
                    throw std::runtime_error("invalid snapshot tree");
                }
                snap.epoch.tree = QString::fromStdString(doc["tree"].get<std::string>());
            }
            if (trusted && doc.contains("link")) {
                if (!doc["link"].is_string()) {
                    throw std::runtime_error("invalid snapshot link");
                }
                snap.epoch.link = QString::fromStdString(doc["link"].get<std::string>());
            }
            const auto &files = doc["files"];
            for (auto it = files.begin(); it != files.end(); ++it) {
                const QString path = QString::fromStdString(it.key());
                if (path.isEmpty()) {
                    throw std::runtime_error("empty snapshot path");
                }
                if (!trusted) {
                    snap.files.insert(path, FileRecord::unknown(Epoch()));
                    continue;
                }
                if (!it.value().is_object() || !it.value().contains("since")
                    || !it.value().contains("state") || !it.value()["since"].is_number_integer()) {
                    throw std::runtime_error("invalid snapshot file record");
                }
                const int since = it.value()["since"].get<int>();
                if (since < 0 || since > snap.turn) {
                    throw std::runtime_error("invalid snapshot provenance");
                }
                const auto &state = it.value()["state"];
                FileRecord  rec   = FileRecord::unknown(snap.epoch, since);
                if (state.is_null()) {
                    rec = FileRecord::absent(snap.epoch, since);
                } else if (state.is_string()) {
                    const QString value = QString::fromStdString(state.get<std::string>());
                    if (isSha256Hex(value)) {
                        rec = FileRecord::present(value, snap.epoch, since);
                    } else if (value != QStringLiteral("unknown")) {
                        throw std::runtime_error("invalid snapshot state");
                    }
                } else {
                    throw std::runtime_error("invalid snapshot state");
                }
                snap.files.insert(path, rec);
            }
            result.append(snap);
        } catch (const std::exception &) {
            cachedSnapshots.clear();
            cacheValid = true;
            indexState = IndexState::Invalid;
            return {};
        }
    }
    /* Stable: two snapshots can share a turn (one baseline per tree, plus a
     * legacy baseline that names none), and flattening must take the one
     * written later, not whichever the sort happened to leave last. */
    std::stable_sort(result.begin(), result.end(), [](const Snapshot &lhs, const Snapshot &rhs) {
        return lhs.turn < rhs.turn;
    });
    const auto provenanceKey = [](const Epoch &epoch, const QString &path) {
        return QString::number(epoch.tree.size()) + QLatin1Char(':') + epoch.tree
               + QString::number(epoch.link.size()) + QLatin1Char(':') + epoch.link + path;
    };
    QHash<QString, int> introduced;
    QSet<QString>       baselines;
    for (const Snapshot &snap : result) {
        if (snap.epoch.tree.isEmpty() || snap.epoch.link.isEmpty()) {
            continue;
        }
        for (auto it = snap.files.cbegin(); it != snap.files.cend(); ++it) {
            const QString key   = provenanceKey(snap.epoch, it.key());
            const int     since = it.value().introducedTurn();
            if (introduced.contains(key) && introduced.value(key) != since) {
                cachedSnapshots.clear();
                cacheValid = true;
                indexState = IndexState::Invalid;
                return {};
            }
            introduced.insert(key, since);
            if (snap.turn == since && !it.value().isUnknown()) {
                baselines.insert(key);
            }
        }
    }
    for (auto it = introduced.cbegin(); it != introduced.cend(); ++it) {
        if (!baselines.contains(it.key())) {
            cachedSnapshots.clear();
            cacheValid = true;
            indexState = IndexState::Invalid;
            return {};
        }
    }
    cachedSnapshots = result;
    cacheValid      = true;
    indexState      = IndexState::Valid;
    return result;
}

bool QSocFileHistory::saveSnapshots(const QList<Snapshot> &snapshots) const
{
    if (!storageIsBound()) {
        return false;
    }
    if (indexState == IndexState::Unknown) {
        (void) loadSnapshots();
    }
    if (indexState == IndexState::Invalid) {
        return false;
    }
    if (!ensureDirs()) {
        return false;
    }
    const QString path = snapshotsPath();
    QSaveFile     file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
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
        if (!snap.epoch.link.isEmpty()) {
            doc["link"] = snap.epoch.link.toStdString();
        }
        json filesObj = json::object();
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            json record;
            record["since"] = it.value().introducedTurn();
            if (it.value().isPresent()) {
                record["state"] = it.value().sha256().toStdString();
            } else if (it.value().isAbsent()) {
                record["state"] = nullptr;
            } else {
                record["state"] = "unknown";
            }
            filesObj[it.key().toStdString()] = std::move(record);
        }
        doc["files"]              = filesObj;
        const std::string payload = doc.dump();
        doc["seal"]               = sha256Bytes(QByteArray::fromStdString(payload)).toStdString();
        const std::string serialized = doc.dump();
        if (file.write(serialized.data(), static_cast<qint64>(serialized.size()))
                != static_cast<qint64>(serialized.size())
            || file.write("\n", 1) != 1) {
            file.cancelWriting();
            return false;
        }
    }
    if (!file.commit()) {
        return false;
    }
    cachedSnapshots = snapshots;
    cacheValid      = true;
    indexState      = IndexState::Valid;
    return true;
}

void QSocFileHistory::gcOrphanedBackups() const
{
    if (!storageIsBound()) {
        return;
    }
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

    const QString backupsDir = QDir(historyDir(storageCanonicalRootValue, sessionIdValue))
                                   .filePath(QStringLiteral("backups"));
    QDir          dir(backupsDir);
    if (!dir.exists()) {
        return;
    }
    const auto entries = dir.entryInfoList({QStringLiteral("*.bak")}, QDir::Files);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        const QString sha = entry.completeBaseName();
        if (!referenced.contains(sha) && storageIsBound()) {
            QFile::remove(entry.absoluteFilePath());
        }
    }
}
