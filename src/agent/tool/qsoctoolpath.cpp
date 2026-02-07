// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolpath.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

/* QSocPathContext Implementation */

QSocPathContext::QSocPathContext(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
    , workingDir(QDir::currentPath())
{}

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
        workingDir = info.absoluteFilePath();
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

    QString absPath = info.absoluteFilePath();

    /* Avoid duplicates */
    if (userDirs.contains(absPath)) {
        return;
    }

    /* Limit size - remove oldest if full */
    if (userDirs.size() >= MaxUserDirs) {
        userDirs.removeFirst();
    }

    userDirs.append(absPath);
}

void QSocPathContext::removeUserDir(const QString &dir)
{
    QMutexLocker locker(&mutex);
    QFileInfo    info(dir);
    userDirs.removeAll(info.absoluteFilePath());
}

void QSocPathContext::clearUserDirs()
{
    QMutexLocker locker(&mutex);
    userDirs.clear();
}

bool QSocPathContext::isWriteAllowed(const QString &path) const
{
    QStringList dirs = getWritableDirs();

    QFileInfo fileInfo(path);
    QString   canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        /* File doesn't exist yet, check parent */
        QFileInfo parentInfo(fileInfo.absolutePath());
        canonicalPath = parentInfo.canonicalFilePath();
        if (canonicalPath.isEmpty()) {
            return false;
        }
    }

    for (const QString &dir : dirs) {
        QDir    d(dir);
        QString canonicalDir = d.canonicalPath();
        if (!canonicalDir.isEmpty() && canonicalPath.startsWith(canonicalDir)) {
            return true;
        }
    }
    return false;
}

QStringList QSocPathContext::getWritableDirs() const
{
    QMutexLocker locker(&mutex);
    QStringList  dirs;

    /* Project directory */
    if (projectManager) {
        QString projDir = projectManager->getProjectPath();
        if (!projDir.isEmpty()) {
            dirs << projDir;
        }
    }

    /* Working directory */
    if (!workingDir.isEmpty()) {
        dirs << workingDir;
    }

    /* User directories */
    dirs << userDirs;

    /* System temp directory */
    dirs << QDir::tempPath();

    return dirs;
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
