// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagentcompletion.h"

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"

#include <algorithm>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

QAgentCompletionEngine::QAgentCompletionEngine()
{
    /* bus/ module/ schematic/ are user-authored YAML/XML content in QSoC
     * projects — they must remain completable. Only ignore caches and
     * generated artifacts here. */
    ignoreDirs = {
        QStringLiteral(".git"),
        QStringLiteral(".qsoc"),
        QStringLiteral("build"),
        QStringLiteral("output"),
        QStringLiteral("node_modules"),
        QStringLiteral(".cache"),
        QStringLiteral("__pycache__"),
    };
}

bool QAgentCompletionEngine::shouldRescan(const QString &projectPath) const
{
    if (!cacheValid || projectPath != cachedProjectPath) {
        return true;
    }
    if (!cacheTimer.isValid()) {
        return true;
    }
    return cacheTimer.elapsed() > cacheTtlMs;
}

void QAgentCompletionEngine::invalidateCache()
{
    cacheValid = false;
    cachedFiles.clear();
}

void QAgentCompletionEngine::scan(const QString &projectPath)
{
    cachedFiles.clear();
    cachedProjectPath = projectPath;

    QDir rootDir(projectPath);
    if (!rootDir.exists()) {
        cacheValid = false;
        return;
    }

    /* Walk the tree manually so we can prune ignored directories without
     * descending into them (QDirIterator has no prune hook). */
    QStringList stack;
    stack.append(projectPath);

    int scanned = 0;
    while (!stack.isEmpty() && scanned < DEFAULT_SCAN_LIMIT) {
        QString dirPath = stack.takeLast();
        QDir    dir(dirPath);
        if (!dir.exists()) {
            continue;
        }

        const auto entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo &entry : entries) {
            if (scanned >= DEFAULT_SCAN_LIMIT) {
                break;
            }
            const QString name = entry.fileName();
            if (entry.isDir()) {
                if (ignoreDirs.contains(name) || name.startsWith(QLatin1Char('.'))) {
                    continue;
                }
                stack.append(entry.absoluteFilePath());
                continue;
            }
            QString rel = rootDir.relativeFilePath(entry.absoluteFilePath());
            cachedFiles.append(rel);
            scanned++;
        }
    }

    std::sort(cachedFiles.begin(), cachedFiles.end());
    cacheValid = true;
    cacheTimer.restart();
}

void QAgentCompletionEngine::scanRemote(QSocSshSession *session, const QString &remoteRoot)
{
    cachedFiles.clear();
    cachedProjectPath = QStringLiteral("remote:") + remoteRoot;
    if (session == nullptr || remoteRoot.isEmpty()) {
        cacheValid = false;
        return;
    }

    auto shellEscape = [](const QString &value) {
        QString out = QStringLiteral("'");
        for (const QChar chr : value) {
            if (chr == QLatin1Char('\'')) {
                out += QStringLiteral("'\\''");
            } else {
                out += chr;
            }
        }
        out += QLatin1Char('\'');
        return out;
    };

    QString pruneExpr;
    for (const QString &dir : ignoreDirs) {
        if (!pruneExpr.isEmpty()) {
            pruneExpr += QStringLiteral(" -o ");
        }
        pruneExpr += QStringLiteral("-name ") + shellEscape(dir);
    }
    if (pruneExpr.isEmpty()) {
        pruneExpr = QStringLiteral("-false");
    }

    const QString cmd = QStringLiteral(
                            "find %1 -type d \\( %2 \\) -prune -o -type f -print 2>/dev/null "
                            "| head -n %3")
                            .arg(shellEscape(remoteRoot))
                            .arg(pruneExpr)
                            .arg(DEFAULT_SCAN_LIMIT);

    QSocSshExec         exec(*session);
    QSocSshExec::Result res = exec.run(cmd, 15000);
    if (res.stdoutBytes.isEmpty()) {
        cacheValid = false;
        return;
    }

    QString prefix = remoteRoot;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }

    const QList<QByteArray> lines = res.stdoutBytes.split('\n');
    cachedFiles.reserve(lines.size());
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) {
            continue;
        }
        QString path = QString::fromUtf8(line);
        if (path.startsWith(prefix)) {
            path = path.mid(prefix.size());
        } else if (path == remoteRoot) {
            continue;
        }
        if (!path.isEmpty()) {
            cachedFiles.append(path);
        }
    }

    std::sort(cachedFiles.begin(), cachedFiles.end());
    cacheValid = true;
    cacheTimer.restart();
}

int QAgentCompletionEngine::fuzzyScore(const QString &path, const QString &query)
{
    if (query.isEmpty()) {
        return 0;
    }

    const QString lowerPath  = path.toLower();
    const QString lowerQuery = query.toLower();

    /* Tier 1: exact basename match → highest */
    QString basename = path.section(QLatin1Char('/'), -1);
    if (basename.compare(query, Qt::CaseInsensitive) == 0) {
        return 10000;
    }

    /* Tier 2: basename starts with query */
    if (basename.startsWith(query, Qt::CaseInsensitive)) {
        return 5000 - basename.size();
    }

    /* Tier 3: contiguous substring anywhere in path */
    int idx = lowerPath.indexOf(lowerQuery);
    if (idx >= 0) {
        /* Earlier match and shorter path scores higher */
        return 2000 - idx - path.size();
    }

    /* Tier 4: ordered-character subsequence match */
    int queryIdx = 0;
    int matches  = 0;
    for (int pathIdx = 0; pathIdx < lowerPath.size() && queryIdx < lowerQuery.size(); pathIdx++) {
        if (lowerPath[pathIdx] == lowerQuery[queryIdx]) {
            matches++;
            queryIdx++;
        }
    }
    if (queryIdx == lowerQuery.size()) {
        return 500 + matches - (path.size() / 10);
    }

    return -1;
}

QStringList QAgentCompletionEngine::complete(
    const QString &projectPath, const QString &query, int maxResults)
{
    if (shouldRescan(projectPath)) {
        scan(projectPath);
    }
    return rank(query, maxResults);
}

QStringList QAgentCompletionEngine::completeRemote(
    QSocSshSession *session, const QString &remoteRoot, const QString &query, int maxResults)
{
    const QString key = QStringLiteral("remote:") + remoteRoot;
    if (shouldRescan(key)) {
        scanRemote(session, remoteRoot);
    }
    return rank(query, maxResults);
}

QStringList QAgentCompletionEngine::rank(const QString &query, int maxResults) const
{
    QList<QPair<int, QString>> scored;
    scored.reserve(cachedFiles.size());
    for (const QString &path : cachedFiles) {
        int score = fuzzyScore(path, query);
        if (score >= 0) {
            scored.append({score, path});
        }
    }

    std::sort(scored.begin(), scored.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });

    QStringList result;
    int         limit = qMin(maxResults, static_cast<int>(scored.size()));
    result.reserve(limit);
    for (int i = 0; i < limit; i++) {
        result.append(scored[i].second);
    }
    return result;
}
