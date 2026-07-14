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
     * frontmatter to save cache tokens on read-only agents. */
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

    /* Selective recall. When enabled, instead of injecting the whole
     * MEMORY.md index into the (cached) system prompt, the agent ranks
     * topic-file headers against each turn's query and injects only the
     * most relevant files as a non-persisted reminder. Keeps the system
     * prompt prefix byte-stable so the provider prompt cache survives.
     * Disabling falls back to full-index injection (legacy behavior). */
    bool    memoryRecallEnabled = true;
    QString memoryRecallModel;              /* Empty = primary model */
    int     memoryRecallMaxFiles   = 5;     /* Max files selected per turn */
    int     memoryRecallPerFileCap = 4096;  /* Max bytes injected per file */
    int     memoryRecallTurnBudget = 61440; /* Max cumulative bytes per turn */

    /* Background extraction. After each turn the REPL forks a constrained
     * child (memory_read / memory_write only) that distills the new
     * conversation slice into memory files, so memory accrues without the
     * main agent having to call memory_write explicitly. */
    bool    memoryExtractEnabled = true;
    QString memoryExtractModel;              /* Empty = primary model */
    int     memoryExtractEveryTurns     = 1; /* Run every N turns */
    int     memoryExtractMinNewMessages = 2; /* Skip trivial turns */

    /* Consolidation ("dream"). Periodically a constrained child merges
     * near-duplicate memories, normalizes dates, drops contradicted facts,
     * and prunes the index. Gated by time + session count and serialized
     * by a lock file. */
    bool    memoryDreamEnabled = true;
    QString memoryDreamModel;            /* Empty = primary model */
    int     memoryDreamMinHours    = 24; /* Min hours between dreams */
    int     memoryDreamMinSessions = 5;  /* Min sessions since last dream */

    /* Auto session title: after the first turn a short title is generated so
     * the resume picker is scannable. A manual /rename always wins. */
    bool    sessionTitleEnabled = true;
    QString sessionTitleModel; /* Empty = primary model */

    /* Away summary ("recap"): when the terminal loses focus (DECSET 1004)
     * for awaySummaryDelaySeconds, a 1-3 sentence "while you were away"
     * recap of the task and the next step is generated and printed once
     * per away period, using the user's configured model (knob may
     * override; empty = primary). */
    bool    awaySummaryEnabled = true;
    QString awaySummaryModel; /* Empty = primary model */
    int     awaySummaryDelaySeconds = 300;

    /* Post-compaction context restore: after a compaction, re-inject the
     * most recently read files (small ones inlined, large ones as a
     * path-only pointer), the recently invoked skills, and any running
     * background agents, so working memory survives the summary swap.
     * Budget/count knobs only; the restore never touches the model. */
    bool contextRestoreEnabled        = true;
    int  contextRestoreMaxFiles       = 5;
    int  contextRestoreFileBudget     = 50000;
    int  contextRestoreMaxTokensFile  = 5000;
    int  contextRestoreMaxTokensSkill = 5000;
    int  contextRestoreSkillBudget    = 25000;

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
     * structured error string. Empty list inherits the parent registry
     * subject to fixed child gates. `agent`, `ask_user`, `goal_complete`, and
     * both plan-mode control tools are always filtered when isSubAgent is true. */
    QStringList toolsAllow;

    /* Tool name denylist. Applied AFTER `toolsAllow` (deny wins).
     * Empty = no extra denies on top of allowlist and fixed child gates. */
    QStringList toolsDeny;

    /* Per-agent iteration cap. 0 = inherit `maxIterations`. When > 0
     * the agent stops with a "Reached max turns limit" message after
     * this many iterations even if `maxIterations` would allow more.
     * Sub-agent definitions set this via the `max_turns:` frontmatter
     * field. */
    int maxTurnsOverride = 0;

    /* Reminder text re-injected as a system message at every LLM
     * turn (in messagesWithSystem only, never persisted to
     * `messages`). Defends against drift on long runs of
     * read-only / verification sub-agents. Empty = no reminder. */
    QString criticalReminder;

    /* Maximum number of sub-agents that may be Running concurrently.
     * 0 (the default) means unbounded: every spawn runs at once and
     * flow control is left to the provider's HTTP 429 backpressure plus
     * the agent loop's exponential backoff. A positive value caps
     * in-flight children and queues the rest; 1 = strict serial. Set it
     * to re-bound for a strict single-key provider. */
    int maxConcurrentSubagents = 0;

    /* A foreground sub-agent that has not finished within this many
     * milliseconds is automatically detached to the background: the
     * spawn tool returns a task_id and the child keeps running, with
     * its terminal result delivered later as a task notification.
     * 0 disables auto-background (the parent waits for the child no
     * matter how long it takes). */
    int autoBackgroundMs = 120000;

    /* Plan mode. When true the agent may only run read-only tools (plus
     * the shell, whose per-command safety is LLM-judged, and the spawn
     * tool, whose children inherit this flag); all mutating tools are
     * hidden from the model and rejected at dispatch. The main agent explores
     * and then calls exit_plan_mode to present a plan for user approval.
     * Spawned sub-agents inherit the gate and return their findings and plan to
     * the parent. */
    bool planMode = false;
};

#endif // QSOCAGENTCONFIG_H
