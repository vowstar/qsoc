// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolfile.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

/* QSocToolFileRead Implementation */

QSocToolFileRead::QSocToolFileRead(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileRead::~QSocToolFileRead() = default;

QString QSocToolFileRead::getName() const
{
    return "read_file";
}

QString QSocToolFileRead::getDescription() const
{
    return "Read the contents of a file. Any file on the system can be read.";
}

json QSocToolFileRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Path to the file to read (relative to project or absolute)"}}},
          {"max_lines",
           {{"type", "integer"}, {"description", "Maximum number of lines to read (default: 500)"}}},
          {"offset",
           {{"type", "integer"},
            {"description", "Line number to start reading from (0-indexed, default: 0)"}}}}},
        {"required", json::array({"file_path"})}};
}

QString QSocToolFileRead::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return "Error: file_path is required";
    }

    QString filePath = QString::fromStdString(arguments["file_path"].get<std::string>());

    /* Make path absolute if relative */
    QFileInfo fileInfo(filePath);
    if (!fileInfo.isAbsolute()) {
        QString basePath;
        if (pathContext) {
            basePath = pathContext->getProjectDir();
        }
        if (basePath.isEmpty()) {
            basePath = QDir::currentPath();
        }
        filePath = QDir(basePath).absoluteFilePath(filePath);
        fileInfo = QFileInfo(filePath);
    }

    /* Check if file exists */
    if (!fileInfo.exists()) {
        return QString("Error: File not found: %1").arg(filePath);
    }

    if (!fileInfo.isFile()) {
        return QString("Error: Path is not a file: %1").arg(filePath);
    }

    /* Get parameters */
    int maxLines = 500;
    int offset   = 0;

    if (arguments.contains("max_lines") && arguments["max_lines"].is_number_integer()) {
        maxLines = arguments["max_lines"].get<int>();
        if (maxLines <= 0) {
            maxLines = 500;
        }
    }

    if (arguments.contains("offset") && arguments["offset"].is_number_integer()) {
        offset = arguments["offset"].get<int>();
        if (offset < 0) {
            offset = 0;
        }
    }

    /* Read file */
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString("Error: Cannot open file: %1").arg(filePath);
    }

    QTextStream in(&file);
    QString     result;
    int         lineNum   = 0;
    int         linesRead = 0;

    while (!in.atEnd() && linesRead < maxLines) {
        QString line = in.readLine();
        if (lineNum >= offset) {
            result += line + "\n";
            linesRead++;
        }
        lineNum++;
    }

    file.close();

    if (result.isEmpty()) {
        return QString("File is empty or offset beyond file length: %1").arg(filePath);
    }

    return result;
}

void QSocToolFileRead::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

/* QSocToolFileList Implementation */

QSocToolFileList::QSocToolFileList(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileList::~QSocToolFileList() = default;

QString QSocToolFileList::getName() const
{
    return "list_files";
}

QString QSocToolFileList::getDescription() const
{
    return "List files in a directory. Any directory on the system can be listed.";
}

json QSocToolFileList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"directory",
           {{"type", "string"},
            {"description",
             "Directory path to list (relative to project or absolute, default: project root)"}}},
          {"pattern",
           {{"type", "string"},
            {"description", "Glob pattern to filter files (e.g., '*.v', '*.yaml')"}}},
          {"recursive",
           {{"type", "boolean"}, {"description", "List files recursively (default: false)"}}},
          {"include_hidden",
           {{"type", "boolean"}, {"description", "Include hidden files (default: false)"}}}}},
        {"required", json::array()}};
}

