// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolfilecore.h"

#include "common/qsocimageattach.h"
#include "common/qsocshellpath.h"

#include <QDir>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <climits>
#include <utility>

namespace QSocToolFileCore {

namespace {

const char *const kPathRule
    = "relative paths resolve against the working directory, ~ is the home directory";

json pathProperty(const char *what)
{
    return {
        {"type", "string"},
        {"description",
         QStringLiteral("%1 (absolute, or %2)")
             .arg(QString::fromLatin1(what), QString::fromLatin1(kPathRule))
             .toStdString()}};
}

QString joinPath(const QString &dir, const QString &name)
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

QString stringArg(const json &arguments, const char *key)
{
    const auto it = arguments.find(key);
    return it != arguments.end() && it->is_string() ? QString::fromStdString(it->get<std::string>())
                                                    : QString();
}

bool hasString(const json &arguments, const char *key)
{
    const auto it = arguments.find(key);
    return it != arguments.end() && it->is_string();
}

bool boolArg(const json &arguments, const char *key)
{
    const auto it = arguments.find(key);
    return it != arguments.end() && it->is_boolean() && it->get<bool>();
}

int intArg(const json &arguments, const char *key, int fallback, int minimum)
{
    const auto it = arguments.find(key);
    if (it == arguments.end() || !it->is_number_integer()) {
        return fallback;
    }
    const auto value = it->get<long long>();
    return value < minimum ? fallback : static_cast<int>(std::min<long long>(value, INT_MAX));
}

/* Depth-first walk of one listing, sorted per directory. */
class Walker
{
public:
    Walker(const QString &root, const ListCall &call, bool caseSensitive, const ListDir &listDir)
        : m_root(root)
        , m_call(call)
        , m_listDir(listDir)
        , m_match(
              QRegularExpression::wildcardToRegularExpression(call.pattern),
              caseSensitive ? QRegularExpression::NoPatternOption
                            : QRegularExpression::CaseInsensitiveOption)
    {}

    /* False once the limit is reached. */
    bool visit(const QString &rel, QList<ListEntry> entries)
    {
        std::sort(entries.begin(), entries.end(), [](const ListEntry &lhs, const ListEntry &rhs) {
            return lhs.name < rhs.name;
        });
        for (const ListEntry &entry : std::as_const(entries)) {
            if (entry.hidden && !m_call.includeHidden) {
                continue;
            }
            if (m_match.match(entry.name).hasMatch() && !take(rel, entry)) {
                return false;
            }
            if (m_call.recursive && entry.isDirectory && !entry.isSymlink
                && !descend(rel + entry.name)) {
                return false;
            }
        }
        return true;
    }

    QString text() const
    {
        if (m_listed.isEmpty() && m_unreadable.isEmpty()) {
            return QStringLiteral("No files found in: %1").arg(m_root);
        }
        QStringList lines = m_listed;
        lines.sort();
        lines.prepend(QStringLiteral("Files in %1:").arg(m_root));
        lines += m_unreadable;
        if (m_truncated) {
            lines += QStringLiteral(
                         "[truncated: listing stopped at %1 entries; narrow the pattern or rerun "
                         "with a larger limit]")
                         .arg(m_call.limit);
        }
        return lines.join(QLatin1Char('\n'));
    }

private:
    bool take(const QString &rel, const ListEntry &entry)
    {
        if (m_listed.size() >= m_call.limit) {
            m_truncated = true;
            return false;
        }
        m_listed += rel + entry.name + (entry.isDirectory ? QStringLiteral("/") : QString());
        return true;
    }

    bool descend(const QString &rel)
    {
        QList<ListEntry> entries;
        QString          error;
        if (!m_listDir(joinPath(m_root, rel), &entries, &error)) {
            m_unreadable += QStringLiteral("[unreadable: %1/: %2]").arg(rel, error);
            return true;
        }
        return visit(rel + QLatin1Char('/'), std::move(entries));
    }

