// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMEMORYMANAGER_H
#define QSOCMEMORYMANAGER_H

#include "common/qsocprojectmanager.h"

#include <QDateTime>
#include <QObject>
#include <QString>

/**
 * @brief Manages persistent memory across agent sessions
 * @details Provides a two-level memory system (user-global and project-local)
 *          with topic files, YAML frontmatter, and auto-indexed MEMORY.md.
 *
 *          Storage layout:
 *          - User:    ~/.config/qsoc/memory/MEMORY.md + topic files
 *          - Project: <project>/.qsoc/memory/MEMORY.md + topic files
 */
class QSocMemoryManager : public QObject
{
    Q_OBJECT

public:
    explicit QSocMemoryManager(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocMemoryManager() override;

    /* A single parsed memory entry */
    struct MemoryEntry
    {
        QString   path;
        QString   name;
        QString   type; /* user, feedback, project, reference */
        QString   description;
        QString   content; /* Full file content including frontmatter */
        QDateTime lastModified;
        int       ageDays = 0;
    };

    /* Lightweight header (frontmatter only, no body). Feeds the recall
     * selector so it can rank by name/type/description without reading
     * full file bodies. */
    struct MemoryHeader
    {
        QString   scope; /* "user" or "project" */
        QString   name;
        QString   type;
        QString   description;
        QDateTime lastModified;
        int       ageDays = 0;
    };

    /* Directory paths */
    QString userMemoryDir() const;
    QString projectMemoryDir() const;
    QString userIndexPath() const;
    QString projectIndexPath() const;

    /* Load MEMORY.md index content for system prompt injection */
    QString loadMemoryForPrompt(int maxChars = 24000) const;

    /* Scan topic files (excludes MEMORY.md), sorted by mtime descending */
    QList<MemoryEntry> scanMemories(const QString &scope = "all") const;

    /* Scan topic-file headers (frontmatter only), sorted by mtime
     * descending. Cheaper than scanMemories: skips file bodies. Each
     * header carries its scope ("user"/"project"). */
    QList<MemoryHeader> scanHeaders(const QString &scope = "all") const;

    /* Write a topic file with frontmatter and rebuild the index */
    bool writeTopicFile(
        const QString &scope,
        const QString &name,
        const QString &type,
        const QString &description,
        const QString &content);

    /* Rebuild MEMORY.md index from topic files in the given scope */
    bool updateIndex(const QString &scope);

    /* Per-scope topic-file cap; beyond it the oldest are dropped. */
    static int maxTopicFiles();

    /* Uncapped count of topic files (excludes MEMORY.md) in the scope. */
    int topicFileCount(const QString &scope = "all") const;

    /* Read a specific topic file by name and scope */
    QString readTopicFile(const QString &scope, const QString &name) const;

    /* Strip a leading YAML frontmatter block from content. Returns the
     * body as-is when there is no frontmatter. */
    static QString stripFrontmatter(const QString &content);

    /* Delete a topic file and rebuild the index */
    bool deleteTopicFile(const QString &scope, const QString &name);

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    /* Parse YAML frontmatter from a memory file */
    MemoryEntry parseMemoryFile(const QString &path) const;

    /* Parse frontmatter fields (name/type/description) from raw content.
     * Shared by parseMemoryFile and scanHeadersDir; only overwrites a
     * field when its key is present, so caller-supplied defaults survive. */
    static void parseFrontmatter(
        const QString &content, QString &name, QString &type, QString &description);

    /* Scan a single directory for topic files */
    QList<MemoryEntry> scanDir(const QString &dirPath) const;

    /* Scan a single directory for topic-file headers, tagging scope */
    QList<MemoryHeader> scanHeadersDir(const QString &dirPath, const QString &scope) const;

    /* File I/O helpers */
    QString readFile(const QString &path) const;
    bool    writeFile(const QString &path, const QString &content) const;
    bool    ensureDir(const QString &dirPath) const;

    /* Sanitize topic name for use as filename */
    static QString sanitizeName(const QString &name);

    /* Memory directory for a given scope */
    QString memoryDirForScope(const QString &scope) const;
};

#endif // QSOCMEMORYMANAGER_H
