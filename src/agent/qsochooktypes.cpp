// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochooktypes.h"

#include "common/qsocconsole.h"

namespace {

/* Same gating discipline as qsocmcptypes: yaml-cpp will throw on
 * Type-querying methods called against an undefined node, so always
 * IsDefined() before IsScalar()/IsMap()/IsSequence(). */

QString yamlScalarToQString(const YAML::Node &node)
{
    if (!node.IsDefined() || !node.IsScalar()) {
        return {};
    }
    try {
        return QString::fromStdString(node.as<std::string>());
    } catch (const YAML::Exception &) {
        return {};
    }
}

int yamlIntOrDefault(const YAML::Node &node, int defaultValue)
{
    if (!node.IsDefined() || !node.IsScalar()) {
        return defaultValue;
    }
    try {
        return node.as<int>();
    } catch (const YAML::Exception &) {
        return defaultValue;
    }
}

QList<HookCommandConfig> parseCommandList(const YAML::Node &commands, const QString &eventLabel)
{
    QList<HookCommandConfig> out;
    if (!commands.IsDefined() || !commands.IsSequence()) {
        return out;
    }
    for (const auto &entry : commands) {
        if (!entry.IsMap()) {
            QSocConsole::warn() << "Skipping non-map hook entry under" << eventLabel;
            continue;
        }
        HookCommandConfig cfg;
        const QString     parsedType = yamlScalarToQString(entry["type"]);
        if (!parsedType.isEmpty()) {
            cfg.type = parsedType;
        }
        cfg.command    = yamlScalarToQString(entry["command"]);
        cfg.timeoutSec = yamlIntOrDefault(entry["timeout"], cfg.timeoutSec);

        if (!cfg.isValid()) {
            QSocConsole::warn() << "Skipping invalid hook entry under" << eventLabel
                                << "(type=" << cfg.type << ")";
            continue;
        }
        out << cfg;
    }
    return out;
}

QList<HookMatcherConfig> parseMatcherList(const YAML::Node &matchers, const QString &eventLabel)
{
    QList<HookMatcherConfig> out;
    if (!matchers.IsDefined() || !matchers.IsSequence()) {
        return out;
    }
    for (const auto &entry : matchers) {
        if (!entry.IsMap()) {
            QSocConsole::warn() << "Skipping non-map matcher under" << eventLabel;
            continue;
        }
        HookMatcherConfig group;
        group.matcher  = yamlScalarToQString(entry["matcher"]);
        group.commands = parseCommandList(entry["hooks"], eventLabel);
        if (group.commands.isEmpty()) {
            continue;
        }
        out << group;
    }
    return out;
}

} // namespace

QString hookEventToYamlKey(QSocHookEvent event)
{
    switch (event) {
    case QSocHookEvent::PreToolUse:
        return QStringLiteral("pre_tool_use");
    case QSocHookEvent::PostToolUse:
        return QStringLiteral("post_tool_use");
    case QSocHookEvent::UserPromptSubmit:
        return QStringLiteral("user_prompt_submit");
    case QSocHookEvent::SessionStart:
        return QStringLiteral("session_start");
    case QSocHookEvent::Stop:
        return QStringLiteral("stop");
    }
    return {};
}

std::optional<QSocHookEvent> hookEventFromYamlKey(const QString &key)
{
    if (key == QStringLiteral("pre_tool_use")) {
        return QSocHookEvent::PreToolUse;
    }
    if (key == QStringLiteral("post_tool_use")) {
        return QSocHookEvent::PostToolUse;
    }
    if (key == QStringLiteral("user_prompt_submit")) {
        return QSocHookEvent::UserPromptSubmit;
    }
    if (key == QStringLiteral("session_start")) {
        return QSocHookEvent::SessionStart;
    }
    if (key == QStringLiteral("stop")) {
        return QSocHookEvent::Stop;
    }
    return std::nullopt;
}

QList<QSocHookEvent> allHookEvents()
{
    return {
        QSocHookEvent::PreToolUse,
        QSocHookEvent::PostToolUse,
        QSocHookEvent::UserPromptSubmit,
        QSocHookEvent::SessionStart,
        QSocHookEvent::Stop,
    };
}

bool HookCommandConfig::isValid() const
{
    if (type == QStringLiteral("command")) {
        return !command.isEmpty();
    }
    return false;
}

bool QSocHookConfig::isEmpty() const
{
    for (auto it = byEvent.constBegin(); it != byEvent.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            return false;
        }
    }
    return true;
}

int QSocHookConfig::totalCommands() const
{
    int count = 0;
    for (auto it = byEvent.constBegin(); it != byEvent.constEnd(); ++it) {
        for (const auto &group : it.value()) {
            count += static_cast<int>(group.commands.size());
        }
    }
    return count;
}

QList<HookMatcherConfig> QSocHookConfig::matchersFor(QSocHookEvent event) const
{
    return byEvent.value(event);
}

QSocHookConfig QSocHookConfig::parseFromYaml(const YAML::Node &agentHooks)
{
    QSocHookConfig out;
    if (!agentHooks.IsDefined() || agentHooks.IsNull() || !agentHooks.IsMap()) {
        return out;
    }
    for (const auto &entry : agentHooks) {
        const QString rawKey = yamlScalarToQString(entry.first);
        const auto    event  = hookEventFromYamlKey(rawKey);
        if (!event.has_value()) {
            QSocConsole::warn() << "Skipping unknown hook event:" << rawKey;
            continue;
        }
        const auto matchers = parseMatcherList(entry.second, rawKey);
        if (!matchers.isEmpty()) {
            out.byEvent.insert(*event, matchers);
        }
    }
    return out;
}
