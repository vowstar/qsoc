// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolpath.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

namespace {

constexpr Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool pathIsWithin(const QString &path, const QString &directory)
{
    const QString normalizedPath      = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QString       normalizedDirectory = QDir::cleanPath(QDir::fromNativeSeparators(directory));
    if (normalizedPath.compare(normalizedDirectory, pathCaseSensitivity()) == 0) {
        return true;
    }
    if (!normalizedDirectory.endsWith('/')) {
        normalizedDirectory += '/';
    }
    return normalizedPath.startsWith(normalizedDirectory, pathCaseSensitivity());
}

QString normalizedRoot(const QString &path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

QString rootKey(const QString &path)
{
    const QString normalized = normalizedRoot(path);
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

QString canonicalRoot(const QString &path)
{
    return QDir::fromNativeSeparators(QFileInfo(normalizedRoot(path)).canonicalFilePath());
}

} // namespace

/* QSocPathContext Implementation */

QSocPathContext::QSocPathContext(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
    , workingDir(QDir::currentPath())
{
    bindWritableRoot(workingDir);
    bindWritableRoot(QDir::tempPath());
    if (projectManager != nullptr) {
        bindWritableRoot(projectManager->getProjectPath());
    }
}

QString QSocPathContext::getProjectDir() const
{
    if (projectManager) {
        return projectManager->getProjectPath();
    }
    return QString();
}

QString QSocPathContext::getWorkingDir() const
{
    QMutexLocker locker(&mutex);
    return workingDir;
}

QStringList QSocPathContext::getUserDirs() const
{
    QMutexLocker locker(&mutex);
    return userDirs;
}

void QSocPathContext::setWorkingDir(const QString &dir)
{
    QMutexLocker locker(&mutex);
    QFileInfo    info(dir);
    if (info.isDir()) {
        workingDir           = normalizedRoot(dir);
        const QString anchor = canonicalRoot(workingDir);
        if (!anchor.isEmpty()) {
            writableAnchors.insert(rootKey(workingDir), anchor);
        }
    }
}

void QSocPathContext::addUserDir(const QString &dir)
{
    QMutexLocker locker(&mutex);
    QFileInfo    info(dir);

    /* Only add existing directories */
    if (!info.exists() || !info.isDir()) {
        return;
    }

    const QString absPath = normalizedRoot(dir);
    const QString anchor  = canonicalRoot(absPath);
    if (anchor.isEmpty()) {
        return;
    }

    /* Avoid duplicates */
    if (userDirs.contains(absPath)) {
        writableAnchors.insert(rootKey(absPath), anchor);
        return;
    }

    /* Limit size - remove oldest if full */
    if (userDirs.size() >= MaxUserDirs) {
        userDirs.removeFirst();
    }

    userDirs.append(absPath);
    writableAnchors.insert(rootKey(absPath), anchor);
}

void QSocPathContext::removeUserDir(const QString &dir)
{
    QMutexLocker locker(&mutex);
    userDirs.removeAll(normalizedRoot(dir));
}

void QSocPathContext::clearUserDirs()
{
    QMutexLocker locker(&mutex);
    userDirs.clear();
}

bool QSocPathContext::isWriteAllowed(const QString &path) const
{
    return resolveWritablePath(path, nullptr);
}

bool QSocPathContext::resolveWritablePath(const QString &path, QString *resolved) const
{
    const QString absolute = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    QString       probe    = QDir::cleanPath(absolute);
    QStringList   tail;
    QString       canonicalPath;
    for (;;) {
        const QFileInfo info(probe);
        if (info.exists() || info.isSymLink()) {
            canonicalPath = info.canonicalFilePath();
            break;
        }
        const QString name = info.fileName();
        const QString next = info.absolutePath();
        if (name.isEmpty() || next == probe) {
            return false;
        }
        tail.prepend(name);
        probe = next;
    }
    if (canonicalPath.isEmpty()) {
        return false;
    }
    for (const QString &part : tail) {
        canonicalPath = QDir(canonicalPath).filePath(part);
    }

    for (const WritableRoot &root : writableRoots()) {
        const QString current = canonicalRoot(root.lexical);
        if (!root.anchor.isEmpty() && current.compare(root.anchor, pathCaseSensitivity()) == 0
            && pathIsWithin(canonicalPath, root.anchor)) {
            if (resolved != nullptr) {
                *resolved = QDir::fromNativeSeparators(QDir::cleanPath(canonicalPath));
            }
            return true;
        }
    }
    return false;
}

bool QSocPathContext::resolveWritableEntry(const QString &path, QString *entry) const
{
    const QString absolute = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    const QFileInfo info(absolute);
    if (info.fileName().isEmpty()) {
        return false;
    }
    QString canonicalParent;
    if (!resolveWritablePath(info.absolutePath(), &canonicalParent)) {
        return false;
    }
    const QString resolved = QDir::fromNativeSeparators(
        QDir(canonicalParent).filePath(info.fileName()));
    if (resolved.compare(absolute, pathCaseSensitivity()) != 0) {
        return false;
    }
    if (entry != nullptr) {
        *entry = resolved;
    }
    return true;
}

QStringList QSocPathContext::getWritableDirs() const
{
    QStringList dirs;
    for (const WritableRoot &root : writableRoots()) {
        dirs.append(root.lexical);
    }
    return dirs;
}

void QSocPathContext::bindWritableRoot(const QString &dir)
{
    if (dir.isEmpty()) {
        return;
    }
    const QString anchor = canonicalRoot(dir);
    if (anchor.isEmpty()) {
        return;
    }
    QMutexLocker locker(&mutex);
    writableAnchors.insert(rootKey(dir), anchor);
}

QList<QSocPathContext::WritableRoot> QSocPathContext::writableRoots() const
{
    QMutexLocker        locker(&mutex);
    QList<WritableRoot> roots;
    QSet<QString>       seen;
    const auto          append = [this, &roots, &seen](const QString &dir, bool admitNew) {
        if (dir.isEmpty()) {
            return;
        }
        const QString lexical = normalizedRoot(dir);
        const QString key     = rootKey(lexical);
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        if (admitNew && !writableAnchors.contains(key)) {
            const QString anchor = canonicalRoot(lexical);
            if (!anchor.isEmpty()) {
                writableAnchors.insert(key, anchor);
            }
        }
        roots.append({lexical, writableAnchors.value(key)});
    };

    /* Project directory */
    if (projectManager) {
        append(projectManager->getProjectPath(), true);
    }

    /* Working directory */
    append(workingDir, false);

    /* User directories */
    for (const QString &dir : userDirs) {
        append(dir, false);
    }

    /* System temp directory */
    append(QDir::tempPath(), false);

    return roots;
}

QString QSocPathContext::getSummary() const
{
    QMutexLocker locker(&mutex);

    QStringList parts;

    QString projDir = getProjectDir();
    if (!projDir.isEmpty()) {
        /* Show only last component for brevity */
        parts.append("P:" + QDir(projDir).dirName());
    }

    if (!workingDir.isEmpty()) {
        parts.append("W:" + QDir(workingDir).dirName());
    }

    if (!userDirs.isEmpty()) {
        parts.append(QString("U:%1").arg(userDirs.size()));
    }

    return parts.isEmpty() ? "No paths" : parts.join(" ");
}

QString QSocPathContext::getFullContext() const
{
    QMutexLocker locker(&mutex);

    QString result;

    QString projDir = getProjectDir();
    if (!projDir.isEmpty()) {
        QFileInfo info(projDir);
        QString   status = info.exists() && info.isDir() ? "" : " [missing]";
        result += QString("Project: %1%2\n").arg(projDir, status);
    }

    if (!workingDir.isEmpty()) {
        QFileInfo info(workingDir);
        QString   status = info.exists() && info.isDir() ? "" : " [missing]";
        result += QString("Working: %1%2\n").arg(workingDir, status);
    }

    if (!userDirs.isEmpty()) {
        result += "Recent:\n";
        for (const QString &dir : userDirs) {
            QFileInfo info(dir);
            QString   status = info.exists() && info.isDir() ? "" : " [missing]";
            result += QString("  - %1%2\n").arg(dir, status);
        }
    }

    return result.isEmpty() ? "No paths configured." : result.trimmed();
}

/* QSocToolPathContext Implementation */

QSocToolPathContext::QSocToolPathContext(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolPathContext::~QSocToolPathContext() = default;

QString QSocToolPathContext::getName() const
{
    return "path_context";
}

QString QSocToolPathContext::getDescription() const
{
    return "Manage commonly used directory paths. "
           "Actions: 'list' (show all paths), 'set_working' (change working dir), "
           "'add' (remember a user directory), 'remove' (forget a directory), 'clear' (clear user "
           "dirs). "
           "Use this to track project and working directories for file operations.";
}

json QSocToolPathContext::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", {"list", "set_working", "add", "remove", "clear"}},
            {"description", "Action to perform"}}},
          {"path",
           {{"type", "string"},
            {"description", "Directory path (required for set_working, add, remove)"}}}}},
        {"required", json::array({"action"})}};
}

