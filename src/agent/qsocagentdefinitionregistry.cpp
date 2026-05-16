// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentdefinitionregistry.h"

#include "agent/remote/qsocsftpclient.h"
#include "common/qsocconsole.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

QSocAgentDefinitionRegistry::QSocAgentDefinitionRegistry(QObject *parent)
    : QObject(parent)
{}

void QSocAgentDefinitionRegistry::registerBuiltins()
{
    QSocAgentDefinition generalPurpose;
    generalPurpose.name        = QStringLiteral("general-purpose");
    generalPurpose.description = QStringLiteral(
        "General-purpose sub-agent for self-contained sub-tasks "
        "(exploration, summarization, focused multi-step work).");
    generalPurpose.scope           = QStringLiteral("builtin");
    generalPurpose.toolsAllow      = QStringList(); /* inherit parent set; spawn tool denied */
    generalPurpose.injectMemory    = false;
    generalPurpose.injectSkills    = false;
    generalPurpose.injectProjectMd = true;
    generalPurpose.promptBody      = QStringLiteral(
        "You are a sub-agent of QSoC's main agent.\n"
        "\n"
        "# Role\n"
        "You execute a single self-contained task delegated by the parent agent and "
        "return ONE concise final answer that the parent will read as a tool result.\n"
        "\n"
        "# Doing the task\n"
        "- Read the parent's prompt carefully and stay strictly within its scope.\n"
        "- Use the available tools to gather facts; do not guess.\n"
        "- When the task is research / exploration, return findings as a short report.\n"
        "- When the task is verification, run the checks and return pass/fail with the\n"
        "  smallest evidence the parent needs.\n"
        "- Do not perform unrelated cleanup, refactoring, or improvements.\n"
        "- You cannot spawn further sub-agents; the `agent` tool is unavailable here.\n"
        "\n"
        "# Output\n"
        "- One final assistant message, terse and direct.\n"
        "- Lead with the answer or conclusion. Append minimal supporting detail.\n"
        "- Cite file paths as `path:line` when referring to code.\n"
        "- No filler, no apology, no restating the question.\n");
    registerDefinition(generalPurpose);

    QSocAgentDefinition explore;
    explore.name        = QStringLiteral("explore");
    explore.description = QStringLiteral(
        "Read-only exploration sub-agent for codebase / project deep-dives. "
        "Returns a concise markdown report with file:line citations.");
    explore.scope      = QStringLiteral("builtin");
    explore.toolsAllow = QStringList{
        QStringLiteral("read_file"),
        QStringLiteral("list_files"),
        QStringLiteral("path_context"),
        QStringLiteral("module_list"),
        QStringLiteral("module_show"),
        QStringLiteral("bus_list"),
        QStringLiteral("bus_show"),
        QStringLiteral("project_list"),
        QStringLiteral("project_show"),
        QStringLiteral("query_docs"),
        QStringLiteral("skill_find"),
        QStringLiteral("lsp"),
        QStringLiteral("memory_read"),
        QStringLiteral("todo_list"),
    };
    explore.injectMemory    = false;
    explore.injectSkills    = false;
    explore.injectProjectMd = true;
    explore.promptBody      = QStringLiteral(
        "You are a read-only exploration sub-agent of QSoC's main agent.\n"
        "\n"
        "# Role\n"
        "Investigate a code area, project structure, or design topic on the parent's "
        "behalf and return a focused markdown report.\n"
        "\n"
        "# Hard limits\n"
        "- You CANNOT modify the workspace: no shell, no file writes, no imports, "
        "no generation, no memory writes. The tool list given to you reflects this.\n"
        "- If the parent asked you to change something, refuse and explain that "
        "exploration is read-only.\n"
        "\n"
        "# How to investigate\n"
        "- The exact tool list available to you is provided separately and may vary\n"
        "  between local and remote workspaces. Pick the right tool from that list.\n"
        "- Start by mapping the surface: file listing, path / project introspection.\n"
        "- Drill into specifics with file reads and language-server lookups.\n"
        "- Use documentation-query tools for clock/reset/power/FSM/bus topics.\n"
        "- Stop as soon as you have enough to answer; do not exhaustively dump files.\n"
        "\n"
        "# Output\n"
        "- A markdown report with section headings.\n"
        "- Cite every code reference as `path:line`.\n"
        "- Lead with a one-line summary; details after.\n"
        "- No speculation. If something is unclear, say so explicitly.\n");
    registerDefinition(explore);

    QSocAgentDefinition verification;
    verification.name        = QStringLiteral("verification");
    verification.description = QStringLiteral(
        "Verification sub-agent for running tests, static checks, and lints. "
        "Reports pass/fail with the smallest evidence; never modifies code.");
    verification.scope      = QStringLiteral("builtin");
    verification.toolsAllow = QStringList{
        QStringLiteral("bash"),
        QStringLiteral("bash_manage"),
        QStringLiteral("read_file"),
        QStringLiteral("list_files"),
        QStringLiteral("path_context"),
        QStringLiteral("lsp"),
        QStringLiteral("query_docs"),
        QStringLiteral("todo_list"),
        QStringLiteral("todo_add"),
        QStringLiteral("todo_update"),
    };
    verification.injectMemory    = false;
    verification.injectSkills    = false;
    verification.injectProjectMd = true;
    verification.promptBody      = QStringLiteral(
        "You are a verification sub-agent of QSoC's main agent.\n"
        "\n"
        "# Role\n"
        "Run tests, static analysis, and lints on the parent's behalf. Report "
        "pass / fail with the minimum evidence the parent needs to act.\n"
        "\n"
        "# Hard limits\n"
        "- You CANNOT change source: no file writes, no imports, no generation, "
        "no memory writes. The tool list given to you reflects this.\n"
        "- Shell access is for verification only: test runners, linters, static "
        "analyzers, build commands. Do not edit files via shell either.\n"
        "- If the parent asked you to fix a failure, refuse and report the "
        "failure with diagnostic detail; fixing belongs to the parent.\n"
        "\n"
        "# How to verify\n"
        "- The exact tool list available to you is provided separately and may\n"
        "  vary between local and remote workspaces; pick from that list.\n"
        "- Use shell for the actual test / build / lint commands (it points at\n"
        "  the right host automatically).\n"
        "- Use language-server diagnostics when available.\n"
        "- Use file-read tools to extract relevant source snippets when reporting.\n"
        "- Track multi-step verification with todo tools when present.\n"
        "\n"
        "# Output\n"
        "- A short pass/fail verdict at the top.\n"
        "- For failures, the smallest reproducible command and the salient "
        "error lines (with `path:line`).\n"
        "- No suggestions for how to fix unless the parent asked.\n");
    registerDefinition(verification);
}

