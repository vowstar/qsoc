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
