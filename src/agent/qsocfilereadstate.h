// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFILEREADSTATE_H
#define QSOCFILEREADSTATE_H

#include <QCryptographicHash>
#include <QHash>
#include <QList>
#include <QString>

#include <algorithm>

/**
 * @brief In-memory record of what the agent has read, for read-before-edit.
 * @details Maps an absolute path to the SHA-256 of the content seen at the
 *          last full read (or last write). The file tools consult it to
 *          (1) require a file be read before it is edited or overwritten and
 *          (2) reject an edit when the on-disk content changed since that
 *          read, preventing blind / lost-update writes.
 *
 *          State is per-process and not persisted: a resumed session starts
 *          empty, so the agent must re-read before editing, which is correct
 *          because the file may have changed while the session was closed.
 *          Comparison is content-based (SHA), so it is immune to filesystem
 *          mtime granularity and works identically for local and remote
 *          files. Callers pass content; the edit/write tools already read it.
 */
class QSocFileReadState
{
public:
    static QString sha(const QString &content)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    /** @brief Record that @p path was read with the given full content. */
    void recordRead(const QString &path, const QString &content)
    {
        entries.insert(path, {.sha = sha(content), .seq = nextSeq++});
    }

    /** @brief True once @p path has been read (or written) this process. */
    bool wasRead(const QString &path) const { return entries.contains(path); }

    /** @brief True when @p path was read but its content differs now. */
    bool changedSinceRead(const QString &path, const QString &currentContent) const
    {
        const auto iter = entries.constFind(path);
        return iter != entries.constEnd() && iter.value().sha != sha(currentContent);
    }

    /** @brief Forget all reads (on /clear or project switch). */
    void clear()
    {
        entries.clear();
        nextSeq = 1;
    }

    /**
     * @brief Paths read this process, most-recent first, capped at @p max.
     * @details Recency is the read sequence (a re-read bumps the path to the
     *          front). Used to pick the candidate files to re-supply after a
     *          context compaction. @p max <= 0 returns all paths.
     */
    QList<QString> pathsByRecencyDesc(int max) const
    {
        QList<QString> paths = entries.keys();
        std::sort(paths.begin(), paths.end(), [this](const QString &lhs, const QString &rhs) {
            return entries.value(lhs).seq > entries.value(rhs).seq;
        });
        if (max > 0 && paths.size() > max) {
            paths = paths.mid(0, max);
        }
        return paths;
    }

private:
    struct Entry
    {
        QString sha;
        quint64 seq = 0;
    };
    QHash<QString, Entry> entries;
    quint64               nextSeq = 1;
};

#endif // QSOCFILEREADSTATE_H
