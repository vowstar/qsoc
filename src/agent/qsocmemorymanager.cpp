// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemorymanager.h"

#include "common/qsocpaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

/* Limits */
static constexpr int MAX_TOPIC_FILES = 50;
static constexpr int MAX_INDEX_LINES = 200;
static constexpr int MAX_INDEX_BYTES = 25000;
static constexpr int STALENESS_DAYS  = 1;
static const QString INDEX_FILENAME  = QStringLiteral("MEMORY.md");

QSocMemoryManager::QSocMemoryManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
{}

QSocMemoryManager::~QSocMemoryManager() = default;

void QSocMemoryManager::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

/* Directory paths */

QString QSocMemoryManager::userMemoryDir() const
{
    return QDir(QSocPaths::userRoot()).filePath("memory");
}

QString QSocMemoryManager::projectMemoryDir() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    const QString root = QSocPaths::projectRoot(projectPath);
    return root.isEmpty() ? QString() : QDir(root).filePath("memory");
}

QString QSocMemoryManager::userIndexPath() const
{
    return QDir(userMemoryDir()).filePath(INDEX_FILENAME);
}

QString QSocMemoryManager::projectIndexPath() const
{
    QString dir = projectMemoryDir();
    if (dir.isEmpty()) {
        return {};
    }
    return QDir(dir).filePath(INDEX_FILENAME);
}

QString QSocMemoryManager::memoryDirForScope(const QString &scope) const
{
    if (scope == "user") {
        return userMemoryDir();
    }
    if (scope == "project") {
        return projectMemoryDir();
    }
    return {};
}

/* Load index content for system prompt injection */

QString QSocMemoryManager::loadMemoryForPrompt(int maxChars) const
{
    QString result;

    /* Read user-level index */
    QString userIndex = readFile(userIndexPath());
    if (!userIndex.isEmpty()) {
        result += "### User Memory\n\n" + userIndex + "\n";
    }

    /* Read project-level index */
    QString projIndexFile = projectIndexPath();
    if (!projIndexFile.isEmpty()) {
        QString projIndex = readFile(projIndexFile);
        if (!projIndex.isEmpty()) {
            if (!result.isEmpty()) {
                result += "\n";
            }
            result += "### Project Memory\n\n" + projIndex + "\n";
        }
    }

    /* Truncate to budget */
    if (result.size() > maxChars) {
        result.truncate(maxChars);
        /* Find last newline to avoid cutting mid-line */
        qsizetype lastNewline = result.lastIndexOf('\n');
        if (lastNewline > maxChars / 2) {
            result.truncate(lastNewline + 1);
        }
        result += "\n(Memory truncated due to size limit)\n";
    }

    return result;
}

/* Scan topic files */

QList<QSocMemoryManager::MemoryEntry> QSocMemoryManager::scanMemories(const QString &scope) const
{
    QList<MemoryEntry> entries;

    if (scope == "user" || scope == "all") {
        entries.append(scanDir(userMemoryDir()));
    }

    if (scope == "project" || scope == "all") {
        QString projDir = projectMemoryDir();
        if (!projDir.isEmpty()) {
            entries.append(scanDir(projDir));
        }
    }

    /* Sort by mtime descending (newest first) */
    std::sort(entries.begin(), entries.end(), [](const MemoryEntry &lhs, const MemoryEntry &rhs) {
        return lhs.lastModified > rhs.lastModified;
    });

    /* Enforce file limit */
    if (entries.size() > MAX_TOPIC_FILES) {
        entries.resize(MAX_TOPIC_FILES);
    }

    return entries;
}

QList<QSocMemoryManager::MemoryEntry> QSocMemoryManager::scanDir(const QString &dirPath) const
{
    QList<MemoryEntry> entries;
    QDir               dir(dirPath);

    if (!dir.exists()) {
        return entries;
    }

    QDirIterator iter(dirPath, {"*.md"}, QDir::Files | QDir::Readable, QDirIterator::NoIteratorFlags);

    while (iter.hasNext()) {
        iter.next();
        /* Skip the index file */
        if (iter.fileName() == INDEX_FILENAME) {
            continue;
        }
        entries.append(parseMemoryFile(iter.filePath()));
    }

    return entries;
}

int QSocMemoryManager::maxTopicFiles()
{
    return MAX_TOPIC_FILES;
}

int QSocMemoryManager::topicFileCount(const QString &scope) const
{
    const auto countDir = [](const QString &dirPath) {
        int  files = 0;
        QDir dir(dirPath);
        if (!dir.exists()) {
            return files;
        }
        QDirIterator
            iter(dirPath, {"*.md"}, QDir::Files | QDir::Readable, QDirIterator::NoIteratorFlags);
        while (iter.hasNext()) {
            iter.next();
            if (iter.fileName() != INDEX_FILENAME) {
                files++;
            }
        }
        return files;
    };

    int total = 0;
    if (scope == "user" || scope == "all") {
        total += countDir(userMemoryDir());
    }
    if (scope == "project" || scope == "all") {
        const QString projDir = projectMemoryDir();
        if (!projDir.isEmpty()) {
            total += countDir(projDir);
        }
    }
    return total;
}

