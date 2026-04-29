// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTDEFINITION_H
#define QSOCAGENTDEFINITION_H

#include <QString>
#include <QStringList>

/**
 * @brief Definition of a sub-agent type.
 * @details Describes a child agent the parent can spawn through the
 *          `agent` tool: prompt body becomes the child's system
 *          prompt, toolsAllow restricts the child's tool set, and
 *          inject* flags gate dynamic context (memory, skills) that
 *          the parent normally embeds. Built-in definitions are
 *          compiled in; user / project markdown loading is reserved
 *          for a future scan-from-disk path.
 */
struct QSocAgentDefinition
{
    /* Slug used as the `subagent_type` enum value when calling the
     * spawn tool. Must be unique within the registry. */
    QString name;

    /* One-line human description. Surfaced in the tool schema so the
     * parent LLM can pick the right type. */
    QString description;

    /* Full system prompt for the child. Replaces the static identity
     * sections; environment / project instructions / optional skills /
     * memory are still appended by buildSystemPromptWithMemory. */
    QString promptBody;

    /* Tool name allowlist for the child. Empty = inherit the parent
     * registry's full set (with the spawn-agent tool always filtered
     * out for recursion safety). */
    QStringList toolsAllow;

    /* Optional model override. Empty = inherit parent's model. */
    QString model;

    /* Provenance: "builtin" today; "user" / "project" reserved for
     * markdown discovery in a future iteration. */
    QString scope = QStringLiteral("builtin");

    /* Source path for non-builtin scopes (empty for builtins). */
    QString sourcePath;

    /* Inject parent memory snapshot into the child's prompt. Default
     * off: sub-tasks should run on a clean context unless the
     * definition explicitly opts in. */
    bool injectMemory = false;

    /* Inject the parent's skill listing into the child's prompt.
     * Default off for the same reason as memory. */
    bool injectSkills = false;

    /* Inject AGENTS.md / AGENTS.local.md from the project root.
     * Default on: project-level rules (testing isolation, commit
     * conventions) apply to children too. */
    bool injectProjectMd = true;

    /* Non-empty iff the definition failed to load; surfaced by
     * `/agents` listing for diagnostics. */
    QString parseError;
};

#endif /* QSOCAGENTDEFINITION_H */
