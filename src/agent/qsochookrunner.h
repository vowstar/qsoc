// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHOOKRUNNER_H
#define QSOCHOOKRUNNER_H

#include "agent/qsochooktypes.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>

/**
 * @brief Synchronous runner for a single hook command.
 * @details Spawns the configured shell command, writes the event JSON
 *          payload on stdin, captures stdout / stderr, and translates
 *          the exit code into a coarse status the manager can act on.
 *          Stdin is closed after the payload is written; the child is
 *          killed if it exceeds the configured timeout. The runner
 *          owns no persistent state and is safe to re-use for the
 *          next event.
 */
class QSocHookRunner : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Success,          /* exit 0 */
        Block,            /* exit 2 */
        NonBlockingError, /* any other non-zero exit */
        Timeout,
        StartFailed,
    };

    struct Result
    {
        Status         status   = Status::Success;
        int            exitCode = 0;
        QString        stdoutText;
        QString        stderrText;
        nlohmann::json response; /* First stdout line if JSON */
        bool           hasResponse = false;
        QString        errorMessage; /* StartFailed / Timeout detail */
    };

    explicit QSocHookRunner(QObject *parent = nullptr);

    /**
     * @brief Run the hook synchronously.
     * @param cfg Command spec (type, command line, timeout).
     * @param payload Event payload, serialized to JSON and written to stdin.
     * @return Outcome with captured streams and parsed response.
     */
    Result run(const HookCommandConfig &cfg, const nlohmann::json &payload);
};

#endif // QSOCHOOKRUNNER_H
