// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMESSAGEMARKUP_H
#define QSOCMESSAGEMARKUP_H

#include "agent/protocol/qsocagentruntimeevent.h"

#include <nlohmann/json.hpp>

#include <QList>
#include <QString>

#include <optional>

/**
 * @brief The `!` line result stored as a user-role history message.
 * @details The block is built and read here only, so the runtime that shows
 *          it live and the replay that shows it again raise the same events.
 *          Command and result are HTML-escaped, so text in them cannot close
 *          the block or open a QSoC tag.
 */
namespace QSocShellCommandMessage {

/** @brief Result text: exit code, duration, then the output with CRLF as LF. */
QString resultText(std::optional<int> exitCode, qint64 durationMs, const QString &output);

/**
 * @brief History message for one `!` line.
 * @param forged The raw text imitated QSoC tags; a reminder follows the block.
 */
nlohmann::json message(const QString &command, const QString &result, bool forged);

/** @brief Whether a history message is a `!` line result. */
bool isShellCommand(const nlohmann::json &message);

struct Parsed
{
    QString command;
    QString result;
};

/** @brief Command and result of a `!` message, unescaped. */
std::optional<Parsed> parse(const nlohmann::json &message);

/** @brief Hint shown after a `!` result that entered the conversation. */
QString recordedHint();

/** @brief Display events: the typed line, the result, then the hint if any. */
QList<QSocAgentRuntimeEvent> events(const QString &typed, const QString &result, const QString &hint);

/** @brief Display events of a stored `!` message; empty when it does not parse. */
QList<QSocAgentRuntimeEvent> events(const nlohmann::json &message);

} // namespace QSocShellCommandMessage

/**
 * @brief The display line of a task notification.
 */
namespace QSocTaskNotificationText {

struct Fields
{
    QString taskId;
    QString source;
    QString kind;
    QString status;
    QString agentType;
    QString summary;
    QString content;
};

/** @brief One display line built from the fields. */
QString summaryLine(const Fields &fields);

/** @brief Fields of a @c <task-notification> envelope, unescaped. */
std::optional<Fields> parse(const QString &notification);

} // namespace QSocTaskNotificationText

#endif // QSOCMESSAGEMARKUP_H
