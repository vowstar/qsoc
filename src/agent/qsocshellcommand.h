// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSHELLCOMMAND_H
#define QSOCSHELLCOMMAND_H

#include "common/qsoctoolresultstatus.h"

#include <nlohmann/json.hpp>
#include <QString>

/**
 * @brief What the local and the remote bash tools share: schemas and texts.
 * @details One definition, so a model sees one dialect whichever machine the
 *          command runs on. Only the id of a still-running command differs:
 *          a local process id is a number, a remote job id a string.
 */
namespace QSocShellCommand {

constexpr int kDefaultTimeoutMs   = 60000;
constexpr int kDefaultOutputLines = 200;
constexpr int kLastOutputLines    = 50;
constexpr int kTerminateGraceMs   = 5000;

/** @brief How one command ended. */
struct Outcome
{
    QSocToolResultStatus status   = QSocToolResultStatus::Uncertain;
    int                  exitCode = -1; /**< -1 when the exit status is unknown. */
    QString              exitSignal;    /**< Signal name without SIG, e.g. "KILL". */
    bool                 timedOut      = false;
    bool                 aborted       = false;
    bool                 transportDead = false;
    QString              error;
    QString              output; /**< stdout and stderr, merged. */
};

/** @brief Header lines (status, exit_code, ...) and then the output. */
QString resultText(const Outcome &outcome);

/** @brief Who still runs a command the call stopped waiting for. */
struct Handle
{
    QString idKey;   /**< bash_manage argument, "process_id" or "job_id". */
    QString idLabel; /**< "Process ID" or "Job ID". */
    QString id;
    QString outputFile;
    QString lastOutput;
};

/** @brief The command outlived @p timeoutMs and keeps running. */
QString timedOutText(int timeoutMs, const Handle &handle);

/** @brief An abort could not stop the command. */
QString stillRunningAfterAbortText(const Handle &handle);

/** @brief The command was stopped by an abort. */
QString abortedText();

/** @brief Signal name of a signal number, e.g. 9 -> "KILL"; the number if unknown. */
QString signalName(int signal);

/** @brief Timeout argument in ms: `timeout`, else the `timeout_ms` alias, else @p fallback. */
int timeoutArgument(const nlohmann::json &arguments, int fallback = kDefaultTimeoutMs);

/** @brief `max_lines` argument, else @p fallback. */
int maxLinesArgument(const nlohmann::json &arguments, int fallback = kDefaultOutputLines);

/** @brief Parameters of bash, local and remote. */
nlohmann::json bashSchema();

/**
 * @brief Parameters of bash_manage.
 * @param idKey Name of the id argument.
 * @param idType JSON type of the id argument.
 */
nlohmann::json bashManageSchema(const char *idKey, const char *idType);

} // namespace QSocShellCommand

#endif // QSOCSHELLCOMMAND_H