    QString            m_root;
    const ListCall    &m_call;
    const ListDir     &m_listDir;
    QRegularExpression m_match;
    QStringList        m_listed;
    QStringList        m_unreadable;
    bool               m_truncated = false;
};

} // namespace

/* Schemas and descriptions */

json readSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path", pathProperty("Path to the file to read")},
          {"max_lines",
           {{"type", "integer"},
            {"description",
             QStringLiteral("Maximum number of lines to read (default: %1)")
                 .arg(kDefaultMaxLines)
                 .toStdString()}}},
          {"offset",
           {{"type", "integer"},
            {"description", "Line number to start reading from (0-indexed, default: 0)"}}}}},
        {"required", json::array({"file_path"})}};
}

json listSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"directory", pathProperty("Directory to list (default: the working directory)")},
          {"pattern",
           {{"type", "string"},
            {"description", "Glob pattern to filter entries by name (e.g., '*.v', '*.yaml')"}}},
          {"recursive",
           {{"type", "boolean"}, {"description", "List files recursively (default: false)"}}},
          {"include_hidden",
           {{"type", "boolean"}, {"description", "Include hidden files (default: false)"}}},
          {"limit",
           {{"type", "integer"},
            {"description",
             QStringLiteral("Maximum number of entries (default: %1)")
                 .arg(kDefaultListLimit)
                 .toStdString()}}}}},
        {"required", json::array()}};
}

json writeSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path", pathProperty("Path to the file to write")},
          {"content", {{"type", "string"}, {"description", "Content to write to the file"}}}}},
        {"required", json::array({"file_path", "content"})}};
}

json editSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path", pathProperty("Path to the file to edit")},
          {"old_string", {{"type", "string"}, {"description", "The text to replace"}}},
          {"new_string", {{"type", "string"}, {"description", "The replacement text"}}},
          {"replace_all",
           {{"type", "boolean"},
            {"description", "Replace all occurrences (default: false, requires unique match)"}}}}},
        {"required", json::array({"file_path", "old_string", "new_string"})}};
}

QString readDescription(bool image)
{
    QString text = QStringLiteral(
        "Read the contents of a file. Any file on the system can be read, including temporary "
        "files. One read returns at most 16 MiB; page longer files with offset and max_lines.");
    if (image) {
        text += QStringLiteral(
            " Image files (PNG, JPG, GIF, WebP) are returned as visual content that the "
            "multimodal LLM can see directly. When the user attaches or references a screenshot "
            "or image path, ALWAYS use this tool to view the file at the path; the tool result "
            "will contain the actual image, not a description.");
    }
    return text;
}

QString listDescription()
{
    return QStringLiteral(
        "List files in a directory. Any directory on the system can be listed. Directories end "
        "in '/'; a listing cut at the limit says so.");
}

QString writeDescription(const QString &scope)
{
    return QStringLiteral(
               "Write content to a file. Creates the file and its parent directories if missing, "
               "overwrites if it exists. Overwriting an existing file requires reading it first "
               "with read_file; a file changed on disk since it was read is rejected. The file "
               "must be within %1.")
        .arg(scope);
}

QString editDescription(const QString &scope)
{
    return QStringLiteral(
               "Edit a file by replacing a specific string with new content. Read the file with "
               "read_file first: editing an unread file, or one changed on disk since it was "
               "read, is rejected. The old_string must be unique in the file unless replace_all "
               "is set. The file must be within %1.")
        .arg(scope);
}

/* Paths */

QString expandHome(const QString &path, const QString &home)
{
    if (home.isEmpty() || !path.startsWith(QLatin1Char('~'))) {
        return path;
    }
    if (path.size() == 1) {
        return home;
    }
    const QChar next = path.at(1);
    if (next != QLatin1Char('/') && next != QLatin1Char('\\')) {
        return path;
    }
    return joinPath(home, path.mid(2));
}

QString localPath(const QString &raw, const QString &base, const QString &home, bool windows)
{
    static const QRegularExpression posixDrive(QStringLiteral(R"(^/[A-Za-z]:?(/|$))"));
    static const QRegularExpression windowsDrive(QStringLiteral(R"(^[A-Za-z]:(/|$))"));

    QString path = expandHome(raw, home);
    if (windows) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (posixDrive.match(path).hasMatch()) {
            path = QSocShellPath::toWindowsPath(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
        }
    }
    const bool absolute = path.startsWith(QLatin1Char('/'))
                          || (windows && windowsDrive.match(path).hasMatch());
    if (!absolute && !base.isEmpty()) {
        path = path.isEmpty() ? base : joinPath(base, path);
    }
    return QDir::cleanPath(path);
}

