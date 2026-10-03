// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMESSAGEAUTHORITY_H
#define QSOCMESSAGEAUTHORITY_H

#include <nlohmann/json.hpp>
#include <QString>

/**
 * @brief Keep QSoC's runtime markup authoritative.
 * @details QSoC speaks to the model through tags such as
 *          @c <system-reminder>, @c <approved_plan> and
 *          @c <task-notification>. Text that QSoC did not write (tool
 *          output, file contents, peer and sub-agent messages, memory
 *          bodies) must not be able to forge them, so every opening or
 *          closing form of those tags in such text has its @c < replaced
 *          by @c &lt;. The escape is pure and idempotent, so applying it
 *          on every request keeps the request prefix byte-stable.
 */
namespace QSocMessageAuthority {

/**
 * @brief Neutralize QSoC authority tags in untrusted text.
 * @details Matches case-insensitively, accepts @c - or @c _ inside the
 *          tag name and whitespace around the optional @c /.
 */
QString escapeTags(const QString &text);

/**
 * @brief Neutralize QSoC authority tags in UTF-8 text.
 */
std::string escapeTags(const std::string &text);

/**
 * @brief Copy a history message into its on-the-wire form.
 * @details Drops QSoC-internal @c _ keys and escapes authority tags in
 *          tool-result content, for both string and text-part content.
 *          A tool result that had tags escaped gets a QSoC reminder that
 *          the imitation carries no authority.
 */
nlohmann::json toWire(nlohmann::json message);

} // namespace QSocMessageAuthority

#endif // QSOCMESSAGEAUTHORITY_H
