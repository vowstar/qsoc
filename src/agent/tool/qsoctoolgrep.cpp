// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolgrep.h"
#include "agent/qsocworkspacefs.h"

#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <algorithm>

namespace {

constexpr qint64 kFileBytes   = 2 * 1024 * 1024;
constexpr qint64 kScanBytes   = 32 * 1024 * 1024;
constexpr int    kEntries     = 10000;
constexpr int    kFiles       = 512;
constexpr int    kDepth       = 16;
constexpr int    kOutputBytes = 120 * 1024;
constexpr int    kLineBytes   = 64 * 1024;
constexpr int    kScanMs      = 10000;

json lineResult(const QByteArray &line, int number)
{
    QString text = QString::fromUtf8(line);
    if (text.endsWith('\r'))
        text.chop(1);
    const bool clipped = text.size() > 512;
    if (clipped) {
        text.truncate(512);
        if (!text.isEmpty() && text.at(text.size() - 1).isHighSurrogate())
            text.chop(1);
    }
    return {{"line", number}, {"text", text.toStdString()}, {"text_truncated", clipped}};
}

json matchResult(const QString &path, const QList<QByteArray> &lines, int index, int context)
{
    json result         = lineResult(lines[index], index + 1);
    result["path"]      = path.toStdString();
    result["before"]    = json::array();
    result["after"]     = json::array();
    const int lineCount = static_cast<int>(lines.size());
    for (int i = std::max(0, index - context); i < index; ++i)
        result["before"].push_back(lineResult(lines[i], i + 1));
    for (int i = index + 1; i < std::min(lineCount, index + context + 1); ++i)
        result["after"].push_back(lineResult(lines[i], i + 1));
    return result;
}

struct GrepQuery
{
    QString            path;
    QString            glob;
    int                context       = 0;
    int                maxResults    = 50;
    bool               includeHidden = false;
    QRegularExpression expression;
    QRegularExpression filter;
};

QString parseQuery(const json &arguments, GrepQuery *query)
{
    QString pattern;
    bool    ignoreCase;
    bool    literal;
    try {
        pattern              = QString::fromStdString(arguments.at("pattern").get<std::string>());
        query->path          = QString::fromStdString(arguments.value("path", std::string(".")));
        query->glob          = QString::fromStdString(arguments.value("glob", std::string()));
        query->context       = arguments.value("context", 0);
        query->maxResults    = arguments.value("max_results", 50);
        query->includeHidden = arguments.value("include_hidden", false);
        ignoreCase           = arguments.value("ignore_case", false);
        literal              = arguments.value("literal", false);
    } catch (const json::exception &) {
        return QStringLiteral("Error: invalid grep arguments");
    }
    if (!QSocWorkspaceFs::isContainedRelative(query->path))
        return QStringLiteral("Error: path must stay within the bound workspace");
    if (pattern.isEmpty() || pattern.size() > 1024 || query->glob.size() > 1024
        || query->context < 0 || query->context > 5 || query->maxResults < 1
        || query->maxResults > 500)
        return QStringLiteral("Error: pattern, context or result limit is out of range");
    const auto options = ignoreCase ? QRegularExpression::CaseInsensitiveOption
                                    : QRegularExpression::NoPatternOption;
    query->expression  = QRegularExpression(
        QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)")
            + (literal ? QRegularExpression::escape(pattern) : pattern),
        options);
    if (!query->expression.isValid())
        return QStringLiteral("Error: invalid search regular expression");
    query->filter = QRegularExpression(QRegularExpression::wildcardToRegularExpression(query->glob));
    return {};
}

struct Pending
{
    QString                path;
    int                    depth;
    QSocWorkspaceFs::Entry entry;
};

struct SearchState
{
    json          matches = json::array();
    QElapsedTimer timer;
    qint64        bytesScanned = 0;
    int           entries      = 0;
    int           files        = 0;
    int           skipped      = 0;
    int           skippedLines = 0;
    int           outputBytes  = 1024;
    bool          truncated    = false;
    bool          stopped      = false;
    QString       error;

    bool alive(const QPointer<QSocToolCallContext> &call)
    {
        if (call && call->isCancellationRequested()) {
            error   = QStringLiteral("Error: search aborted");
            stopped = true;
        }
        if (timer.elapsed() >= kScanMs)
            stopped = truncated = true;
        return !stopped;
    }
};

void appendDirectory(
    QSocWorkspaceFs *fs,
    const Pending   &item,
    const GrepQuery &query,
    QList<Pending>  *pending,
    SearchState     *state)
{
    if (item.entry.symlink)
        return;
    if (item.depth >= kDepth) {
        state->truncated = true;
        return;
    }
    const int remaining = kEntries - state->entries - static_cast<int>(pending->size());
    if (remaining <= 0) {
        state->stopped = state->truncated = true;
        return;
    }
    QList<QSocWorkspaceFs::Entry> children;
    QString                       error;
    bool                          truncated = false;
    if (fs->listLimited(item.path, remaining, &children, &truncated, &error)
        != QSocWorkspaceFs::Result::Ok) {
        ++state->skipped;
        state->truncated = true;
        return;
    }
    state->truncated = state->truncated || truncated;
    std::sort(children.begin(), children.end(), [](const auto &left, const auto &right) {
        return left.name < right.name;
    });
    for (auto child = children.crbegin(); child != children.crend(); ++child) {
        if (child->name == "." || child->name == ".." || child->name.contains('/')
            || child->name.contains('\\'))
            continue;
        if (!query.includeHidden && child->name.startsWith('.'))
            continue;
        if (pending->size() + state->entries >= kEntries) {
            state->truncated = true;
            break;
        }
        pending->append({QDir::cleanPath(item.path + '/' + child->name), item.depth + 1, *child});
    }
}