void QSocAgentDefinitionRegistry::registerDefinition(const QSocAgentDefinition &def)
{
    defs_.insert(def.name, def);
}

const QSocAgentDefinition *QSocAgentDefinitionRegistry::find(const QString &name) const
{
    auto found = defs_.constFind(name);
    if (found == defs_.constEnd()) {
        return nullptr;
    }
    return &found.value();
}

QStringList QSocAgentDefinitionRegistry::availableNames() const
{
    QStringList names = defs_.keys();
    std::sort(names.begin(), names.end());
    return names;
}

QString QSocAgentDefinitionRegistry::describeAvailable() const
{
    QString out;
    for (const QString &name : availableNames()) {
        const QSocAgentDefinition *def = find(name);
        if (def == nullptr) {
            continue;
        }
        if (!out.isEmpty()) {
            out += QLatin1Char('\n');
        }
        out += QStringLiteral("- ") + def->name + QStringLiteral(": ") + def->description;
    }
    return out;
}

int QSocAgentDefinitionRegistry::count() const
{
    return static_cast<int>(defs_.size());
}

QList<QSocAgentDefinition> QSocAgentDefinitionRegistry::brokenDefinitions() const
{
    return broken_;
}

void QSocAgentDefinitionRegistry::scanFromDisk(const QString &userDir, const QString &projectDir)
{
    /* User scope first so project scope can shadow it. */
    if (!userDir.isEmpty()) {
        scanDirectory(userDir, QStringLiteral("user"));
    }
    if (!projectDir.isEmpty()) {
        scanDirectory(projectDir, QStringLiteral("project"));
    }
}

