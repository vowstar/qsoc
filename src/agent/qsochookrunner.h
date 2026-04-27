// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHOOKRUNNER_H
#define QSOCHOOKRUNNER_H

#include "agent/qsochooktypes.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>

class QProcess;
class QTimer;

/**
 * @brief Runner for a single hook command.
 * @details Spawns the configured shell command, writes the event JSON
 *          payload on stdin, captures stdout / stderr, and translates
 *          the exit code into a coarse status the manager can act on.
 *          Stdin is closed after the payload is written; the child is
 *          killed if it exceeds the configured timeout.
 *
 *          Exposes both a synchronous `run()` (used by single-shot
 *          callers and unit tests) and an asynchronous start/finished
 *          API the manager uses to fan-out parallel matchers.
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
    ~QSocHookRunner() override;

    /**
     * @brief Run the hook synchronously.
     * @details Internally drives a local event loop until the child
     *          finishes or times out.
     */
    Result run(const HookCommandConfig &cfg, const nlohmann::json &payload);

    /**
     * @brief Start the hook asynchronously.
     * @details Caller must connect `finished()` before this returns
     *          relevant state. After `finished()` fires, `result()`
     *          carries the outcome. Calling start while an earlier run
     *          is in flight is a programming error.
     */
    void start(const HookCommandConfig &cfg, const nlohmann::json &payload);

    /**
     * @brief Latest async outcome. Only meaningful after `finished()`.
     */
    const Result &result() const { return m_result; }

    /**
     * @brief True between `start()` and `finished()`.
     */
    bool isRunning() const { return m_running; }

signals:
    /**
     * @brief Emitted exactly once per `start()` call.
     */
    void finished();

private slots:
    void handleProcessFinished();
    void handleTimeout();

private:
    void   reset();
    void   captureStreams();
    Result interpretExit() const;

    QProcess *m_process = nullptr;
    QTimer   *m_timer   = nullptr;
    Result    m_result;
    int       m_timeoutMs = 0;
    bool      m_running   = false;
    bool      m_finalized = false;
};

#endif // QSOCHOOKRUNNER_H