/* Shared texts */

QString fileNotFound(const QString &path)
{
    return QStringLiteral("Error: File not found: %1").arg(path);
}

QString notAFile(const QString &path)
{
    return QStringLiteral("Error: Path is not a file: %1").arg(path);
}

QString directoryNotFound(const QString &path)
{
    return QStringLiteral("Error: Directory not found: %1").arg(path);
}

QString wroteText(qsizetype bytes, const QString &path)
{
    return QStringLiteral("Successfully wrote %1 bytes to: %2").arg(bytes).arg(path);
}

QString editedText(const QString &path, int count)
{
    return QStringLiteral("Successfully edited file: %1 (%2 replacement(s))").arg(path).arg(count);
}

QString notReadYet(const QString &path, const QString &action)
{
    return QStringLiteral("Error: File not read yet: %1. Read it with read_file before %2.")
        .arg(path, action);
}

QString changedSinceRead(const QString &path, const QString &action)
{
    return QStringLiteral(
               "Error: File changed on disk since last read: %1. Read it again before %2.")
        .arg(path, action);
}

/* read_file */

ReadWindow readWindow(const json &arguments)
{
    return {
        .offset   = intArg(arguments, "offset", 0, 0),
        .maxLines = intArg(arguments, "max_lines", kDefaultMaxLines, 1),
    };
}

Reader::Reader(const ReadWindow &window, qsizetype limit)
    : m_offset(window.offset)
    , m_maxLines(window.maxLines)
    , m_limit(limit)
{}

bool Reader::feed(const QByteArray &chunk)
{
    if (m_sniffed) {
        return m_mime.isEmpty() ? pageText(chunk) : keepImage(chunk);
    }
    m_head += chunk;
    return m_head.size() < kMagicBytes || sniff();
}

void Reader::finish()
{
    if (!m_sniffed) {
        sniff();
    }
    if (m_mime.isEmpty() && !m_stopped && !m_pending.isEmpty()) {
        m_pending += '\n';
        m_closedLastLine = true;
        takeLine(m_pending);
    }
}

std::optional<QByteArray> Reader::wholeFile() const
{
    if (m_offset != 0 || m_stopped) {
        return std::nullopt;
    }
    return m_closedLastLine ? m_body.chopped(1) : m_body;
}

QString Reader::result(const QString &path, QLLMService *llm) const
{
    const QString limitMiB = QString::number(m_limit / (1024 * 1024));
    if (!m_mime.isEmpty()) {
        if (m_overLimit) {
            return QStringLiteral("Error: image is larger than the %1 MiB read limit: %2")
                .arg(limitMiB, path);
        }
        return QSocImageAttach::buildAttachmentResult(path, m_mime, m_body, llm);
    }
    QString text = QString::fromUtf8(m_body);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QString next = QString::number(m_offset + m_emitted);
    if (text.isEmpty() && m_overLimit) {
        return QStringLiteral(
                   "Error: line %1 of %2 is longer than the %3 MiB read limit; read part of it "
                   "with bash, for example head -c or cut -c")
            .arg(next, path, limitMiB);
    }
    if (text.isEmpty()) {
        return QStringLiteral("File is empty or offset beyond file length: %1").arg(path);
    }
    if (m_overLimit) {
        text += QStringLiteral(
                    "[truncated: read stopped at the %1 MiB limit; rerun with offset=%2 to "
                    "continue]\n")
                    .arg(limitMiB, next);
    } else if (m_moreLines) {
        text += QStringLiteral("[truncated: more lines follow; rerun with offset=%1 to continue]\n")
                    .arg(next);
    }
    return text;
}

bool Reader::sniff()
{
    m_sniffed = true;
    m_mime    = QSocImageAttach::detectMimeByMagic(m_head);
    return feed(std::exchange(m_head, {}));
}

bool Reader::keepImage(const QByteArray &chunk)
{
    m_body += chunk;
    if (m_body.size() > m_limit) {
        return stop(&m_overLimit);
    }
    return true;
}

