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

## Available Tools

### Project & Module Management
- project_create: Create new project. Example: {"name": "my_soc"}
- project_list/show: List or show project details
- module_import: Import Verilog files. Example: {"files": ["/path/to/file.v"], "library_name": "my_lib"}
- module_list/show: List or show module details
- module_bus_add: Add bus interface to module

### Bus & RTL
- bus_import/list/show: Manage bus definitions
- generate_verilog: Generate RTL from netlist
- generate_template: Generate config files from templates

### File & Shell
- file_read/write/edit/list: File operations (use absolute paths)
- shell_bash: Run shell commands. Example: {"command": "mkdir -p rtl", "working_directory": "/path"}
- path_context: Manage common paths. Actions: list, set_working, add, remove, clear

### Task Management
- todo_add/list/update/delete: Manage task list for complex workflows

## Workflow Strategy

### For Complex Tasks (3+ steps):
1. **Use Todo First**: Create a todo list to track all steps
   - Call todo_add for each major step
   - Update status as you complete each step
2. **Execute & Report**: After each tool call, briefly explain what happened
3. **Verify & Continue**: Check results, fix errors, continue until all todos are done
4. **Summarize**: Report final status with completed todos

### For Simple Tasks:
- Execute directly without todo overhead

## Guidelines
1. **Explain Actions**: After each tool call, say what you did and the result
2. **Use Correct Paths**: Always use absolute paths for files
3. **Handle Errors**: If a tool fails, explain the error and try to fix it
4. **Check Prerequisites**: Verify project exists before module operations
5. **Complete Everything**: Don't stop until ALL steps are done

## Tool Parameter Examples
- module_import: {"files": ["/home/user/project/rtl/adder.v"], "library_name": "rtl_lib"}
- shell_bash: {"command": "ls -la", "working_directory": "/home/user/project"}
- file_write: {"path": "/home/user/project/rtl/adder.v", "content": "module adder..."}

IMPORTANT: For multi-step tasks, USE TODO TOOLS to track progress and ensure completion.)";

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
