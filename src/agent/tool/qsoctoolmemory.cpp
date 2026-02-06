// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolmemory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

/* QSocToolMemoryRead Implementation */

QSocToolMemoryRead::QSocToolMemoryRead(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolMemoryRead::~QSocToolMemoryRead() = default;

QString QSocToolMemoryRead::getName() const
{
    return "memory_read";
}

QString QSocToolMemoryRead::getDescription() const
{
    return "Read persistent memory containing user preferences and project context. "
           "Memory is stored in Markdown format and persists across sessions.";
}

json QSocToolMemoryRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"scope",
           {{"type", "string"},
            {"enum", {"user", "project", "all"}},
            {"description",
             "Which memory to read: 'user' for user preferences, 'project' for project "
             "context, 'all' for both (default: all)"}}}}},
        {"required", json::array()}};
}

QString QSocToolMemoryRead::userMemoryPath() const
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configPath).filePath("qsoc/memory.md");
}

QString QSocToolMemoryRead::projectMemoryPath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/memory.md");
}

QString QSocToolMemoryRead::readMemoryFile(const QString &filePath) const
{
    if (filePath.isEmpty()) {
        return {};
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return content;
}

QString QSocToolMemoryRead::execute(const json &arguments)
{
    QString scope = "all";
    if (arguments.contains("scope") && arguments["scope"].is_string()) {
        scope = QString::fromStdString(arguments["scope"].get<std::string>());
    }

    QString result;

    /* Read user memory */
    if (scope == "user" || scope == "all") {
        QString userPath   = userMemoryPath();
        QString userMemory = readMemoryFile(userPath);

        if (!userMemory.isEmpty()) {
            result += "## User Memory\n\n";
            result += userMemory;
            result += "\n";
        } else if (scope == "user") {
            result += "No user memory found at: " + userPath + "\n";
        }
    }

    /* Read project memory */
    if (scope == "project" || scope == "all") {
        QString projectPath   = projectMemoryPath();
        QString projectMemory = readMemoryFile(projectPath);

        if (!projectMemory.isEmpty()) {
            if (!result.isEmpty()) {
                result += "\n---\n\n";
            }
            result += "## Project Memory\n\n";
            result += projectMemory;
            result += "\n";
        } else if (scope == "project") {
            result += "No project memory found at: " + projectPath + "\n";
        }
    }

    if (result.isEmpty()) {
        return "No memory found. Use memory_write to save preferences and context.";
    }

    return result;
}

void QSocToolMemoryRead::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolMemoryWrite Implementation */

QSocToolMemoryWrite::QSocToolMemoryWrite(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolMemoryWrite::~QSocToolMemoryWrite() = default;

QString QSocToolMemoryWrite::getName() const
{
    return "memory_write";
}

QString QSocToolMemoryWrite::getDescription() const
{
    return "Write persistent memory to save user preferences or project context. "
           "Memory is stored in Markdown format and persists across sessions. "
           "Use 'user' scope for user preferences, 'project' scope for project-specific context.";
}

json QSocToolMemoryWrite::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"content",
           {{"type", "string"},
            {"description", "The content to write (Markdown format recommended)"}}},
          {"scope",
           {{"type", "string"},
            {"enum", {"user", "project"}},
            {"description",
             "Where to save: 'user' for user preferences (~/.config/qsoc/memory.md), "
             "'project' for project context (<project>/.qsoc/memory.md)"}}},
          {"append",
           {{"type", "boolean"},
            {"description",
             "If true, append to existing content instead of replacing "
             "(default: false)"}}}}},
        {"required", json::array({"content", "scope"})}};
}

QString QSocToolMemoryWrite::userMemoryPath() const
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configPath).filePath("qsoc/memory.md");
}

QString QSocToolMemoryWrite::projectMemoryPath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/memory.md");
}

bool QSocToolMemoryWrite::writeMemoryFile(const QString &filePath, const QString &content) const
{
    if (filePath.isEmpty()) {
        return false;
    }

    /* Create parent directories if needed */
    QFileInfo fileInfo(filePath);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        if (!parentDir.mkpath(".")) {
            return false;
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << content;
    file.close();

    return true;
}

QString QSocToolMemoryWrite::execute(const json &arguments)
{
    if (!arguments.contains("content") || !arguments["content"].is_string()) {
        return "Error: content is required";
    }

    if (!arguments.contains("scope") || !arguments["scope"].is_string()) {
        return "Error: scope is required (must be 'user' or 'project')";
    }

    QString content = QString::fromStdString(arguments["content"].get<std::string>());
    QString scope   = QString::fromStdString(arguments["scope"].get<std::string>());

    bool append = false;
    if (arguments.contains("append") && arguments["append"].is_boolean()) {
        append = arguments["append"].get<bool>();
    }

    /* Determine file path */
    QString filePath;
    if (scope == "user") {
        filePath = userMemoryPath();
    } else if (scope == "project") {
        filePath = projectMemoryPath();
        if (filePath.isEmpty()) {
            return "Error: No project directory available for project-scoped memory";
        }
    } else {
        return "Error: scope must be 'user' or 'project'";
    }

    /* Handle append mode */
    if (append) {
        QFile file(filePath);
        if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            QString     existing = stream.readAll();
            file.close();
            content = existing + "\n" + content;
        }
    }

    /* Write the file */
    if (!writeMemoryFile(filePath, content)) {
        return QString("Error: Failed to write memory to: %1").arg(filePath);
    }

    return QString("Successfully saved memory to: %1 (%2 bytes)").arg(filePath).arg(content.size());
}

void QSocToolMemoryWrite::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}
