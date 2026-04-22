// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPATHS_H
#define QSOCPATHS_H

#include <QString>
#include <QStringList>

/**
 * @brief Layered resource path resolution for qsoc.
 * @details Four layers, highest to lowest priority:
 *            1. $QSOC_HOME                   — env override (any platform)
 *            2. <projectPath>/.qsoc          — project-local
 *            3. ~/.config/qsoc               — user-level (cross-platform)
 *               (honors $XDG_CONFIG_HOME if set)
 *            4. <platform system root>/qsoc — system-level (platform-native)
 *
 *          System root per platform:
 *            - Linux:   /etc/qsoc
 *            - macOS:   /Library/Application Support/qsoc
 *            - Windows: %PROGRAMDATA%/qsoc
 */
namespace QSocPaths {

/** $QSOC_HOME value or empty string if unset. */
QString envRoot();

/**
 * @brief Project-level root: <projectPath>/.qsoc
 * @param projectPath Project directory (may be empty → returns empty).
 */
QString projectRoot(const QString &projectPath);

/** User-level root: $XDG_CONFIG_HOME/qsoc or ~/.config/qsoc. */
QString userRoot();

/** System-level root, platform-native. */
QString systemRoot();

/**
 * @brief Ordered list of candidate directories for a named subdir.
 * @details Returns non-empty entries in priority order (high → low),
 *          canonicalized and deduplicated.
 * @param subdir Subdirectory name (e.g. "skills", "memory").
 *               Empty string returns the layer roots themselves.
 * @param projectPath Project directory for layer 2; empty skips that layer.
 */
QStringList resourceDirs(const QString &subdir, const QString &projectPath = QString());

} // namespace QSocPaths

#endif // QSOCPATHS_H
