// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSHELLPATH_H
#define QSOCSHELLPATH_H

#include <QString>
#include <QStringList>

/**
 * @brief POSIX shell discovery and path normalization across platforms.
 * @details All qsoc features that run a user command string (`bash -c`)
 *          resolve the interpreter through here so the platform rules
 *          live in one place:
 *          - Unix: `/bin/bash`, else `bash` on PATH, else `/bin/sh`.
 *          - Windows: `QSOC_GIT_BASH_PATH` env override, else `bash.exe`
 *            derived from the `git` executable on PATH. `bash` found
 *            directly on PATH is never used: `System32\bash.exe` is the
 *            WSL launcher, not a POSIX shell for the host.
 *          An empty result means no usable shell; callers must fail the
 *          one operation gracefully instead of crashing.
 */
namespace QSocShellPath {

/**
 * @brief Resolve the POSIX shell executable for `-c` command strings.
 * @details The result is cached for the process lifetime. Candidates
 *          located inside the current working directory are rejected so
 *          a repository cannot plant its own `bash`. An explicit
 *          `QSOC_GIT_BASH_PATH` override is trusted but fail-closed:
 *          when set and invalid, the result is empty rather than a
 *          silent fallback to a different interpreter.
 * @return Absolute shell path, or empty when none is usable.
 */
QString bashPath();

/**
 * @brief Compute bash.exe candidates from a git executable location.
 * @details Pure string transform for the Git for Windows layout:
 *          `<root>/cmd/git.exe` yields `<root>/bin/bash.exe` and
 *          `<root>/usr/bin/bash.exe`. No filesystem access.
 * @param gitExePath Absolute path of the git executable.
 * @return Candidate paths in probe order (may not exist).
 */
QStringList gitBashCandidates(const QString &gitExePath);

/**
 * @brief Convert a Windows path to POSIX (MSYS/git-bash) form.
 * @details Pure string transform: `C:\Users\foo` becomes `/c/Users/foo`,
 *          UNC `\\server\share` becomes `//server/share`, remaining
 *          backslashes are flipped. Input already in POSIX form passes
 *          through unchanged.
 * @param path Path in Windows or mixed form.
 * @return Path in POSIX form.
 */
QString toPosixPath(const QString &path);

/**
 * @brief Normalize a path for consumption by the resolved shell.
 * @details On Windows the shell is git-bash, which cannot resolve
 *          `C:\...` paths, so this applies toPosixPath(). On other
 *          platforms it is the identity.
 * @param path Native path.
 * @return Path in the form the shell understands.
 */
QString toShellPath(const QString &path);

/**
 * @brief Reset the cached shell path (test support).
 */
void resetCache();

} // namespace QSocShellPath

#endif // QSOCSHELLPATH_H
