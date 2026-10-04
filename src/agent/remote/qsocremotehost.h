// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEHOST_H
#define QSOCREMOTEHOST_H

#include "agent/remote/qsocsshexec.h"
#include "common/qsocshellexecutor.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

/** @brief What one probe learned about a remote host. */
struct QSocRemoteHost
{
    enum class Kind : std::uint8_t {
        Unknown, /**< Neither probe answered. */
        Posix,   /**< `sh -c` answered with markers. */
        Windows, /**< `cmd /c ver` answered. */
    };

    Kind              kind = Kind::Unknown;
    QString           os;              /**< `uname -s`, or "Windows". */
    QString           arch;            /**< `uname -m`. */
    bool              hasProc = false; /**< `/proc/1` exists. */
    QSocShellExecutor shell;           /**< What runs commands; None when nothing does. */
    QString           shellError;      /**< Why a requested shell was not usable. */

    /** @brief One line for the prompt: os, arch and executor. */
    QString summary() const;
};

/** @brief One exec request: the line the login shell parses, and stdin. */
struct QSocRemoteExec
{
    QString    command; /**< Fixed text; carries no caller-supplied bytes. */
    QByteArray input;   /**< Script the interpreter reads from stdin. */

    bool isValid() const { return !command.isEmpty(); }
};

/* Policy: what each kind of host is offered. Kept here so it changes in one
 * place. */

/** @brief Whether bash, bash_manage and monitor are offered for @p host. */
bool remoteHostOffersExecTools(const QSocRemoteHost &host);

/**
 * @brief Whether `!` on @p host runs the raw line in the login shell.
 * @details True for a host with no executor: the line runs as typed, with no
 *          working-directory change, and the result says so.
 */
bool remoteHostShellEscapePassthrough(const QSocRemoteHost &host);

/** @brief Notice that leads a passthrough `!` result. */
QString remoteShellEscapePassthroughNotice();

/**
 * @brief Check a host.yml `shell:` value.
 * @details Accepts empty, `auto`, `bash` or `sh`.
 * @return Empty when valid, else the reason.
 */
QString validateRemoteShellPreference(const QString &preference);

/**
 * @brief The POSIX probe, as a line any login shell parses the same way.
 * @details `sh -c '<script>'` with no `!`, no newline and no single quote
 *          inside, so csh-family history and quoting rules do not apply. The
 *          fields it prints sit between two frame lines.
 */
QString remoteHostProbeCommand();

/** @brief The Windows fallback probe. */
QString remoteWindowsProbeCommand();

/**
 * @brief Turn probe output into a host description.
 * @param posixOut Stdout of @ref remoteHostProbeCommand.
 * @param windowsOut Stdout of @ref remoteWindowsProbeCommand; read only when
 *        @p posixOut carries no marker.
 * @param preference Validated `shell:` value.
 */
QSocRemoteHost parseRemoteHostProbe(
    const QString &posixOut, const QString &windowsOut, const QString &preference);

/** @brief Whether @p posixOut carries a complete probe frame. */
bool remoteProbeAnswered(const QString &posixOut);

/** @brief Why one probe exec gave no answer (timeout, abort, dead link), empty otherwise. */
QString remoteProbeFailure(const QSocSshExec::Result &result);

/** @brief A host the probe could not classify, with @p reason as its shell error. */
QSocRemoteHost unknownRemoteHost(const QString &reason);

/**
 * @brief Run both probes over @p session within @p budgetMs.
 * @details The Windows probe runs only when the POSIX one printed no marker.
 *          A probe that times out or answers nothing usable yields an
 *          unknown host with the reason; the session is left as the exec
 *          left it, and the next adopt probes again.
 */
QSocRemoteHost probeRemoteHost(QSocSshSession *session, const QString &preference, int budgetMs);

/** @brief POSIX single-quote @p value. */
QString remoteShellQuote(const QString &value);

/**
 * @brief Run a POSIX script under the host's executor, script on stdin.
 * @details The exec line names the interpreter, so the host's PATH finds
 *          it. Stdin carries one line that evaluates the quoted script with
 *          stdin from /dev/null and then exits.
 * @param asLogin Pass `-l` when the executor accepts it.
 * @return An invalid request when the host has no executor.
 */
QSocRemoteExec remoteScriptExec(const QSocRemoteHost &host, const QString &script, bool asLogin);

/**
 * @brief Run a user command from @p cwd under the host's login executor.
 * @details `cd -- <cwd> && eval <command>`, so a command that fails to reach
 *          @p cwd never runs.
 * @return An invalid request when the host has no executor.
 */
QSocRemoteExec remoteCommandExec(
    const QSocRemoteHost &host, const QString &cwd, const QString &command);

#endif // QSOCREMOTEHOST_H