QString QSocToolFileList::execute(const json &arguments)
{
    /* Get directory path */
    QString dirPath;
    if (arguments.contains("directory") && arguments["directory"].is_string()) {
        dirPath = QString::fromStdString(arguments["directory"].get<std::string>());
    }

    /* Make path absolute if relative or empty */
    if (dirPath.isEmpty()) {
        if (pathContext) {
            dirPath = pathContext->getProjectDir();
        }
        if (dirPath.isEmpty()) {
            dirPath = QDir::currentPath();
        }
    } else {
        QFileInfo dirInfo(dirPath);
        if (!dirInfo.isAbsolute()) {
            QString basePath;
            if (pathContext) {
                basePath = pathContext->getProjectDir();
            }
            if (basePath.isEmpty()) {
                basePath = QDir::currentPath();
            }
            dirPath = QDir(basePath).absoluteFilePath(dirPath);
        }
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        return QString("Error: Directory not found: %1").arg(dirPath);
    }

    /* Get parameters */
    QString pattern       = "*";
    bool    recursive     = false;
    bool    includeHidden = false;

    if (arguments.contains("pattern") && arguments["pattern"].is_string()) {
        pattern = QString::fromStdString(arguments["pattern"].get<std::string>());
    }

    if (arguments.contains("recursive") && arguments["recursive"].is_boolean()) {
        recursive = arguments["recursive"].get<bool>();
    }

    if (arguments.contains("include_hidden") && arguments["include_hidden"].is_boolean()) {
        includeHidden = arguments["include_hidden"].get<bool>();
    }

    /* List files */
    QStringList   files;
    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;

    if (includeHidden) {
        filters |= QDir::Hidden;
    }

    if (recursive) {
        QDirIterator it(dirPath, QStringList() << pattern, filters, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            /* Make path relative to the requested directory */
            files.append(dir.relativeFilePath(path));
        }
    } else {
        dir.setNameFilters(QStringList() << pattern);
        dir.setFilter(filters);
        for (const QFileInfo &info : dir.entryInfoList()) {
            QString entry = info.fileName();
            if (info.isDir()) {
                entry += "/";
            }
            files.append(entry);
        }
    }

    files.sort();

    if (files.isEmpty()) {
        return QString("No files found in: %1").arg(dirPath);
    }

    return QString("Files in %1:\n%2").arg(dirPath, files.join("\n"));
}

void QSocToolFileList::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

/* QSocToolFileWrite Implementation */

QSocToolFileWrite::QSocToolFileWrite(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileWrite::~QSocToolFileWrite() = default;

QString QSocToolFileWrite::getName() const
{
    return "write_file";
}

QString QSocToolFileWrite::getDescription() const
{
    return "Write content to a file. "
           "Creates the file if it doesn't exist, overwrites if it does. "
           "File must be within an allowed directory (project, working, user dirs, or temp).";
}

json QSocToolFileWrite::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Path to the file to write (relative to project or absolute)"}}},
          {"content", {{"type", "string"}, {"description", "Content to write to the file"}}}}},
        {"required", json::array({"file_path", "content"})}};
}

QString QSocToolFileWrite::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return "Error: file_path is required";
    }

    if (!arguments.contains("content") || !arguments["content"].is_string()) {
        return "Error: content is required";
    }

    QString filePath = QString::fromStdString(arguments["file_path"].get<std::string>());
    QString content  = QString::fromStdString(arguments["content"].get<std::string>());

    /* Make path absolute if relative */
    QFileInfo fileInfo(filePath);
    if (!fileInfo.isAbsolute()) {
        QString basePath;
        if (pathContext) {
            basePath = pathContext->getProjectDir();
        }
        if (basePath.isEmpty()) {
            basePath = QDir::currentPath();
        }
        filePath = QDir(basePath).absoluteFilePath(filePath);
        fileInfo = QFileInfo(filePath);
    }

    /* Security check */
    if (pathContext && !pathContext->isWriteAllowed(filePath)) {
        return "Error: Access denied. File must be within an allowed directory "
               "(project, working, user, or temp).";
    }

    /* Create parent directories if needed */
    QDir parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        if (!parentDir.mkpath(".")) {
            return QString("Error: Cannot create directory: %1").arg(parentDir.absolutePath());
        }
    }

    /* Write file */
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QString("Error: Cannot open file for writing: %1").arg(filePath);
    }

    QTextStream out(&file);
    out << content;
    file.close();

    return QString("Successfully wrote %1 bytes to: %2").arg(content.size()).arg(filePath);
}

