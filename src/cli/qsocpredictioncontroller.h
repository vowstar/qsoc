// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPREDICTIONCONTROLLER_H
#define QSOCPREDICTIONCONTROLLER_H

#include "common/qllmservice.h"

#include <QObject>
#include <QString>

/**
 * @brief Predicts the user's next input and offers it as ghost text.
 * @details After an agent turn completes the REPL calls requestPrediction()
 *          with the conversation. The controller asks an isolated (cloned)
 *          QLLMService, on its own QNAM, for one short line the user is
 *          likely to type next, filters the result, and emits ghostReady().
 *          The async path never blocks: it uses sendRequestAsync, so the
 *          input monitor's raw-mode event loop is never re-entered.
 *
 *          Cancellation is a generation token rather than a network abort:
 *          sendRequestAsync exposes no reply handle, so cancel() bumps the
 *          token and any in-flight callback whose captured token no longer
 *          matches is dropped. The cloned QNAM makes the orphaned reply
 *          harmless to the live agent stream.
 */
class QSocPredictionController : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Construct a controller that clones @p mainLlm for predictions.
     * @param parent  Owning QObject (also parents the cloned service).
     * @param mainLlm The live agent LLM service; cloned for isolation. May be
     *                nullptr, in which case prediction is permanently disabled.
     */
    explicit QSocPredictionController(QObject *parent, const QLLMService *mainLlm);

    /** @brief Enable or disable prediction (config toggle). */
    void setEnabled(bool value);
    bool isEnabled() const { return enabled; }

    /**
     * @brief Request a prediction for the next user input.
     * @details Applies suppression guards (disabled, no service, in-flight,
     *          fewer than two assistant turns, no new turn since last request,
     *          last turn errored) and only then fires an async request. Safe
     *          to call every prompt-loop iteration; redundant calls are
     *          deduplicated by assistant-message count.
     * @param messages OpenAI-format conversation from QSocAgent::getMessages().
     */
    void requestPrediction(const json &messages);

    /**
     * @brief Invalidate any in-flight request and clear the current ghost.
     * @details Bumps the generation token (dropping a pending callback) and,
     *          if a ghost was showing, emits ghostCleared(). Called on the
     *          first keystroke, on accept, on agent start, abort, and exit.
     */
    void cancel();

    /** @brief Mark that the just-finished turn errored; skip one prediction. */
    void markError() { lastTurnError = true; }

    bool    hasGhost() const { return !ghost.isEmpty(); }
    QString currentGhost() const { return ghost; }

    /**
     * @brief Filter a raw model line; true means reject (not a usable input).
     * @details Static and side-effect free so unit tests can exercise it
     *          directly without an LLM. Rejects empty, over-long, evaluative,
     *          assistant-voice, interrogative, multi-sentence, and
     *          out-of-range-word-count lines (single words must be allowlisted).
     */
    static bool shouldFilter(const QString &line);

signals:
    /** @brief A filtered prediction is ready to show as ghost text. */
    void ghostReady(const QString &text);
    /** @brief The ghost was cleared; the REPL should hide it. */
    void ghostCleared();

private:
    /** @brief Flatten the last few turns into a compact role-tagged prompt. */
    static QString buildTranscript(const json &messages);
    /** @brief Count assistant messages in the conversation. */
    static int assistantTurnCount(const json &messages);

    QLLMService *llm                = nullptr; /* Cloned service, own QNAM */
    bool         enabled            = true;
    bool         inFlight           = false;
    bool         lastTurnError      = false;
    quint64      generation         = 0;  /* Cancellation token */
    int          lastPredictedTurns = -1; /* Dedup by assistant-turn count */
    QString      ghost;
};

#endif // QSOCPREDICTIONCONTROLLER_H
