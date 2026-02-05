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

    /* Threshold ratio for triggering history compression (0.0-1.0) */
    double compressionThreshold = 0.8;

    /* Number of recent messages to keep during compression */
    int keepRecentMessages = 10;

    /* LLM temperature parameter (0.0-1.0) */
    double temperature = 0.2;

    /* Enable verbose output */
    bool verbose = true;

    /* System prompt for the agent */
    QString systemPrompt
        = R"(You are QSoC Agent, an AI assistant for System-on-Chip design automation.

Available tools:
- Project management (create, list, show)
- Module management (import Verilog, list, show, add bus interfaces)
- Bus management (import, list, show)
- RTL generation (Verilog, templates)
- Documentation queries

Guidelines:
1. Use query_docs to look up format specifications when needed
2. Verify project exists before operating on modules
3. Report errors clearly with suggested fixes
4. Break complex tasks into steps
5. Always explain what you're doing and why)";

    /* Maximum iterations for safety */
    int maxIterations = 100;
};

#endif // QSOCAGENTCONFIG_H
