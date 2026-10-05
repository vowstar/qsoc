// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTERMINALTEXT_H
#define QSOCTERMINALTEXT_H

#include <nlohmann/json_fwd.hpp>

#include <QString>

namespace QSocTerminalText {

/**
 * @brief Remove escape sequences and control characters but tab and newline.
 * @details For text QSoC did not write, so it cannot drive the terminal.
 */
QString plain(const QString &text);

/** @brief The same for every string inside a JSON value. */
nlohmann::json plain(nlohmann::json value);

} // namespace QSocTerminalText

#endif // QSOCTERMINALTEXT_H
