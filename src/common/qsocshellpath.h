// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSHELLPATH_H
#define QSOCSHELLPATH_H

#include <QHash>
#include <QString>
#include <QStringList>

/**
 * @brief POSIX shell discovery and path normalization across platforms.
 * @details All qsoc features that run a user command string (`bash -c`)
 *          resolve the interpreter through here so the platform rules
 *          live in one place:
 *          - Unix: `/bin/bash`, else `bash` on PATH, else `/bin/sh`.
 *          - Windows: `QSOC_GIT_BASH_PATH` env override, else
 *            @ref windowsGitBashCandidates. `bash` found
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
 * @brief bash.exe candidates under the standard Git for Windows install roots.
 * @details Pure: reads only @p env. Roots, in order: `GIT_INSTALL_ROOT`,
 *          `ProgramFiles\Git`, `ProgramFiles(x86)\Git`,
 *          `LOCALAPPDATA\Programs\Git`, and scoop's `apps\git\current`
 *          under `SCOOP` or `USERPROFILE\scoop`. Each yields
 *          `bin/bash.exe` and `usr/bin/bash.exe`.
 * @param env Environment variable values by name.
 * @return Candidate paths with forward slashes (may not exist).
 */
QStringList gitBashInstallCandidates(const QHash<QString, QString> &env);

/**
 * @brief Convert a Windows path to POSIX (MSYS/git-bash) form.
 * @details Pure string transform: `C:\Users\foo` and the SFTP form
 *          `/C:/Users/foo` become `/c/Users/foo`,
 *          UNC `\\server\share` becomes `//server/share`, remaining
 *          backslashes are flipped. Input already in POSIX form passes
 *          through unchanged.
 * @param path Path in Windows or mixed form.
 * @return Path in POSIX form.
 */
QString toPosixPath(const QString &path);

/**
 * @brief Convert an MSYS, SFTP or mixed Windows path to Windows form.
 * @details Pure string transform: `/c/x`, `/C:/x` and `C:/x` become `C:\x`.
 *          Meant for paths known to be on a Windows machine.
 */
QString toWindowsPath(const QString &path);

/** @brief The SFTP form of a Windows path: `C:\x` and `/c/x` become `/C:/x`. */
QString toSftpPath(const QString &path);

/** @brief Whether @p path is an absolute Windows path to a `bash.exe`. */
bool isWindowsBashPath(const QString &path);

/**
 * @brief Every Git Bash candidate for a Windows machine, in probe order.
 * @details The one list for the local machine and for a remote host. A
 *          non-empty @p override is the only candidate. Otherwise the
 *          layout around @p gitExePath (@ref gitBashCandidates), then the
 *          install roots in @p env (@ref gitBashInstallCandidates). Never a
 *          `bash.exe` from PATH or System32 (the WSL launcher).
 * @param override `QSOC_GIT_BASH_PATH` locally, host.yml `shell:` remotely.
 * @param gitExePath Located git executable, or empty.
 * @param env Environment values by name.
 */
QStringList windowsGitBashCandidates(
    const QString &override, const QString &gitExePath, const QHash<QString, QString> &env);

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
 * @brief The raw argument string for `cmd.exe` running @p command.
 * @details `/d /s /c "<command>"`: AutoRun is skipped, and `/s` makes cmd
 *          strip only the outer quotes, so it sees @p command verbatim.
 *          Meant for QProcess::setNativeArguments, which adds no quoting.
 */
QString cmdExeNativeArguments(const QString &command);

/**
 * @brief Decode a shell command's output, the same rule for every machine.
 * @details UTF-8 when the bytes are valid UTF-8. Otherwise the OEM code
 *          page on Windows, the code page of a child started in its own
 *          console, and the local 8-bit encoding elsewhere.
 */
QString decodeConsoleOutput(const QByteArray &bytes);

/**
 * @brief Reset the cached shell path (test support).
 */
void resetCache();

} // namespace QSocShellPath

#endif // QSOCSHELLPATH_H
