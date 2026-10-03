// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTCOMPLETION_H
#define QAGENTCOMPLETION_H

#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>

class QSocSshSession;

/**
 * @brief File path completion engine for '@file' references in agent input.
 * @details Scans a project directory with QDirIterator, ignoring common
 *          build/output directories, and returns fuzzy-matched relative
 *          paths for a query string. Results are sorted by a simple
 *          relevance score: exact substring matches beat out-of-order
 *          character matches. Scan results are cached for a short TTL to
 *          avoid re-walking the filesystem on every keystroke.
 */
class QAgentCompletionEngine
{
public:
    static constexpr int DEFAULT_CACHE_TTL_MS = 5000;
    static constexpr int DEFAULT_MAX_RESULTS  = 20;
    static constexpr int DEFAULT_SCAN_LIMIT   = 10000;

    QAgentCompletionEngine();

    /* Rescan project files, ignoring directories in the ignore set */
    void scan(const QString &projectPath);

    /* Rescan a remote workspace over SSH using `find`. Populates cache under
     * the "remote:" + remoteRoot key so it does not collide with local scans. */
    void scanRemote(QSocSshSession *session, const QString &remoteRoot);

    /* Return the current cached file list (relative paths) */
    const QStringList &allFiles() const { return cachedFiles; }

    /* Return up to maxResults paths matching the query, sorted by score */
    QStringList complete(
        const QString &projectPath, const QString &query, int maxResults = DEFAULT_MAX_RESULTS);

    /* Remote variant of complete(). Uses the "remote:" cache bucket. */
    QStringList completeRemote(
        QSocSshSession *session,
        const QString  &remoteRoot,
        const QString  &query,
        int             maxResults = DEFAULT_MAX_RESULTS);

    /* Compute fuzzy score: higher is better. Returns -1 when no match. */
    static int fuzzyScore(const QString &path, const QString &query);

    /* Override the ignore set for tests. Default: .git build .qsoc output bus module schematic */
    void setIgnoreDirs(const QSet<QString> &dirs) { ignoreDirs = dirs; }

    /* Override cache TTL for tests */
    void setCacheTtlMs(int ttlMs) { cacheTtlMs = ttlMs; }

    /* Force the next complete() call to rescan */
    void invalidateCache();

private:
    QStringList   cachedFiles;
    QString       cachedProjectPath;
    QElapsedTimer cacheTimer;
    bool          cacheValid = false;
    int           cacheTtlMs = DEFAULT_CACHE_TTL_MS;
    QSet<QString> ignoreDirs;

    bool        shouldRescan(const QString &projectPath) const;
    QStringList rank(const QString &query, int maxResults) const;
};

#endif // QAGENTCOMPLETION_H
