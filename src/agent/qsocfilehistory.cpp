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

} // namespace

QSocFileHistory::QSocFileHistory(QString projectPath, QString sessionId)
    : projectPathValue(std::move(projectPath))
    , sessionIdValue(std::move(sessionId))
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

    /* Write the baseline blob (empty sha for absent files). */
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
    if (baseline == nullptr) {
        Snapshot fresh;
        fresh.turn      = 0;
        fresh.timestamp = QDateTime::currentDateTimeUtc();
        fresh.files.insert(filePath, sha);
        snapshots.prepend(fresh);
    } else if (!baseline->files.contains(filePath)) {
        baseline->files.insert(filePath, sha);
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
    Snapshot snap;
    snap.turn      = turn;
    snap.timestamp = QDateTime::currentDateTimeUtc();
    for (const QString &path : trackedFiles) {
        QFileInfo info(path);
        if (!info.exists()) {
            /* File was deleted or was never created for this turn. */
            snap.files.insert(path, QString());
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            /* Unreadable at snapshot time — record as absent so rewind
             * won't try to restore a stale blob. */
            snap.files.insert(path, QString());
            continue;
        }
        const QByteArray utf8 = file.readAll();
        file.close();
        const QString content = QString::fromUtf8(utf8);
        const QString sha     = sha256Hex(content);
        writeBackup(sha, content);
        snap.files.insert(path, sha);
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

QMap<QString, QString> QSocFileHistory::effectiveStateAt(int turn) const
{
    QMap<QString, QString> state;
    const auto             snapshots = loadSnapshots();
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

QStringList QSocFileHistory::applySnapshot(int turn)
{
    QStringList touched;
    const auto  state = effectiveStateAt(turn);
    if (state.isEmpty()) {
        return touched;
    }
    for (auto it = state.begin(); it != state.end(); ++it) {
        const QString &path = it.key();
        const QString &sha  = it.value();
        if (sha.isEmpty()) {
            /* File was absent at the target turn — remove it if present. */
            QFile existing(path);
            if (existing.exists()) {
                existing.remove();
                touched.append(path);
            }
            continue;
        }
        const QString content = readBackup(sha);
        if (content.isNull()) {
            continue; /* backup missing; leave file alone rather than corrupt */
        }
        QFileInfo info(path);
        QDir      parent = info.absoluteDir();
        if (!parent.exists()) {
            parent.mkpath(QStringLiteral("."));
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            continue;
        }
        file.write(content.toUtf8());
        file.close();
        touched.append(path);
    }
    std::sort(touched.begin(), touched.end());
    return touched;
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
    const QString sha = state.value(filePath);
    if (sha.isEmpty()) {
        return QString();
    }
    return readBackup(sha);
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
            const auto &files = doc["files"];
            if (files.is_object()) {
                for (auto it = files.begin(); it != files.end(); ++it) {
                    const QString path = QString::fromStdString(it.key());
                    QString       sha;
                    if (it.value().is_string()) {
                        sha = QString::fromStdString(it.value().get<std::string>());
                    }
                    snap.files.insert(path, sha);
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
        json filesObj = json::object();
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            if (it.value().isEmpty()) {
                filesObj[it.key().toStdString()] = nullptr;
            } else {
                filesObj[it.key().toStdString()] = it.value().toStdString();
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

void QSocFileHistory::appendSnapshot(const Snapshot & /*snapshot*/) const
{
    /* Reserved for future optimisation: avoid full rewrite on the hot
     * append path. Current callers always go through saveSnapshots(). */
}

void QSocFileHistory::evictOldest()
{
    /* Currently folded into makeSnapshot's trim loop. */
}

void QSocFileHistory::gcOrphanedBackups() const
{
    /* Collect every sha256 still referenced by a surviving snapshot. */
    const auto    snapshots = loadSnapshots();
    QSet<QString> referenced;
    for (const Snapshot &snap : snapshots) {
        for (auto it = snap.files.begin(); it != snap.files.end(); ++it) {
            if (!it.value().isEmpty()) {
                referenced.insert(it.value());
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
