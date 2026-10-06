// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEPATHCONTEXT_H
#define QSOCREMOTEPATHCONTEXT_H

#include "agent/qsocfilereadstate.h"

#include <QString>
#include <QStringList>

#include <functional>

/**
 * @brief The one home of the remote workspace paths, and their publisher.
 * @details Normalizes remote paths lexically without ever touching the local
 *          filesystem via QFileInfo. Remote paths are POSIX absolute or
 *          resolved against the current remote working directory.
 *
 *          The working directory is read from here by the tools and copied
 *          elsewhere by whoever describes the session, so @ref setCwd is both
 *          the only mutator and the publisher: see @ref setCwdObserver.
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

    /**
     * @brief The only mutator of the working directory, and it publishes.
     * @details Every copy of the working directory (the agent config, and
     *          through it the system prompt and the hook envelope) is written
     *          from here, so no caller can move the directory without the
     *          copies following.
     */
    void setCwd(const QString &cwd);

    /** @brief Install the observer @ref setCwd publishes to. Replaces any. */
    void setCwdObserver(std::function<void(const QString &)> observer);

    void setWritableDirs(const QStringList &dirs);

    /**
     * @brief How the bound host spells paths.
     * @details On a Windows host @ref normalize maps `C:\x`, `C:/x`, the Git
     *          Bash `/c/x` and the executor's spelling of the workspace root
     *          (@p shellRoot, standing for @p sftpRoot) to the SFTP form
     *          `/C:/x`, and containment ignores case.
     */
    void setHostStyle(bool windows, const QString &sftpRoot = {}, const QString &shellRoot = {});

    /** @brief The host's home directory, which a leading `~` names. */
    void    setHome(const QString &home);
    QString home() const { return m_home; }

    /** @brief Case rule for comparing paths on the bound host. */
    Qt::CaseSensitivity pathCase() const
    {
        return m_windows ? Qt::CaseInsensitive : Qt::CaseSensitive;
    }

    /** @brief Whether @p path starts with `~` naming the home directory. */
    static bool namesHome(const QString &path);

    /**
     * @brief The SFTP spelling of a path on a Windows host.
     * @details Pure. Backslashes become `/`; a path under @p shellRoot maps to
     *          @p sftpRoot; `C:\x`, `C:/x`, `/c/x` and `/C:/x` become `/C:/x`.
     *          Anything else is returned with only its separators changed.
     */
    static QString windowsSftpPath(
        const QString &path, const QString &sftpRoot, const QString &shellRoot);

    /**
     * @brief Forget everything a transport taught us, keep the observer.
     * @details Assigning a default-constructed context instead drops the
     *          observer, and the working directory then moves with nobody
     *          listening.
     */
    void reset();

    /**
     * @brief Lexically normalize a remote path.
     * @details Handles POSIX `//`, `.`, and `..` segments. Empty input
     *          resolves to cwd (falling back to root). A leading `~` is the
     *          home set by @ref setHome. Relative paths resolve against cwd.
     *          Absolute paths pass through the same lexical cleanup. On a
     *          Windows host the Windows spellings map first, see
     *          @ref setHostStyle. Never consults the local filesystem.
     */
    QString normalize(const QString &raw) const;

    /**
     * @brief True when @p normalizedPath is inside one of @p normalizedDirs.
     * @details Matching is byte-prefix with a trailing-slash guard so
     *          `/foo/barabc` does not match writable root `/foo/bar`. Both
     *          arguments must be in the same spelling: comparing a
     *          host-canonical path against a lexical directory is how a
     *          workspace reached through a symlink refuses every write in it.
     */
    static bool isWithinAny(
        const QString      &normalizedPath,
        const QStringList  &normalizedDirs,
        Qt::CaseSensitivity cs = Qt::CaseSensitive);

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
    QString            hostSpelling(const QString &path) const;

    QString                              m_root;
    QString                              m_cwd;
    QStringList                          m_writableDirs;
    QString                              m_home;
    QString                              m_sftpRoot;
    QString                              m_shellRoot;
    bool                                 m_windows = false;
    QSocFileReadState                    m_readState;
    std::function<void(const QString &)> m_cwdObserver;
};

#endif // QSOCREMOTEPATHCONTEXT_H