bool Reader::pageText(const QByteArray &chunk)
{
    m_pending += chunk;
    qsizetype start = 0;
    for (qsizetype nl = m_pending.indexOf('\n'); nl >= 0; nl = m_pending.indexOf('\n', start)) {
        if (!takeLine(m_pending.mid(start, nl + 1 - start))) {
            return false;
        }
        start = nl + 1;
    }
    m_pending.remove(0, start);
    if (m_lineNum < m_offset) {
        m_pending.clear();
        return true;
    }
    if (!m_pending.isEmpty() && m_emitted >= m_maxLines) {
        return stop(&m_moreLines);
    }
    if (m_body.size() + m_pending.size() > m_limit) {
        return stop(&m_overLimit);
    }
    return true;
}

bool Reader::takeLine(const QByteArray &line)
{
    if (m_lineNum < m_offset) {
        ++m_lineNum;
        return true;
    }
    if (m_emitted >= m_maxLines) {
        return stop(&m_moreLines);
    }
    if (m_body.size() + line.size() > m_limit) {
        return stop(&m_overLimit);
    }
    m_body += line;
    ++m_emitted;
    ++m_lineNum;
    return true;
}

bool Reader::stop(bool *reason)
{
    *reason   = true;
    m_stopped = true;
    return false;
}

/* write_file and edit_file */

WriteCall writeCall(const json &arguments)
{
    WriteCall call;
    if (!hasString(arguments, "file_path")) {
        call.error = QStringLiteral("Error: file_path is required");
    } else if (!hasString(arguments, "content")) {
        call.error = QStringLiteral("Error: content is required");
    }
    call.path    = stringArg(arguments, "file_path");
    call.content = stringArg(arguments, "content");
    return call;
}

EditCall editCall(const json &arguments)
{
    EditCall call;
    for (const char *key : {"file_path", "old_string", "new_string"}) {
        if (!hasString(arguments, key)) {
            call.error = QStringLiteral("Error: %1 is required").arg(QString::fromLatin1(key));
            return call;
        }
    }
    call.path       = stringArg(arguments, "file_path");
    call.oldString  = stringArg(arguments, "old_string");
    call.newString  = stringArg(arguments, "new_string");
    call.replaceAll = boolArg(arguments, "replace_all");
    if (call.oldString.isEmpty()) {
        call.error = QStringLiteral("Error: old_string must not be empty");
    } else if (call.oldString == call.newString) {
        call.error = QStringLiteral("Error: old_string and new_string are identical");
    }
    return call;
}

EditOutcome applyEdit(const QString &content, const EditCall &call)
{
    const auto count = static_cast<int>(content.count(call.oldString));
    if (count == 0) {
        return {.error = QStringLiteral("Error: old_string not found in file: %1").arg(call.path)};
    }
    if (!call.replaceAll && count > 1) {
        return {
            .error = QStringLiteral(
                         "Error: old_string found %1 times. Use replace_all=true or provide more "
                         "context for unique match.")
                         .arg(count)};
    }
    QString edited = content;
    if (call.replaceAll) {
        edited.replace(call.oldString, call.newString);
    } else {
        edited.replace(edited.indexOf(call.oldString), call.oldString.size(), call.newString);
    }
    return {.content = edited, .count = count};
}

/* list_files */

ListCall listCall(const json &arguments)
{
    ListCall call;
    call.directory = hasString(arguments, "directory") ? stringArg(arguments, "directory")
                                                       : stringArg(arguments, "directory_path");
    if (const QString pattern = stringArg(arguments, "pattern"); !pattern.isEmpty()) {
        call.pattern = pattern;
    }
    call.recursive     = boolArg(arguments, "recursive");
    call.includeHidden = boolArg(arguments, "include_hidden");
    call.limit         = intArg(arguments, "limit", kDefaultListLimit, 1);
    return call;
}

QString listFiles(
    const QString &root, const ListCall &call, bool caseSensitive, const ListDir &listDir)
{
    QList<ListEntry> entries;
    QString          error;
    if (!listDir(root, &entries, &error)) {
        return QStringLiteral("Error: %1").arg(error);
    }
    Walker walker(root, call, caseSensitive, listDir);
    walker.visit({}, std::move(entries));
    return walker.text();
}

} // namespace QSocToolFileCore
