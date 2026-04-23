// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEPATHCONTEXT_H
#define QSOCREMOTEPATHCONTEXT_H

#include <QString>
#include <QStringList>

/**
 * @brief Pure-logic container for remote workspace paths.
 * @details Normalizes remote paths lexically without ever touching the local
 *          filesystem via QFileInfo. Remote paths are POSIX absolute or
 *          resolved against the current remote working directory.
 *
 *          Writable-directory checks operate on already-normalized paths; the
 *          caller is responsible for running SFTP realpath/stat separately to
 *          validate actual remote-side canonicalization before writing.
 */
class QSocRemotePathContext
{
public:
    QSocRemotePathContext() = default;
    QSocRemotePathContext(QString root, QString cwd, QStringList writableDirs);

    QString     root() const { return m_root; }
    QString     cwd() const { return m_cwd; }
    QStringList writableDirs() const { return m_writableDirs; }

    void setRoot(const QString &root);
    void setCwd(const QString &cwd);
    void setWritableDirs(const QStringList &dirs);

    /**
     * @brief Lexically normalize a remote path.
     * @details Handles POSIX `//`, `.`, and `..` segments. Empty input
     *          resolves to cwd (falling back to root). Relative paths
     *          resolve against cwd. Absolute paths pass through the same
     *          lexical cleanup. Never consults the local filesystem.
     */
    QString normalize(const QString &path) const;

    /**
     * @brief True when a normalized absolute path is inside any writable dir.
     * @details Matching is byte-prefix with a trailing-slash guard so
     *          `/foo/barabc` does not match writable root `/foo/bar`.
     */
    bool isWritable(const QString &normalizedPath) const;

    /**
     * @brief Resolve a user-supplied relative reference intended to set cwd.
     * @details Unlike @ref normalize, this always returns an absolute path
     *          under root, rejecting `..` escapes above root.
     */
    QString resolveCwdRequest(const QString &requested) const;

private:
    static QStringList splitPosix(const QString &path);
    static QString     joinPosix(const QStringList &parts, bool absolute);
    static QString     lexicalNormalize(const QString &path);

    QString     m_root;
    QString     m_cwd;
    QStringList m_writableDirs;
};

#endif // QSOCREMOTEPATHCONTEXT_H
