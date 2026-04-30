// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTCONFIG_H
#define QSOCAGENTCONFIG_H

#include "agent/qsochooktypes.h"

#include <QString>
#include <QStringList>

/**
 * @brief Configuration structure for QSocAgent
 * @details Holds all configuration parameters for the agent's behavior,
 *          including context management, LLM parameters, and output settings.
 */
struct QSocAgentConfig
{
    /* Maximum context tokens before compression. The model window covers
     * input + output combined, so the effective input budget is
     * maxContextTokens - reservedOutputTokens. Threshold math should run
     * against the effective figure, not the raw window. */
    int maxContextTokens     = 128000;
    int reservedOutputTokens = 16384; /* Reserved for the assistant reply */

    /* Layer 1: Tool output pruning. Threshold sits well below the model
     * window so the weighted token estimator's ~20% error margin still
     * leaves comfortable headroom before the model rejects the request. */
    double pruneThreshold      = 0.4;   /* 40% triggers pruning */
    int    pruneProtectTokens  = 40000; /* Protect recent 40k tokens of tool output */
    int    pruneMinimumSavings = 20000; /* Minimum savings to justify pruning */

    /* Layer 2: LLM compaction */
    double  compactThreshold = 0.6; /* 60% triggers LLM summary */
    QString compactionModel;        /* Empty = use primary model */

    /* Number of recent messages to keep during compression */
    int keepRecentMessages = 10;

    /* LLM temperature parameter (0.0-1.0) */
    double temperature = 0.2;

    /* Reasoning effort level: empty=off, "low", "medium", "high" */
    QString effortLevel;

    /* Reasoning model: empty=use primary model when effort is set */
    QString reasoningModel;

    /* Project instructions (AGENTS.md / AGENTS.local.md) */
    QString projectPath; /* Set by parseAgent; empty = no project instructions loaded */

    /* Whether to inject the project AGENTS.md / AGENTS.local.md
     * section into the system prompt. Default true. Sub-agent
     * definitions can opt out with `inject_project_md: false` in
     * frontmatter to save cache tokens on read-only agents
     * (claude-code's `omitClaudeMd`). */
    bool injectProjectMd = true;

    /* Pre-built skill listing for system prompt injection. Populated by
     * the REPL at startup from scanAllSkills(); empty = no skills found. */
    QString skillListing;

    /* Current model ID, injected into the environment section so the LLM
     * knows which model it is running as. Set by the REPL from
     * llmService->getCurrentModelId(). */
    QString modelId;

    /* Memory injection settings */
    bool autoLoadMemory = true;  /* Auto-inject memory into system prompt */
    int  memoryMaxChars = 24000; /* Max chars (~6000 tokens) for memory in prompt */

    /* Enable verbose output */
    bool verbose = true;

    /* System prompt override. When empty (default), buildSystemPromptWithMemory()
     * composes the full prompt from modular sections. When non-empty, the
     * literal string is used as the base prompt (useful for testing or
     * custom deployments). */
    QString systemPromptOverride;

    /* Maximum iterations for safety */
    int maxIterations = 100;

    /* Stuck detection settings */
    bool enableStuckDetection  = true;
    bool autoStatusCheck       = true;
    int  stuckThresholdSeconds = 60;

    /* Retry settings */
    int maxRetries = 3; /* Maximum retry attempts for timeout/network errors */

    /* Remote workspace state (populated by the REPL when a remote target
     * is active). When remoteMode is true the agent system prompt declares
     * the remote workspace and the tool registry swaps file/shell/path
     * implementations for SSH/SFTP-backed versions. */
    bool        remoteMode = false;
    QString     remoteName;       /* Profile name or raw "user@host:port". */
    QString     remoteDisplay;    /* Human-readable target, no credentials. */
    QString     remoteWorkspace;  /* Absolute remote workspace root. */
    QString     remoteWorkingDir; /* Absolute remote cwd (initially = workspace). */
    QStringList remoteWritableDirs;

    /* User-defined lifecycle hooks parsed from `agent.hooks` in
     * .qsoc.yml / qsoc.yml. Hooks always run on the local host, even
     * in remoteMode; the JSON payload includes a `remote` section so
     * scripts can branch on it. */
    QSocHookConfig hooks;

    /* Sub-agent flag. When true the agent runs as a child of another
     * agent: session_start / stop / user_prompt_submit hooks are
     * skipped (parent already fired them), the spawn-agent tool is
     * never exposed (no recursion), and buildSystemPromptWithMemory
     * treats systemPromptOverride as the identity section to which
     * environment / project / skills / memory are still appended. */
    bool isSubAgent = false;

    /* Tool name allowlist. When non-empty only these tools are exposed
     * to the LLM and accepted at dispatch; out-of-list calls return a
     * structured error string. Empty list = inherit the parent
     * registry's full tool set. The `agent` spawn tool is always
     * filtered out when isSubAgent is true, regardless of this list. */
    QStringList toolsAllow;

    /* Tool name denylist. Applied AFTER `toolsAllow` (deny wins).
     * Empty = no extra denies on top of allowlist + recursion guard. */
    QStringList toolsDeny;

    /* Per-agent iteration cap. 0 = inherit `maxIterations`. When > 0
     * the agent stops with a "Reached max turns limit" message after
     * this many iterations even if `maxIterations` would allow more.
     * Sub-agent definitions set this via the `max_turns:` frontmatter
     * field; mirrors claude-code's `maxTurns`. */
    int maxTurnsOverride = 0;

    /* Reminder text re-injected as a system message at every LLM
     * turn (in messagesWithSystem only, never persisted to
     * `messages`). Defends against drift on long runs of
     * read-only / verification sub-agents. Empty = no reminder. */
    QString criticalReminder;

    /* Maximum number of sub-agents that may be Running concurrently.
     * Read by the spawn tool when deciding whether to admit a new
     * spawn. The cap protects per-API-key RPM limits at remote
     * providers. 1 = strict serial (legacy behavior). */
    int maxConcurrentSubagents = 4;
};

#endif // QSOCAGENTCONFIG_H
