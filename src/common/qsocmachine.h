// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMACHINE_H
#define QSOCMACHINE_H

#include "common/qsocshellexecutor.h"

#include <QString>

#include <cstdint>

/**
 * @brief One machine that runs commands: the local one or an SSH host.
 * @details The same rules apply to both. Locally it is filled from the
 *          build platform and @ref localShellExecutor; remotely from the
 *          host probe.
 */
struct QSocMachine
{
    enum class Kind : std::uint8_t {
        Unknown, /**< The probe could not classify the machine. */
        Posix,   /**< A POSIX system. */
        Windows, /**< Windows, including a host whose `uname -s` is MINGW, MSYS or CYGWIN. */
    };

    /** @brief The shell that parses a command line the machine is handed. */
    enum class LoginShell : std::uint8_t {
        Unknown,    /**< Not known; no fixed launcher can be built. */
        Posix,      /**< A POSIX-family shell (sh, bash, csh, fish, ...). */
        Cmd,        /**< cmd.exe. */
        PowerShell, /**< Windows PowerShell or pwsh. */
    };

    Kind              kind       = Kind::Unknown;
    LoginShell        loginShell = LoginShell::Unknown;
    QString           os;              /**< `uname -s`, a product name, or "Windows". */
    QString           arch;            /**< `uname -m` or `PROCESSOR_ARCHITECTURE`. */
    bool              hasProc = false; /**< `/proc/1` exists. */
    QSocShellExecutor shell;           /**< What runs commands; None when nothing does. */
    QString           shellError;      /**< Why no executor is offered. */
    QString           rootFrom;        /**< Workspace root as the file tools spell it. */
    QString           rootTo; /**< The same root as the executor spells it, when it differs. */

    /** @brief One line for logs: os, arch and executor. */
    QString summary() const;
};

/** @brief How `!` runs a line on a machine. */
enum class QSocShellEscapeMode : std::uint8_t {
    Executor,    /**< Through the machine's POSIX executor. */
    Cmd,         /**< Through cmd.exe, verbatim. */
    Passthrough, /**< As typed in the login shell, with no directory change. */
};

/** @brief Whether bash, bash_manage and monitor are offered on @p machine. */
bool machineOffersExecTools(const QSocMachine &machine);

/** @brief How `!` runs on @p machine. */
QSocShellEscapeMode machineShellEscapeMode(const QSocMachine &machine);

/** @brief What a `!` result names as the shell that ran it. */
QString shellEscapeShellName(const QSocMachine &machine);

/** @brief The local machine: build platform, product name, arch and executor. */
QSocMachine localMachine();

/**
 * @brief The Environment lines for @p machine.
 * @details `- OS`, `- Arch`, `- Shell`, `- Executor`, then the Git Bash rule
 *          block when the executor is Git Bash. Local and remote share it.
 */
QString machineEnvironmentLines(const QSocMachine &machine, bool remote);

/** @brief Rules for a model that drives Git Bash on Windows. */
QString gitBashGuidance();

/**
 * @brief @p path as the machine's executor spells it.
 * @details Identity on POSIX. On Windows, the Git Bash form (`/c/x`) of an
 *          SFTP (`/C:/x`) or Windows (`C:\x`) path, using the verified
 *          workspace root mapping when the path lies under it.
 */
QString machineShellPath(const QSocMachine &machine, const QString &path);

/**
 * @brief Escape @p line for one pass of cmd.exe parsing.
 * @details Puts `^` before each `& | < > ^ ( )` outside double quotes, so the
 *          parsing cmd hands the line on unchanged. `%` cannot be escaped on
 *          a command line; a defined `%NAME%` is expanded once more.
 */
QString cmdCaretEscape(const QString &line);

/**
 * @brief The fixed line a Windows login shell parses to start @p exe.
 * @details cmd: `""<exe>" <args>"`. PowerShell: `& '<exe>' <args>`.
 *          Empty for any other login shell.
 * @param exe Windows path of the program.
 * @param args Fixed arguments, never user text.
 */
QString windowsLauncher(QSocMachine::LoginShell login, const QString &exe, const QString &args);

/**
 * @brief The line a Windows login shell parses to run @p command in cmd.exe.
 * @details `cmd /d /s /c "cd /d "<cwd>" && <command>"`, caret-escaped for a
 *          cmd login shell, or after `cmd --%` for PowerShell. Empty for any
 *          other login shell.
 * @param cwd Windows path, or empty to keep the login directory.
 */
QString windowsShellEscapeLine(
    QSocMachine::LoginShell login, const QString &cwd, const QString &command);

#endif // QSOCMACHINE_H
