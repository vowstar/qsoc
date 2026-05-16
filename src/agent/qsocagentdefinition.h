// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTDEFINITION_H
#define QSOCAGENTDEFINITION_H

#include "agent/qsochooktypes.h"

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

    /* Tool name denylist. Applied AFTER `toolsAllow`: deny wins. Use
     * for "inherit everything except these" definitions, or to
     * subtract a few risky tools from a broad allowlist. Lines up
     * with claude-code's `disallowedTools` field (subtract model). */
    QStringList toolsDeny;

    /* Optional model override. Empty = inherit parent's model. */
    QString model;

    /* Per-definition iteration cap. 0 = inherit parent's
     * `maxIterations` (default 100). When > 0 the child stops with a
     * "Reached max turns limit" assistant message after this many
     * agentic iterations, regardless of the parent's higher cap. */
    int maxTurns = 0;

    /* Optional reminder text re-injected as a `system` role message
     * on every LLM turn (not persisted in conversation history).
     * Use for hard-rule reinforcement that should not drift over
     * long runs (e.g. "you are READ-ONLY; refuse any write asks").
     * Empty = no reminder. Mirrors claude-code's
     * `criticalSystemReminder_EXPERIMENTAL`. */
    QString criticalReminder;

    /* Specific skill names to preload into the child's conversation
     * at spawn time. Each name is resolved via the parent's
     * `skill_find` tool; the SKILL.md content is injected as a
     * user-role message in the child's history (capped at 4 KB).
     * Independent from `injectSkills` (which lists EVERY skill in
     * the system prompt — broad context vs. targeted preload). */
    QStringList skills;

    /* Per-definition hook overrides. Empty = inherit the parent
     * agent's hookManager (today's behavior). When non-empty, the
     * spawn tool builds a child-scoped QSocHookManager with these
     * settings and binds it to the child. Declaring a hook for
     * session_start / stop / user_prompt_submit also lifts the
     * sub-agent suppression for those events: the child's hook
     * fires while the parent's still doesn't. */
    QSocHookConfig hooks;

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

    /* Optional host alias the spawn tool should default to when the
     * caller omits the `host` argument. Falls back to the parent's
     * active binding when empty. Resolved against QSocHostCatalog at
     * spawn time; an alias that does not exist in the catalog is
     * ignored with a one-line log line. Frontmatter key:
     * `preferred_host`. */
    QString preferredHost;

    /* Non-empty iff the definition failed to load; surfaced by
     * `/agents` listing for diagnostics. */
    QString parseError;
};

#endif /* QSOCAGENTDEFINITION_H */
