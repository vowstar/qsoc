// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTCONFIG_H
#define QSOCAGENTCONFIG_H

#include <QString>

/**
 * @brief Configuration structure for QSocAgent
 * @details Holds all configuration parameters for the agent's behavior,
 *          including context management, LLM parameters, and output settings.
 */
struct QSocAgentConfig
{
    /* Maximum context tokens before compression */
    int maxContextTokens = 128000;

    /* Layer 1: Tool output pruning */
    double pruneThreshold      = 0.6;   /* 60% triggers pruning */
    int    pruneProtectTokens  = 40000; /* Protect recent 40k tokens of tool output */
    int    pruneMinimumSavings = 20000; /* Minimum savings to justify pruning */

    /* Layer 2: LLM compaction */
    double  compactThreshold = 0.8; /* 80% triggers LLM summary */
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
};

#endif // QSOCAGENTCONFIG_H