QString QSocToolPathContext::execute(const json &arguments)
{
    if (!pathContext) {
        return "Error: Path context not configured";
    }

    if (!arguments.contains("action") || !arguments["action"].is_string()) {
        return "Error: action is required";
    }

    QString action = QString::fromStdString(arguments["action"].get<std::string>());

    if (action == "list") {
        return pathContext->getFullContext();
    }

    if (action == "clear") {
        pathContext->clearUserDirs();
        return "User directories cleared.";
    }

    /* Actions requiring path parameter */
    if (!arguments.contains("path") || !arguments["path"].is_string()) {
        return QString("Error: path is required for action '%1'").arg(action);
    }

    QString path = QString::fromStdString(arguments["path"].get<std::string>());

    if (action == "set_working") {
        QFileInfo info(path);
        if (!info.isDir()) {
            return QString("Error: '%1' is not a valid directory").arg(path);
        }
        pathContext->setWorkingDir(path);
        return QString("Working directory set to: %1").arg(pathContext->getWorkingDir());
    }

    if (action == "add") {
        QFileInfo info(path);
        if (!info.exists() || !info.isDir()) {
            return QString("Error: '%1' does not exist or is not a directory").arg(path);
        }
        pathContext->addUserDir(path);
        return QString("Added to path context: %1").arg(info.absoluteFilePath());
    }

    if (action == "remove") {
        pathContext->removeUserDir(path);
        return QString("Removed from path context: %1").arg(path);
    }

    return QString("Error: Unknown action '%1'").arg(action);
}
