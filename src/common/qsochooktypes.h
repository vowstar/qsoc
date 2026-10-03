// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHOOKTYPES_H
#define QSOCHOOKTYPES_H

#include <yaml-cpp/yaml.h>

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

/**
 * @brief Lifecycle events at which the agent can fire user-defined
 *        hook commands. The minimal set covers the points where users
 *        most often want to inject policy: tool dispatch (pre/post),
 *        prompt admission, and session start/end.
 */
enum class QSocHookEvent {
    PreToolUse,
    PostToolUse,
    UserPromptSubmit,
    SessionStart,
    Stop,
};

/**
 * @brief Map an event enum value to the snake_case key used in YAML
 *        and in the JSON payload sent to hook stdin.
 */
QString hookEventToYamlKey(QSocHookEvent event);

/**
 * @brief Parse a YAML/JSON snake_case event key.
 * @return The event on success, std::nullopt on unknown keys.
 */
std::optional<QSocHookEvent> hookEventFromYamlKey(const QString &key);

/**
 * @brief Full list of supported events, in declaration order.
 */
QList<QSocHookEvent> allHookEvents();

/**
 * @brief A single hook command entry, taken from the inner `hooks:`
 *        array under a matcher.
 * @details Only `type: command` is supported in the MVP. The `command`
 *          string is handed to `/bin/sh -c` (mirrors QSocToolShellBash);
 *          the agent passes the event payload on stdin as JSON and
 *          interprets stdout/exit codes when the child finishes.
 */
struct HookCommandConfig
{
    QString type{QStringLiteral("command")};
    QString command;
    int     timeoutSec = 10;

    /**
     * @brief Quick structural validation.
     * @return True when `type` is a known kind and the kind-specific
     *         required fields are present.
     */
    bool isValid() const;
};

/**
 * @brief A matcher group: zero-or-empty matcher means "always", any
 *        matching event payload triggers the inner command list.
 */
struct HookMatcherConfig
{
    QString                  matcher;
    QList<HookCommandConfig> commands;
};

/**
 * @brief Top-level hook configuration parsed from `agent.hooks`.
 * @details Keys at the YAML level are snake_case event names, values
 *          are arrays of matcher groups. Invalid entries are dropped
 *          with a warning so a single typo does not lock the user out
 *          of the rest of the config.
 */
struct QSocHookConfig
{
    QHash<QSocHookEvent, QList<HookMatcherConfig>> byEvent;

    bool                     isEmpty() const;
    int                      totalCommands() const;
    QList<HookMatcherConfig> matchersFor(QSocHookEvent event) const;

    /**
     * @brief Parse the YAML map under `agent.hooks` into a structured
     *        config. Undefined / non-map nodes return an empty config.
     */
    static QSocHookConfig parseFromYaml(const YAML::Node &agentHooks);
};

#endif // QSOCHOOKTYPES_H
