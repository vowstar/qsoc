// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTOPTIONS_H
#define QSOCAGENTOPTIONS_H

#include <QString>

/**
 * @brief Options that select and tune one runtime instance.
 * @details Mirrors the knobs the CLI exposed on `qsoc agent`, minus the
 *          presentation-only ones. Config-file values are still read by
 *          the runtime; these are the caller's overrides.
 */
struct QSocAgentRuntimeOptions
{
    /** Project directory (-d). Empty = auto-detect / cwd. */
    QString projectDirectory;
    /** Client launch context used to generate shell resume hints. */
    QString launchDirectory;
    QString clientProgram = QStringLiteral("qsoc");
    /** Project name (-p). Empty = first available. */
    QString projectName;
    /** Working directory for tool execution (--workspace). Absolute. */
    QString workspace;
    /** SSH target ([user@]host[:port] or ssh-config alias). */
    QString sshTarget;
    /** Defer SSH until frontend interaction handlers are installed. */
    bool deferRemoteConnection = false;
    /** Session id (or unique prefix) to resume; "-" = let the frontend pick. */
    QString resumeSessionId;
    /** Continue the most recent session. */
    bool continueLatestSession = false;
    /** Override max context tokens; 0 = model registry / config default. */
    int maxContextTokens = 0;
    /** Override temperature; negative = config default. */
    double temperature = -1.0;
    /** Override reasoning effort; empty = default, "off"/"low"/"medium"/"high". */
    QString effortLevel;
    /** Override tool presentation: direct | catalog | auto. */
    QString toolPresentation;
    /** Streaming mode (default true). */
    bool streaming = true;
    /** Resolve streaming from session configuration unless explicitly overridden. */
    bool streamingFromConfig = false;
    /** Verbose logging to QSocConsole::debug(). */
    bool verbose = false;
    /** One query, then exit (`-q`): the agent never wakes on its own. */
    bool singleQuery = false;
};

#endif
