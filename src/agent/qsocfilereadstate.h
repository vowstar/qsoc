// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFILEREADSTATE_H
#define QSOCFILEREADSTATE_H

#include <QCryptographicHash>
#include <QHash>
#include <QString>

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
        shas.insert(path, sha(content));
    }

    /** @brief True once @p path has been read (or written) this process. */
    bool wasRead(const QString &path) const { return shas.contains(path); }

    /** @brief True when @p path was read but its content differs now. */
    bool changedSinceRead(const QString &path, const QString &currentContent) const
    {
        const auto it = shas.constFind(path);
        return it != shas.constEnd() && it.value() != sha(currentContent);
    }

    /** @brief Forget all reads (on /clear or project switch). */
    void clear() { shas.clear(); }

private:
    QHash<QString, QString> shas;
};

#endif // QSOCFILEREADSTATE_H
