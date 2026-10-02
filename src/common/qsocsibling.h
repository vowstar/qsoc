// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSIBLING_H
#define QSOCSIBLING_H

#include <QString>

/**
 * @brief Locate the programs qsoc ships next to its own executable.
 * @details The directory is QSOC_BIN_DIR when set, else the directory of
 *          the running executable. On macOS the programs may also sit in
 *          the QSoC.app or qsoc-gui.app bundle beside it, as in a build tree.
 */
namespace QSocSibling {

/** Absolute path of @p name (no suffix), or empty when it is missing. */
QString path(const QString &name);

/** User-facing error for a missing sibling program. */
QString missingMessage(const QString &name);

} // namespace QSocSibling

#endif // QSOCSIBLING_H
