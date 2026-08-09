// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEPATHCONTEXT_H
#define QSOCREMOTEPATHCONTEXT_H

#include "agent/qsocfilereadstate.h"

#include <QString>
#include <QStringList>

/**
 * @brief Pure-logic container for remote workspace paths.
 * @details Normalizes remote paths lexically without ever touching the local
 *          filesystem via QFileInfo. Remote paths are POSIX absolute or
 *          resolved against the current remote working directory.
 *
 *          Writable-directory checks are byte-prefix comparisons on
 *          already-normalized paths, so no check here can see a symlink. A
 *          caller about to write must first canonicalize on the host with
 *          `QSocSftpClient::canonicalize` and compare the canonical path
 *          against canonicalized writable directories through
 *          @ref isWithinAny; @ref isWritable is the lexical-only shorthand and
 *          is not a containment guarantee on its own.
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
     * @brief True when @p normalizedPath is inside one of @p normalizedDirs.
     * @details Matching is byte-prefix with a trailing-slash guard so
     *          `/foo/barabc` does not match writable root `/foo/bar`. Both
     *          arguments must be in the same spelling: comparing a
     *          host-canonical path against a lexical directory is how a
     *          workspace reached through a symlink refuses every write in it.
     */
    static bool isWithinAny(const QString &normalizedPath, const QStringList &normalizedDirs);

    /**
     * @brief @ref isWithinAny against the configured writable dirs.
     * @details Lexical only. See the class note before using it as a guard.
     */
    bool isWritable(const QString &normalizedPath) const;

    /**
     * @brief Resolve a user-supplied relative reference intended to set cwd.
     * @details Unlike @ref normalize, this always returns an absolute path
     *          under root, rejecting `..` escapes above root.
     */
    QString resolveCwdRequest(const QString &requested) const;

    /* Shared read-before-edit state for the remote file tools, keyed by
     * normalized remote path. Mirrors the local path context. */
    QSocFileReadState &readState() { return m_readState; }

private:
    static QStringList splitPosix(const QString &path);
    static QString     joinPosix(const QStringList &parts, bool absolute);
    static QString     lexicalNormalize(const QString &path);

    QString           m_root;
    QString           m_cwd;
    QStringList       m_writableDirs;
    QSocFileReadState m_readState;
};

#endif // QSOCREMOTEPATHCONTEXT_H