void QSocToolFileWrite::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

/* QSocToolFileEdit Implementation */

QSocToolFileEdit::QSocToolFileEdit(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileEdit::~QSocToolFileEdit() = default;

QString QSocToolFileEdit::getName() const
{
    return "edit_file";
}

QString QSocToolFileEdit::getDescription() const
{
    return "Edit a file by replacing a specific string with new content. "
           "The old_string must be unique in the file for the replacement to succeed. "
           "File must be within an allowed directory (project, working, user dirs, or temp).";
}

json QSocToolFileEdit::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Path to the file to edit (relative to project or absolute)"}}},
          {"old_string", {{"type", "string"}, {"description", "The text to replace"}}},
          {"new_string", {{"type", "string"}, {"description", "The replacement text"}}},
          {"replace_all",
           {{"type", "boolean"},
            {"description", "Replace all occurrences (default: false, requires unique match)"}}}}},
        {"required", json::array({"file_path", "old_string", "new_string"})}};
}

QString QSocToolFileEdit::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return "Error: file_path is required";
    }

    if (!arguments.contains("old_string") || !arguments["old_string"].is_string()) {
        return "Error: old_string is required";
    }

    if (!arguments.contains("new_string") || !arguments["new_string"].is_string()) {
        return "Error: new_string is required";
    }

    QString filePath  = QString::fromStdString(arguments["file_path"].get<std::string>());
    QString oldString = QString::fromStdString(arguments["old_string"].get<std::string>());
    QString newString = QString::fromStdString(arguments["new_string"].get<std::string>());

    bool replaceAll = false;
    if (arguments.contains("replace_all") && arguments["replace_all"].is_boolean()) {
        replaceAll = arguments["replace_all"].get<bool>();
    }

    /* Make path absolute if relative */
    QFileInfo fileInfo(filePath);
    if (!fileInfo.isAbsolute()) {
        QString basePath;
        if (pathContext) {
            basePath = pathContext->getProjectDir();
        }
        if (basePath.isEmpty()) {
            basePath = QDir::currentPath();
        }
        filePath = QDir(basePath).absoluteFilePath(filePath);
        fileInfo = QFileInfo(filePath);
    }

    /* Security check */
    if (pathContext && !pathContext->isWriteAllowed(filePath)) {
        return "Error: Access denied. File must be within an allowed directory "
               "(project, working, user, or temp).";
    }

    /* Check if file exists */
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return QString("Error: File not found: %1").arg(filePath);
    }

    /* Read file */
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString("Error: Cannot open file for reading: %1").arg(filePath);
    }

    QTextStream readStream(&file);
    QString     content = readStream.readAll();
    file.close();

    /* Check for old_string */
    int count = content.count(oldString);
    if (count == 0) {
        return QString("Error: old_string not found in file: %1").arg(filePath);
    }

    if (!replaceAll && count > 1) {
        return QString(
                   "Error: old_string found %1 times. Use replace_all=true or provide more "
                   "context for unique match.")
            .arg(count);
    }

    /* Perform replacement */
    QString newContent;
    if (replaceAll) {
        newContent = content.replace(oldString, newString);
    } else {
        int pos    = content.indexOf(oldString);
        newContent = content.left(pos) + newString + content.mid(pos + oldString.length());
    }

    /* Write file */
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return QString("Error: Cannot open file for writing: %1").arg(filePath);
    }

    QTextStream writeStream(&file);
    writeStream << newContent;
    file.close();

    return QString("Successfully edited file: %1 (%2 replacement(s))")
        .arg(filePath)
        .arg(replaceAll ? count : 1);
}

void QSocToolFileEdit::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}