void QSocAgentDefinitionRegistry::scanFromRemoteSftp(
    QSocSftpClient *sftp, const QString &remoteDir, const QString &scope)
{
    if (sftp == nullptr || remoteDir.isEmpty()) {
        return;
    }
    QString                            listErr;
    const QList<QSocSftpClient::Entry> entries = sftp->listDir(remoteDir, /*limit*/ 0, &listErr);
    if (!listErr.isEmpty()) {
        QSocConsole::debug() << "agent definitions: remote SFTP list failed for " << remoteDir
                             << ": " << listErr;
        return;
    }
    for (const QSocSftpClient::Entry &entry : entries) {
        if (entry.isDirectory) {
            continue;
        }
        if (!entry.name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString    fullPath = remoteDir + QLatin1Char('/') + entry.name;
        QString          readErr;
        const QByteArray bytes = sftp->readFile(fullPath, /*maxBytes*/ qint64{64} * 1024, &readErr);
        if (!readErr.isEmpty()) {
            QSocConsole::debug() << "agent definitions: remote read failed for " << fullPath << ": "
                                 << readErr;
            continue;
        }
        const QString       content = QString::fromUtf8(bytes);
        QSocAgentDefinition def     = parseAgentMarkdownContent(content, fullPath, scope);
        if (!def.parseError.isEmpty()) {
            broken_.append(def);
            continue;
        }
        registerDefinition(def);
    }
}

void QSocAgentDefinitionRegistry::removeByScope(const QString &scope)
{
    QStringList toErase;
    for (auto it = defs_.cbegin(); it != defs_.cend(); ++it) {
        if (it.value().scope == scope) {
            toErase.append(it.key());
        }
    }
    for (const QString &name : toErase) {
        defs_.remove(name);
    }
    /* Same for broken (parseError) entries. */
    for (int i = broken_.size() - 1; i >= 0; --i) {
        if (broken_[i].scope == scope) {
            broken_.removeAt(i);
        }
    }
}

void QSocAgentDefinitionRegistry::scanDirectory(const QString &dirPath, const QString &scope)
{
    QDir dir(dirPath);
    if (!dir.exists()) {
        QSocConsole::debug() << "agent definitions: " << scope << " dir not present: " << dirPath;
        return;
    }
    const QStringList entries
        = dir.entryList({QStringLiteral("*.md")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QString &name : entries) {
        const QString       path = dir.filePath(name);
        QSocAgentDefinition def  = parseAgentMarkdown(path, scope);
        if (!def.parseError.isEmpty()) {
            broken_.append(def);
            continue;
        }
        registerDefinition(def);
    }
}

namespace {

/* Parse the `tools:` value, supporting two shapes:
 *  1) inline list: `tools: read_file, list_files, bash`
 *  2) YAML list block:
 *       tools:
 *         - read_file
 *         - list_files
 * `inlineValue` is whatever follows the colon on the `tools:` line
 * (already trimmed); `followingLines` is the rest of the frontmatter
 * after that line. The function consumes leading list lines.
 */
QStringList parseToolsField(const QString &inlineValue, const QStringList &followingLines)
{
    QStringList out;
    if (!inlineValue.isEmpty()) {
        const QStringList parts = inlineValue.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty()) {
                out.append(trimmed);
            }
        }
        return out;
    }
    /* Block form: consume leading "- foo" lines. */
    for (const QString &line : followingLines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1Char('-'))) {
            break;
        }
        QString item = trimmed.mid(1).trimmed();
        if (!item.isEmpty()) {
            out.append(item);
        }
    }
    return out;
}

bool parseBoolField(const QString &value, bool fallback)
{
    const QString lower = value.toLower();
    if (lower == QLatin1String("true") || lower == QLatin1String("yes")
        || lower == QLatin1String("1")) {
        return true;
    }
    if (lower == QLatin1String("false") || lower == QLatin1String("no")
        || lower == QLatin1String("0")) {
        return false;
    }
    return fallback;
}

} // namespace

QSocAgentDefinition QSocAgentDefinitionRegistry::parseAgentMarkdown(
    const QString &path, const QString &scope) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QSocAgentDefinition def;
        def.scope      = scope;
        def.sourcePath = path;
        def.parseError = QStringLiteral("cannot open file");
        return def;
    }
    QTextStream   stream(&file);
    const QString content = stream.readAll();
    file.close();
    return parseAgentMarkdownContent(content, path, scope);
}