/* Scan topic-file headers (frontmatter only) */

QList<QSocMemoryManager::MemoryHeader> QSocMemoryManager::scanHeaders(const QString &scope) const
{
    QList<MemoryHeader> headers;

    if (scope == "user" || scope == "all") {
        headers.append(scanHeadersDir(userMemoryDir(), "user"));
    }

    if (scope == "project" || scope == "all") {
        QString projDir = projectMemoryDir();
        if (!projDir.isEmpty()) {
            headers.append(scanHeadersDir(projDir, "project"));
        }
    }

    /* Sort by mtime descending (newest first) */
    std::sort(headers.begin(), headers.end(), [](const MemoryHeader &lhs, const MemoryHeader &rhs) {
        return lhs.lastModified > rhs.lastModified;
    });

    /* Enforce file limit */
    if (headers.size() > MAX_TOPIC_FILES) {
        headers.resize(MAX_TOPIC_FILES);
    }

    return headers;
}

QList<QSocMemoryManager::MemoryHeader> QSocMemoryManager::scanHeadersDir(
    const QString &dirPath, const QString &scope) const
{
    QList<MemoryHeader> headers;
    QDir                dir(dirPath);

    if (!dir.exists()) {
        return headers;
    }

    QDirIterator iter(dirPath, {"*.md"}, QDir::Files | QDir::Readable, QDirIterator::NoIteratorFlags);

    while (iter.hasNext()) {
        iter.next();
        /* Skip the index file */
        if (iter.fileName() == INDEX_FILENAME) {
            continue;
        }

        QFileInfo    fileInfo(iter.filePath());
        MemoryHeader header;
        header.scope        = scope;
        header.name         = fileInfo.completeBaseName();
        header.lastModified = fileInfo.lastModified();
        header.ageDays = static_cast<int>(header.lastModified.daysTo(QDateTime::currentDateTime()));
        parseFrontmatter(readFile(iter.filePath()), header.name, header.type, header.description);
        headers.append(header);
    }

    return headers;
}

/* Parse frontmatter from a memory file */

QSocMemoryManager::MemoryEntry QSocMemoryManager::parseMemoryFile(const QString &path) const
{
    MemoryEntry entry;
    entry.path = path;

    QFileInfo fileInfo(path);
    entry.lastModified = fileInfo.lastModified();
    entry.ageDays      = static_cast<int>(entry.lastModified.daysTo(QDateTime::currentDateTime()));

    /* Derive name from filename (without .md extension) */
    entry.name = fileInfo.completeBaseName();

    QString content = readFile(path);
    entry.content   = content;

    if (content.isEmpty()) {
        return entry;
    }

    parseFrontmatter(content, entry.name, entry.type, entry.description);

    return entry;
}

/* Strip a leading YAML frontmatter block from content */

QString QSocMemoryManager::stripFrontmatter(const QString &content)
{
    if (!content.startsWith("---")) {
        return content;
    }
    const qsizetype endMarker = content.indexOf("\n---", 3);
    if (endMarker < 0) {
        return content;
    }
    /* Only treat the leading block as frontmatter when it actually holds a
     * `key:` line; otherwise a body that merely opens with a `---` thematic
     * rule would be silently truncated. */
    static const QRegularExpression keyLine(QStringLiteral("(^|\\n)\\s*[A-Za-z0-9_-]+\\s*:"));
    if (!content.left(endMarker).contains(keyLine)) {
        return content;
    }
    const qsizetype bodyStart = content.indexOf('\n', endMarker + 1);
    if (bodyStart < 0) {
        return {};
    }
    return content.mid(bodyStart + 1).trimmed();
}

/* Parse YAML frontmatter fields shared by parseMemoryFile and scanHeaders */

void QSocMemoryManager::parseFrontmatter(
    const QString &content, QString &name, QString &type, QString &description)
{
    /* Frontmatter sits between --- delimiters at the top of the file */
    if (!content.startsWith("---")) {
        return;
    }

    qsizetype endMarker = content.indexOf("\n---", 3);
    if (endMarker < 0) {
        return;
    }

    QString           frontmatter = content.mid(4, endMarker - 4);
    const QStringList lines       = frontmatter.split('\n');

    for (const QString &line : lines) {
        qsizetype colonPos = line.indexOf(':');
        if (colonPos < 0) {
            continue;
        }

        QString key   = line.left(colonPos).trimmed();
        QString value = line.mid(colonPos + 1).trimmed();

        if (key == "name") {
            name = value;
        } else if (key == "type") {
            type = value;
        } else if (key == "description") {
            description = value;
        }
    }
}

