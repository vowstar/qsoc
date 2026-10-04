// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRIVATEFILE_H
#define QSOCPRIVATEFILE_H

#include <QFileDevice>
#include <QString>

/**
 * @brief Owner-only modes for agent data (sessions, artifacts, sub-agent runs).
 * @details Files get 0600 and directories 0700 regardless of the umask. An
 *          existing entry with looser bits is tightened when it is written.
 *          Tightening is best effort: a file system without Unix modes
 *          keeps working. Symbolic links are never followed. On Windows
 *          the mode calls are no-ops.
 */
namespace QSocPrivateFile {

/** @brief Restrict an open file (or a QSaveFile temporary) to 0600. */
void restrict(QFileDevice &file);

/** @brief Restrict a path to 0600 (file) or 0700 (directory). */
void restrict(const QString &path);

/** @brief Create a directory with its parents, restrict the leaf to 0700. */
bool makeDir(const QString &path);

} // namespace QSocPrivateFile

#endif // QSOCPRIVATEFILE_H