QSocAgentDefinition QSocAgentDefinitionRegistry::parseAgentMarkdownContent(
    const QString &content, const QString &sourcePath, const QString &scope) const
{
    QSocAgentDefinition def;
    def.scope      = scope;
    def.sourcePath = sourcePath;

    if (!content.startsWith(QStringLiteral("---"))) {
        def.parseError = QStringLiteral("missing frontmatter ('---' on first line)");
        return def;
    }
    const qsizetype endMarker = content.indexOf(QStringLiteral("\n---"), 3);
    if (endMarker < 0) {
        def.parseError = QStringLiteral("frontmatter has no closing '---' line");
        return def;
    }

    const QString     frontmatter = content.mid(4, endMarker - 4);
    const QStringList fmLines     = frontmatter.split(QLatin1Char('\n'));

    /* Body = everything after the closing '---' line. */
    const qsizetype bodyStart = endMarker + 4; /* skip "\n---" */
    QString         body      = content.mid(bodyStart);
    /* Trim a single optional newline immediately after the closing marker. */
    while (body.startsWith(QLatin1Char('\n'))) {
        body.remove(0, 1);
    }
    def.promptBody = body;

    for (qsizetype i = 0; i < fmLines.size(); ++i) {
        const QString  &line     = fmLines[i];
        const qsizetype colonPos = line.indexOf(QLatin1Char(':'));
        if (colonPos < 0) {
            continue;
        }
        const QString key   = line.left(colonPos).trimmed();
        const QString value = line.mid(colonPos + 1).trimmed();

        if (key == QLatin1String("name")) {
            def.name = value;
        } else if (key == QLatin1String("description")) {
            def.description = value;
        } else if (key == QLatin1String("tools")) {
            QStringList rest;
            for (qsizetype j = i + 1; j < fmLines.size(); ++j) {
                rest.append(fmLines[j]);
            }
            def.toolsAllow = parseToolsField(value, rest);
        } else if (key == QLatin1String("disallowed_tools") || key == QLatin1String("disallowedTools")) {
            QStringList rest;
            for (qsizetype j = i + 1; j < fmLines.size(); ++j) {
                rest.append(fmLines[j]);
            }
            def.toolsDeny = parseToolsField(value, rest);
        } else if (key == QLatin1String("skills")) {
            /* Same list shape as `tools` / `disallowed_tools`. */
            QStringList rest;
            for (qsizetype j = i + 1; j < fmLines.size(); ++j) {
                rest.append(fmLines[j]);
            }
            def.skills = parseToolsField(value, rest);
        } else if (key == QLatin1String("hooks")) {
            /* Inline single-level mapping:
             *     hooks:
             *       pre_tool_use: /path/to/cmd.sh
             *       session_start: /path/init.sh
             * Each entry becomes one HookCommandConfig with empty
             * matcher. Block-scalar / nested matcher arrays are not
             * supported by this hand-rolled parser. */
            for (qsizetype j = i + 1; j < fmLines.size(); ++j) {
                const QString &raw = fmLines[j];
                if (raw.isEmpty() || !raw.startsWith(QLatin1Char(' '))) {
                    break;
                }
                const QString   trimmed    = raw.trimmed();
                const qsizetype eventColon = trimmed.indexOf(QLatin1Char(':'));
                if (eventColon < 0) {
                    break;
                }
                const QString eventKey = trimmed.left(eventColon).trimmed();
                const QString eventCmd = trimmed.mid(eventColon + 1).trimmed();
                const auto    parsed   = hookEventFromYamlKey(eventKey);
                if (!parsed.has_value() || eventCmd.isEmpty()) {
                    continue;
                }
                HookCommandConfig command;
                command.command = eventCmd;
                HookMatcherConfig matcher;
                matcher.commands.append(command);
                def.hooks.byEvent[parsed.value()].append(matcher);
            }
        } else if (key == QLatin1String("model")) {
            def.model = value;
        } else if (key == QLatin1String("critical_reminder") || key == QLatin1String("criticalReminder")) {
            /* Inline only; users get multi-line via "\n" escapes
             * (parsed back to real newlines). YAML block-scalar
             * `|` form is not supported by this hand-rolled
             * frontmatter parser. */
            QString unescaped = value;
            unescaped.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            def.criticalReminder = unescaped;
        } else if (key == QLatin1String("max_turns") || key == QLatin1String("maxTurns")) {
            bool      parsedOk = false;
            const int parsed   = value.toInt(&parsedOk);
            if (parsedOk && parsed > 0) {
                def.maxTurns = parsed;
            }
        } else if (key == QLatin1String("inject_memory")) {
            def.injectMemory = parseBoolField(value, def.injectMemory);
        } else if (key == QLatin1String("inject_skills")) {
            def.injectSkills = parseBoolField(value, def.injectSkills);
        } else if (key == QLatin1String("inject_project_md")) {
            def.injectProjectMd = parseBoolField(value, def.injectProjectMd);
        } else if (key == QLatin1String("preferred_host")) {
            def.preferredHost = value;
        }
    }

    /* Default name from the file stem when the frontmatter omits it. */
    if (def.name.isEmpty()) {
        def.name = QFileInfo(sourcePath).completeBaseName();
    }
    if (def.name.isEmpty()) {
        def.parseError = QStringLiteral("missing required field 'name'");
        return def;
    }
    if (def.promptBody.trimmed().isEmpty()) {
        def.parseError = QStringLiteral("body (prompt text after frontmatter) is empty");
        return def;
    }

    return def;
}
