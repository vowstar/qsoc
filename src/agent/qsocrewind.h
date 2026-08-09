// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREWIND_H
#define QSOCREWIND_H

#include "agent/qsocfilehistory.h"

#include <QDateTime>
#include <QString>

#include <nlohmann/json.hpp>

#include <functional>

class QSocSession;

/**
 * @brief One rewind, fully decided before anything is mutated.
 * @details Built from pure reads (the picked message, the mode menu, the
 *          session head) so that every value @ref qsocApplyRewind needs is
 *          already fixed when the first byte is written. A request with
 *          @ref restoreConversation false is a code-only rewind and must
 *          leave the conversation and the snapshot index alone.
 */
struct QSocRewindRequest
{
    bool           restoreConversation = false; /**< Roll the message list back. */
    bool           restoreFiles        = false; /**< Put the working tree back. */
    int            targetSnapshot      = 0;     /**< Snapshot to restore (turn - 1). */
    nlohmann::json keptMessages;                /**< Messages surviving the rewind. */
    QDateTime      originalCreatedAt;           /**< Session createdAt to re-emit. */
};

/**
 * @brief What a rewind attempt did, and what is left to tell the user.
 * @details Refused means nothing was mutated at all. Partial means the rewind
 *          ran but at least one path did not end up where it was asked to,
 *          whether it was refused or deliberately left alone, which is a
 *          different thing from a refusal and must never be reported with the
 *          word "cancelled".
 */
struct QSocRewindResult
{
    /** @brief The three terminal states of a rewind. */
    enum class Outcome : quint8 {
        Refused, /**< Nothing was mutated; the refusal text says why. */
        Done,    /**< Everything asked for happened. */
        Partial, /**< The rewind ran, but some paths did not move. */
    };

    Outcome outcome = Outcome::Refused;   /**< Which terminal state was reached. */
    QString refusal;                      /**< Reason, set only when Refused. */
    int     kept = 0;                     /**< Messages kept; 0 in code-only mode. */
    QSocFileHistory::RestoreReport files; /**< Per-path restore outcome. */
};

/**
 * @brief Asks the workspace whether a file restore may proceed.
 * @details Returns an empty string to allow the rewind, or a user-facing
 *          reason to refuse it. Injected so the decision can be driven from
 *          a test without a transport.
 */
using QSocRewindFileGate = std::function<QString()>;

/**
 * @brief Run one rewind: gate first, then mutate, never the other way round.
 * @details Ordering is the whole point of this function. The gate and the
 *          session rewrite are the only steps that may refuse, and both run
 *          before any state the user can observe has changed, so a refusal
 *          is a genuine no-op. Everything after them commits.
 * @param request What to do; see @ref QSocRewindRequest.
 * @param session Session to rewrite; may be nullptr only for a code-only
 *                request.
 * @param history File history to restore from; may be nullptr, which skips
 *                the gate and the restore.
 * @param gate Workspace gate; an empty callable allows the restore.
 * @return What happened, plus the text inputs @ref qsocRewindReport needs.
 */
QSocRewindResult qsocApplyRewind(
    const QSocRewindRequest  &request,
    QSocSession              *session,
    QSocFileHistory          *history,
    const QSocRewindFileGate &gate);

/**
 * @brief Render exactly one outcome line, plus any per-path detail.
 * @param request The request that produced @p result.
 * @param result What @ref qsocApplyRewind returned.
 * @return Text ready for the compositor, newline-terminated.
 */
QString qsocRewindReport(const QSocRewindRequest &request, const QSocRewindResult &result);

#endif // QSOCREWIND_H
