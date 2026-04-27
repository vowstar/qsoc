// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHOOKMANAGER_H
#define QSOCHOOKMANAGER_H

#include "agent/qsochookrunner.h"
#include "agent/qsochooktypes.h"

#include <nlohmann/json.hpp>

#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief Dispatches lifecycle events to user-configured hook commands.
 * @details Owns a `QSocHookConfig` parsed from `agent.hooks` and, when
 *          `fire()` is called for an event, picks the matchers whose
 *          pattern matches the supplied subject (tool name, event
 *          name, etc.), spawns every matched command in parallel,
 *          awaits all of them, and aggregates the outcome into a
 *          single `Outcome` struct.
 *
 *          Aggregation rules:
 *          - any matched hook returning Block flips `blocked = true`
 *          - the block reason is sourced from the response JSON's
 *            `reason` field, the hook stderr, or a generic fallback
 *          - the last hook response that is a JSON object becomes
 *            `mergedResponse` (for events that consume it, like
 *            PreToolUse honoring `updatedInput`)
 *          - all per-hook results are kept in `rawResults` for the
 *            caller to log or surface.
 */
class QSocHookManager : public QObject
{
    Q_OBJECT

public:
    struct Outcome
    {
        bool                          blocked = false;
        QString                       blockReason;
        nlohmann::json                mergedResponse;
        bool                          hasMergedResponse = false;
        QList<QSocHookRunner::Result> rawResults;
    };

    explicit QSocHookManager(QObject *parent = nullptr);

    void setConfig(const QSocHookConfig &config);
    bool hasHooksFor(QSocHookEvent event) const;

    /**
     * @brief Fire an event synchronously.
     * @param event Lifecycle event being raised.
     * @param matchSubject String tested against each matcher pattern
     *                     (tool name for tool-related events; the event
     *                     key for events without a natural subject).
     * @param payload JSON sent on each hook's stdin.
     */
    Outcome fire(QSocHookEvent event, const QString &matchSubject, const nlohmann::json &payload);

    /**
     * @brief Test whether a pattern matches a subject.
     * @details
     *          - Empty / `*` → always matches
     *          - All chars are alphanumeric/underscore plus optional
     *            `|` separators → exact-or-pipe match
     *          - Otherwise → regular expression (anchored full match).
     *            An invalid regex fails closed (returns false).
     */
    static bool matches(const QString &pattern, const QString &subject);

private:
    QSocHookConfig m_config;
};

#endif // QSOCHOOKMANAGER_H