/* Write a topic file with frontmatter and rebuild the index */

bool QSocMemoryManager::writeTopicFile(
    const QString &scope,
    const QString &name,
    const QString &type,
    const QString &description,
    const QString &content)
{
    QString dir = memoryDirForScope(scope);
    if (dir.isEmpty()) {
        return false;
    }

    if (!ensureDir(dir)) {
        return false;
    }

    QString filename = sanitizeName(name) + ".md";
    QString filePath = QDir(dir).filePath(filename);

    /* Build file with frontmatter */
    QString     fileContent;
    QTextStream out(&fileContent);
    out << "---\n";
    out << "name: " << name << "\n";
    if (!type.isEmpty()) {
        out << "type: " << type << "\n";
    }
    if (!description.isEmpty()) {
        out << "description: " << description << "\n";
    }
    out << "---\n\n";
    /* Defensively drop any frontmatter the caller embedded in the body
     * (e.g. a model that echoed memory_read output) to avoid nesting. */
    out << stripFrontmatter(content);

    if (!writeFile(filePath, fileContent)) {
        return false;
    }

    /* Auto-rebuild the index */
    updateIndex(scope);

    return true;
}

/* Read a specific topic file by name */

QString QSocMemoryManager::readTopicFile(const QString &scope, const QString &name) const
{
    QString dir = memoryDirForScope(scope);
    if (dir.isEmpty()) {
        return {};
    }

    QString filename = sanitizeName(name) + ".md";
    QString filePath = QDir(dir).filePath(filename);

    return readFile(filePath);
}

/* Delete a topic file and rebuild the index */

bool QSocMemoryManager::deleteTopicFile(const QString &scope, const QString &name)
{
    QString dir = memoryDirForScope(scope);
    if (dir.isEmpty()) {
        return false;
    }

    QString filename = sanitizeName(name) + ".md";
    QString filePath = QDir(dir).filePath(filename);

    if (!QFile::remove(filePath)) {
        return false;
    }

    updateIndex(scope);
    return true;
}

/* Rebuild MEMORY.md index from topic files */

bool QSocMemoryManager::updateIndex(const QString &scope)
{
    QString dir = memoryDirForScope(scope);
    if (dir.isEmpty()) {
        return false;
    }

    QList<MemoryEntry> entries = scanDir(dir);

    /* Sort by name for stable ordering */
    std::sort(entries.begin(), entries.end(), [](const MemoryEntry &lhs, const MemoryEntry &rhs) {
        return lhs.name < rhs.name;
    });

    /* Build index content */
    QString indexContent;
    int     lineCount = 0;

    for (const auto &entry : entries) {
        if (lineCount >= MAX_INDEX_LINES) {
            indexContent += "\n(Index truncated: too many entries)\n";
            break;
        }

        QString desc = entry.description.isEmpty() ? entry.name : entry.description;
        QString line
            = QString("- [%1](%2.md) — %3\n").arg(entry.name, sanitizeName(entry.name), desc);
        qsizetype newSize = indexContent.toUtf8().size() + line.toUtf8().size();

        if (newSize > MAX_INDEX_BYTES) {
            indexContent += "\n(Index truncated: size limit reached)\n";
            break;
        }

        indexContent += line;
        lineCount++;
    }

    QString indexPath = QDir(dir).filePath(INDEX_FILENAME);
    return writeFile(indexPath, indexContent);
}

/* File I/O helpers */

QString QSocMemoryManager::readFile(const QString &path) const
{
    if (path.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return content;
}

bool QSocMemoryManager::writeFile(const QString &path, const QString &content) const
{
    if (path.isEmpty()) {
        return false;
    }

    if (!ensureDir(QFileInfo(path).absolutePath())) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << content;
    file.close();

    return true;
}

bool QSocMemoryManager::ensureDir(const QString &dirPath) const
{
    QDir dir(dirPath);
    if (dir.exists()) {
        return true;
    }
    return dir.mkpath(".");
}

/* Sanitize name for use as filename */

QString QSocMemoryManager::sanitizeName(const QString &name)
{
    QString result = name.toLower().trimmed();

    /* Replace spaces and underscores with hyphens */
    result.replace(' ', '-');
    result.replace('_', '-');

    /* Remove anything that's not alphanumeric or hyphen */
    static const QRegularExpression invalidChars("[^a-z0-9-]");
    result.remove(invalidChars);

    /* Collapse multiple hyphens */
    static const QRegularExpression multiHyphen("-{2,}");
    result.replace(multiHyphen, "-");

    /* Trim leading/trailing hyphens */
    while (result.startsWith('-')) {
        result.remove(0, 1);
    }
    while (result.endsWith('-')) {
        result.chop(1);
    }

    /* Enforce length limit */
    if (result.size() > 64) {
        result.truncate(64);
    }

    /* Fallback for empty result */
    if (result.isEmpty()) {
        result = "untitled";
    }

    return result;
}
