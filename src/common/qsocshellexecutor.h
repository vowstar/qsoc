// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSHELLEXECUTOR_H
#define QSOCSHELLEXECUTOR_H

#include <QString>

#include <cstdint>
#include <functional>

/**
 * @brief The interpreter that runs command strings for bash-type tools.
 * @details One representation for the local machine and for a remote host:
 *          what it is, where it lives and whether it accepts `-l`.
 */
struct QSocShellExecutor
{
    enum class Kind : std::uint8_t {
        None,    /**< No POSIX shell: shell tools are not offered. */
        Bash,    /**< GNU bash. */
        Sh,      /**< A POSIX sh that is not bash. */
        GitBash, /**< Git for Windows bash (MSYS). */
    };

    Kind    kind = Kind::None;
    QString path;          /**< Absolute interpreter path; empty for None. */
    QString launch;        /**< Name a command line runs it by; @ref path when empty. */
    QString version;       /**< Version string, empty when unknown. */
    bool    login = false; /**< Accepts `-l`. */

    bool available() const { return kind != Kind::None && !path.isEmpty(); }

    /** @brief `<launch>` or `<launch> -l`, for a command line or a nested shell. */
    QString invocation(bool asLogin) const;

    /** @brief The Environment `Shell:` value, e.g. `bash 5.2.37(1)-release`. */
    QString summary() const;
};

/** @brief Version token of a `bash --version` first line, or the line itself. */
QString parseBashVersion(const QString &firstLine);

/**
 * @brief Classify an interpreter by its file name.
 * @param path Absolute interpreter path.
 * @param windows Whether the path is a Windows executable.
 */
QSocShellExecutor::Kind classifyShellPath(const QString &path, bool windows);

/**
 * @brief The executor the local bash tool uses.
 * @details Resolved through QSocShellPath::bashPath() and cached with it.
 *          A resolver installed with @ref setLocalShellResolver replaces the
 *          lookup, so tests can describe any kind without that host.
 */
QSocShellExecutor localShellExecutor();

/** @brief Replace the local lookup; an empty function restores it. */
void setLocalShellResolver(std::function<QSocShellExecutor()> resolver);

#endif // QSOCSHELLEXECUTOR_H