void searchFile(
    QSocWorkspaceFs                     *fs,
    const Pending                       &item,
    const GrepQuery                     &query,
    const QPointer<QSocToolCallContext> &call,
    SearchState                         *state)
{
    const QString fileName   = item.path.section('/', -1);
    const QString filterPath = query.glob.contains('/') ? item.path : fileName;
    if (!query.glob.isEmpty() && !query.filter.match(filterPath).hasMatch())
        return;
    if (item.entry.size < 0 || item.entry.size > kFileBytes) {
        ++state->skipped;
        state->truncated = true;
        return;
    }
    if (item.entry.size > kScanBytes - state->bytesScanned) {
        state->stopped = state->truncated = true;
        return;
    }
    QByteArray bytes;
    QString    error;
    if (fs->readLimited(
            item.path, std::min(kFileBytes, kScanBytes - state->bytesScanned), &bytes, &error)
        != QSocWorkspaceFs::Result::Ok) {
        ++state->skipped;
        state->truncated = true;
        return;
    }
    ++state->files;
    state->bytesScanned += bytes.size();
    if (bytes.contains('\0')) {
        ++state->skipped;
        return;
    }
    auto lines = bytes.split('\n');
    if (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    for (int index = 0; index < lines.size() && state->alive(call); ++index) {
        if (lines[index].size() > kLineBytes) {
            ++state->skippedLines;
            state->truncated = true;
            continue;
        }
        const auto found = query.expression.match(QString::fromUtf8(lines[index]));
        if (!found.isValid()) {
            state->error = QStringLiteral(
                "Error: search regular expression exceeded its execution limits");
            state->stopped = true;
            return;
        }
        if (!found.hasMatch())
            continue;
        json       match      = matchResult(item.path, lines, index, query.context);
        const auto matchBytes = match.dump().size();
        if (matchBytes > static_cast<std::size_t>(kOutputBytes - state->outputBytes)) {
            state->stopped = state->truncated = true;
            return;
        }
        state->outputBytes += static_cast<int>(matchBytes);
        state->matches.push_back(std::move(match));
        if (state->matches.size() >= static_cast<std::size_t>(query.maxResults))
            state->stopped = state->truncated = true;
    }
}

} // namespace

QSocToolGrep::QSocToolGrep(QObject *parent, QSocWorkspaceFs *fs)
    : QSocTool(parent)
    , fs_(fs)
{}

QString QSocToolGrep::getName() const
{
    return QStringLiteral("grep");
}

QString QSocToolGrep::getDescription() const
{
    return QStringLiteral(
        "Search text in the bound workspace, with paths, line numbers and optional context. "
        "Use glob to filter file names or relative paths. Files over 2 MiB and binary files "
        "are skipped. A call scans at most 512 files, 10000 entries and 32 MiB. "
        "A 10-second scan budget is checked between workspace operations. Remote I/O uses "
        "the connection transport timeouts. "
        "Inspect truncated and skipped counts before treating no matches as exhaustive.");
}

json QSocToolGrep::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"pattern",
           {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 1024},
            {"description", "Regular expression or literal text"}}},
          {"path",
           {{"type", "string"},
            {"description", "Workspace-relative file or directory (default: .)"}}},
          {"glob",
           {{"type", "string"},
            {"description", "Optional file glob, for example *.sv or rtl/*.sv"}}},
          {"literal",
           {{"type", "boolean"},
            {"description", "Match literal text instead of a regular expression"}}},
          {"ignore_case", {{"type", "boolean"}}},
          {"include_hidden", {{"type", "boolean"}}},
          {"context",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 5},
            {"description", "Lines before and after each match (default: 0)"}}},
          {"max_results",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 500},
            {"description", "Maximum matching lines (default: 50)"}}}}},
        {"required", json::array({"pattern"})}};
}

QString QSocToolGrep::execute(const json &arguments)
{
    QSocWorkspaceFs *const fs = fs_;
    if (!fs || fs->root().isEmpty())
        return QStringLiteral("Error: no workspace is bound");
    GrepQuery     query;
    const QString invalid = parseQuery(arguments, &query);
    if (!invalid.isEmpty())
        return invalid;
    QSocWorkspaceFs::Entry start;
    QString                error;
    if (fs->stat(query.path, &start, &error) != QSocWorkspaceFs::Result::Ok)
        return QStringLiteral("Error: search path is unavailable: %1").arg(error);
    const QString                       identity = fs->identity();
    const QPointer<QSocToolCallContext> call(currentCallContext());
    QList<Pending>                      pending{{query.path, 0, start}};
    SearchState                         state;
    state.timer.start();
    while (!pending.isEmpty() && state.alive(call)) {
        if (fs_ != fs || fs->identity() != identity)
            return QStringLiteral("Error: workspace changed during search");
        if (state.entries >= kEntries || state.files >= kFiles) {
            state.truncated = true;
            break;
        }
        const Pending item = pending.takeLast();
        ++state.entries;
        if (item.entry.regular)
            searchFile(fs, item, query, call, &state);
        else
            appendDirectory(fs, item, query, &pending, &state);
        /* Workspace I/O can re-enter and change the active binding. */
        // cppcheck-suppress knownConditionTrueFalse
        if (fs_ != fs || fs->identity() != identity)
            return QStringLiteral("Error: workspace changed during search");
    }
    if (!state.error.isEmpty())
        return state.error;
    const json result
        = {{"matches", state.matches},
           {"files_searched", state.files},
           {"entries_scanned", state.entries},
           {"bytes_scanned", state.bytesScanned},
           {"skipped_files", state.skipped},
           {"skipped_lines", state.skippedLines},
           {"truncated", state.truncated}};
    return QString::fromStdString(result.dump());
}

#include "moc_qsoctoolgrep.cpp"
