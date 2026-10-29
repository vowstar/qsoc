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
 * @brief Snapshot hashes and complete-read qualification for file tools.
 * @details State stays in memory and resets with the session or workspace.
 */
class QSocFileReadState
{
public:
    static QString sha(const QByteArray &content)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    }

    void recordSnapshot(const QString &path, const QString &hash, bool complete)
    {
        const auto previous = entries.constFind(path);
        complete            = complete
                              || (previous != entries.constEnd() && previous.value().sha == hash
                                  && previous.value().complete);
        entries.insert(path, {.sha = hash, .seq = nextSeq++, .complete = complete});
    }

    void recordRead(const QString &path, const QString &content, bool complete = true)
    {
        recordSnapshot(path, sha(content.toUtf8()), complete);
    }

    bool wasRead(const QString &path) const { return entries.contains(path); }

    bool wasFullyRead(const QString &path) const
    {
        const auto iter = entries.constFind(path);
        return iter != entries.constEnd() && iter.value().complete;
    }

    /** @brief True when @p path was read but its content differs now. */
    bool changedSinceRead(const QString &path, const QByteArray &currentContent) const
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
        quint64 seq      = 0;
        bool    complete = false;
    };
    QHash<QString, Entry> entries;
    quint64               nextSeq = 1;
};

#endif // QSOCFILEREADSTATE_H
