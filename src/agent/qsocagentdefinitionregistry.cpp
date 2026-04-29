// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentdefinitionregistry.h"

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
